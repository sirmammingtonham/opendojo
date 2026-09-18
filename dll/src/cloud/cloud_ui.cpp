#include "ui/text_buffers.hpp"
#include "cloud/cloud_ui.hpp"

#include "imgui.h"
#include "ui/table_widgets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cloud/api.hpp"
#include "cloud/cloud.hpp"
#include "cloud/handle.hpp"
#include "cloud/worker.hpp"
#include "commands.hpp"
#include "hooks/render_hook.hpp"
#include "log.hpp"
#include "players.hpp"
#include "game_thread.hpp"
#include "ui/menu.hpp"

namespace opendojo::cloud::ui {

namespace {

// ---- Taxonomy. Mirrors the seed data in the SQL migration. If you
// add an entry to drill_categories or drill_difficulties, mirror it
// here AND in the Edge Function's ALLOWED_* sets.
struct Category {
    const char* id;
    const char* label;
};
constexpr Category kCategories[] = {
    {"reaction", "Reaction"},       {"option_select", "Option Select"},
    {"fuzzy_guard", "Fuzzy Guard"}, {"punishment", "Punishment"},
    {"throw_break", "Throw Break"}, {"other", "Other"},
};
constexpr int kMaxDrillTags = 5;  // matches the upload/update validators
constexpr int kInlineTags = 3;
constexpr int kCategoryCount = static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));

void drill_tag_checkbox(int index, bool* picks) {
    int selected = 0;
    for (int i = 0; i < kCategoryCount; ++i)
        selected += picks[i];
    const bool at_limit = !picks[index] && selected >= kMaxDrillTags;
    if (at_limit) ImGui::BeginDisabled();
    ImGui::Checkbox(kCategories[index].label, &picks[index]);
    if (at_limit) ImGui::EndDisabled();
    if (at_limit && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Choose up to %d tags. Uncheck one to choose another.", kMaxDrillTags);
}

struct Difficulty {
    const char* id;
    const char* label;
};
constexpr Difficulty kDifficulties[] = {
    {"beginner", "Beginner"},
    {"intermediate", "Intermediate"},
    {"advanced", "Advanced"},
};
constexpr int kDifficultyCount = static_cast<int>(sizeof(kDifficulties) / sizeof(kDifficulties[0]));

// Combo labels — entries are 1:1 with kDifficulties plus an "any/none"
// slot at index 0. The Browse filter uses these; Upload uses
// kUploadDifficultyLabels which has "(none)" instead of "Any".
const char* kDifficultyFilterLabels[] = {"Any", "Beginner", "Intermediate", "Advanced"};
const char* kUploadDifficultyLabels[] = {"(none)", "Beginner", "Intermediate", "Advanced"};

const char* category_label(const std::string& id) {
    for (const auto& c : kCategories) {
        if (id == c.id) return c.label;
    }
    return id.c_str();  // unknown: surface the raw id rather than hide
}

const char* difficulty_label(const std::string& id) {
    for (const auto& d : kDifficulties) {
        if (id == d.id) return d.label;
    }
    return id.c_str();
}

ImVec4 difficulty_color(const std::string& id) {
    if (id == "beginner") return ImVec4(0.55f, 0.95f, 0.65f, 1);      // green
    if (id == "intermediate") return ImVec4(1.00f, 0.85f, 0.40f, 1);  // amber
    if (id == "advanced") return ImVec4(1.00f, 0.55f, 0.40f, 1);      // red
    return ImVec4(0.65f, 0.65f, 0.65f, 1);
}

// Destructive variant of ImGui::Button. Duplicated here so cloud_ui
// doesn't need a public helper out of menu.cpp; menu's copy lives
// in that file's anon namespace.
bool destructive_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// Center the next popup on screen (the menu sits at the viewport center, so
// this lands the dialog over it). Call right before BeginPopupModal.
//
// We use ImGuiCond_Always, not Appearing: an AlwaysAutoResize modal is hidden
// for one frame while ImGui measures its size, and the Appearing pivot-center
// doesn't reliably re-apply on the following (sized) frame — so those modals
// stuck to the top-left. Always re-centers every frame, which lands correctly
// once the size is known. The only cost is the dialog can't be dragged
// off-center, which is the right behavior for a modal anyway.
void center_next_modal_on_menu() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

// Character-combo indices we use internally:
//   0      = "All characters" (no filter)
//   1..N   = explicit roster pick — index into players::character_roster()
// On first render the combo is auto-selected to the detected CPU's
// roster index (or "All" if no detection); the user can change it
// like any other dropdown. We don't surface a separate "current CPU"
// synthetic option because the Export tab already shows the live CPU
// character right above this section.
constexpr int kCharComboAll = 0;
constexpr int kCharComboRosterBase = 1;

// Cached copy of the sorted character list. Built once on first call;
// keeps draw_browse_tab from hitting the loop in character_roster()
// every frame. The list only changes if the DLL is rebuilt with new
// ids in players::character_name_internal.
const std::vector<std::string>& roster() {
    static const std::vector<std::string> g_roster = opendojo::players::character_roster();
    return g_roster;
}

// Cloud tab has two modes selected via the pill toggle at the top.
// Browse = community drills, full filter row. MyUploads = just the
// caller's uploads, with Edit/Delete row actions instead of Like/Download.
enum class Mode {
    Browse,
    MyUploads,
};

// Shared between the render thread (read for drawing) and the cloud
// worker thread (write on completion). Every field is guarded by
// `mtx`; we copy out to locals for the draw pass to keep lock scope
// short.
struct BrowseState {
    std::mutex mtx;
    std::vector<opendojo::cloud::api::DrillSummary> results;
    bool loading = false;
    std::string error;

    // Per-drill "I liked this" state lives on each DrillSummary (is_liked),
    // seeded from the server's liked_by_me on every list fetch and flipped
    // optimistically by kick_toggle_like — so the heart survives a menu
    // close/reopen and a game restart, and the user can't re-like to inflate.

    // Currently-active view mode (Browse / MyUploads). Switching modes
    // resets pagination + re-kicks the list query.
    Mode mode = Mode::Browse;

    // User-controlled inputs live on the render thread, so they don't
    // need mtx. ImGui owns the buffer storage.
    char search_buf[96] = "";
    // -1 = uninitialized; first draw seeds from the detected CPU.
    int character_combo_idx = -1;
    bool category_filter[kCategoryCount] = {};
    int difficulty_filter_idx = 0;  // 0 = Any
    int sort_idx = 0;               // 0 Newest, 1 Downloaded, 2 Liked
    int offset = 0;
    bool initial_load_done = false;

    // Last CPU character we auto-applied to the character filter. The
    // tab follows the live in-game CPU: when you switch the character
    // you're training against, the Browse filter re-points to the new
    // one. A manual combo pick doesn't move the detected character, so
    // it stays equal to this and we don't clobber the user's choice —
    // we only re-point when the in-game character actually changes.
    // Empty until the first detection.
    std::string auto_cpu_char;

    // Pending owner-delete target. See the matching pattern in
    // menu.cpp's draw_drills_tab — OpenPopup must happen at the
    // same ID-stack level as BeginPopupModal, so we defer.
    std::string delete_target_id;
    std::string delete_target_name;
    bool delete_modal_open_requested = false;

    // Pending report target. Same deferred-OpenPopup pattern.
    std::string report_target_id;
    std::string report_target_name;
    char report_reason_buf[256] = "";
    bool report_modal_open_requested = false;

    // Pending edit target (My uploads). Pre-fill all metadata buffers
    // when the user clicks Edit on a row; the modal reads these and
    // writes back on Save. ImGui owns the text-buffer memory.
    std::string edit_target_id;
    std::string edit_target_original_name;
    char edit_name_buf[opendojo::ui::DRILL_NAME_BUFFER_SIZE] = "";
    char edit_desc_buf[opendojo::ui::DESCRIPTION_BUFFER_SIZE] = "";
    bool edit_cat_picks[kCategoryCount] = {false, false, false, false, false};
    int edit_difficulty_idx = 0;  // matches kUploadDifficultyLabels — 0 = (none)
    bool edit_modal_open_requested = false;
};

BrowseState g_browse;

// Index of cloud drill ids already present in the local library, so the
// Browse list can mark them "Downloaded" and disable re-download. Built by
// scanning local drill headers for the `cloud_id` stamp save_drill_text
// writes. `cloud_ids` is touched only on the render thread (rebuilt when
// `dirty` is set); the cloud worker just flips `dirty` after a download so
// the next frame re-scans. Starts dirty so the first Browse render scans.
struct LocalLibraryIndex {
    std::set<std::string> cloud_ids;
    std::atomic<bool> dirty{true};
};
LocalLibraryIndex g_local;

// Rescan the local drills folder for cloud_id stamps. Render-thread only
// (does filesystem I/O, same cost as the Drills tab's own refresh).
void refresh_local_index_if_dirty() {
    if (!g_local.dirty.exchange(false)) return;
    std::set<std::string> ids;
    for (const auto& d : opendojo::commands::list_drills()) {
        if (!d.cloud_id.empty()) ids.insert(d.cloud_id);
    }
    g_local.cloud_ids = std::move(ids);
}

// Upload-side state. The Tag/Difficulty pickers in the Export tab
// write here; kick_upload reads at submit time.
struct UploadState {
    std::atomic<bool> in_flight{false};
    bool category_picks[kCategoryCount] = {false, false, false, false, false};
    int difficulty_idx = 0;  // 0 = (none)
    // Persistent last-upload status — toast disappears after a few
    // seconds, but the Share card keeps showing this line so a user
    // who looks away can still see whether the last upload succeeded.
    std::mutex status_mtx;
    std::string status_msg;
    bool status_is_error = false;
};
UploadState g_upload;

void set_upload_status(std::string msg, bool err) {
    std::lock_guard lk(g_upload.status_mtx);
    g_upload.status_msg = std::move(msg);
    g_upload.status_is_error = err;
}

// Operator broadcast shown in the window title bar. The render thread
// reads `text` to build the title; the worker thread writes it on
// fetch completion. `in_flight` keeps poll_service_message from
// stacking duplicate fetches while one is outstanding.
struct ServiceMsgState {
    std::mutex mtx;
    std::string text;  // current message ("" = none / not yet fetched)
    std::string update_version;
    std::atomic<bool> in_flight{false};
};
ServiceMsgState g_service_msg;

const char* kSortLabels[] = {"Newest", "Most downloaded", "Most liked"};

// Width an ImGui combo needs to fully show the longest of `items`
// without truncation. Combo reserves space on the right for the
// down-arrow button (== frame height) plus FramePadding on each
// side of the visible text. Use this with PushItemWidth so the
// widget always grows to fit its content rather than relying on
// hand-picked pixel widths that drift as fonts change.
float combo_item_width(const char* const* items, int count) {
    float max_w = 0.0f;
    for (int i = 0; i < count; ++i) {
        max_w = (std::max)(max_w, ImGui::CalcTextSize(items[i]).x);
    }
    const auto& style = ImGui::GetStyle();
    return max_w + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
}

// Convenience overload for std::vector<std::string> (used by the
// character-roster combo which builds its options dynamically).
float combo_item_width(const std::vector<std::string>& items, const char* const* extras,
                       int extra_count) {
    float max_w = 0.0f;
    for (int i = 0; i < extra_count; ++i) {
        max_w = (std::max)(max_w, ImGui::CalcTextSize(extras[i]).x);
    }
    for (const auto& s : items) {
        max_w = (std::max)(max_w, ImGui::CalcTextSize(s.c_str()).x);
    }
    const auto& style = ImGui::GetStyle();
    return max_w + ImGui::GetFrameHeight() + style.FramePadding.x * 2.0f;
}

opendojo::cloud::api::SortOrder sort_from_idx(int i) {
    using S = opendojo::cloud::api::SortOrder;
    switch (i) {
        case 1: return S::MostDownloaded;
        case 2: return S::MostLiked;
        default: return S::NewestFirst;
    }
}

// Resolve the character-combo selection to the actual filter string
// the API expects. Returns "" if "All" is selected.
std::string resolve_character_filter() {
    int idx = g_browse.character_combo_idx;
    if (idx <= kCharComboAll) return {};
    int rosterIdx = idx - kCharComboRosterBase;
    const auto& r = roster();
    if (rosterIdx >= 0 && rosterIdx < static_cast<int>(r.size())) return r[rosterIdx];
    return {};
}

// Find the roster index for `character_name`, or -1 if not present.
int roster_index_of(const std::string& character_name) {
    if (character_name.empty()) return -1;
    const auto& r = roster();
    for (int i = 0; i < static_cast<int>(r.size()); ++i) {
        if (r[i] == character_name) return i;
    }
    return -1;
}

void kick_list() {
    opendojo::cloud::api::ListQuery q;
    if (g_browse.mode == Mode::MyUploads) {
        // My uploads ignores the community-browse filter row entirely —
        // every drill here is yours, so character / difficulty / category
        // narrowing would just hide your own work for no reason. Sort
        // stays newest-first regardless.
        q.mine_only = true;
        q.sort = opendojo::cloud::api::SortOrder::NewestFirst;
    } else {
        q.search_query = g_browse.search_buf;
        q.character_filter = resolve_character_filter();
        for (int i = 0; i < kCategoryCount; ++i) {
            if (g_browse.category_filter[i]) q.category_filter.emplace_back(kCategories[i].id);
        }
        if (g_browse.difficulty_filter_idx > 0 &&
            g_browse.difficulty_filter_idx <= kDifficultyCount) {
            q.difficulty_filter = kDifficulties[g_browse.difficulty_filter_idx - 1].id;
        }
        q.sort = sort_from_idx(g_browse.sort_idx);
    }
    q.offset = g_browse.offset;

    {
        std::lock_guard lk(g_browse.mtx);
        g_browse.loading = true;
        g_browse.error.clear();
    }
    opendojo::cloud::worker::submit(
        [q]() {
            opendojo::cloud::api::ListResult r;
            try {
                r = opendojo::cloud::api::list_drills(q);
            } catch (const std::exception& e) {
                // Never let a parse/throw leave the tab stuck on "loading": the
                // worker would swallow the exception and we'd never clear the flag.
                OPENDOJO_LOG("cloud_ui: list_drills threw: %s", e.what());
                r.ok = false;
                r.error_message = "Couldn't load drills. Please try again.";
            }
            std::lock_guard lk(g_browse.mtx);
            g_browse.loading = false;
            if (r.ok) {
                g_browse.results = std::move(r.drills);
            } else {
                g_browse.error = r.error_message;
            }
        },
        [] {
            std::lock_guard lk(g_browse.mtx);
            g_browse.loading = false;
            g_browse.error = "Couldn't load drills. Please try again.";
        });
}

void kick_toggle_like(const std::string& drill_id) {
    opendojo::cloud::worker::submit([drill_id]() {
        auto r = opendojo::cloud::api::toggle_like(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't save your like. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        // Patch the cached summary so the table updates immediately without
        // a refetch: flip is_liked (the toggle succeeded, so the state is now
        // the opposite of what the button showed) and adopt the server's new
        // like count. A later list fetch re-seeds is_liked from liked_by_me.
        std::lock_guard lk(g_browse.mtx);
        for (auto& d : g_browse.results) {
            if (d.id == drill_id) {
                d.is_liked = !d.is_liked;
                d.likes = r.likes;
                break;
            }
        }
    });
}

void kick_report(const std::string& drill_id, const std::string& display_name,
                 const std::string& reason) {
    opendojo::cloud::worker::submit([drill_id, display_name, reason]() {
        auto r = opendojo::cloud::api::report_drill(drill_id, reason);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't submit your report. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        if (r.reported) {
            opendojo::menu::queue_toast("Reported: " + display_name, false);
        } else {
            opendojo::menu::queue_toast("Already reported.", false);
        }
    });
}

void kick_delete_my_drill(const std::string& drill_id, const std::string& display_name) {
    opendojo::cloud::worker::submit([drill_id, display_name]() {
        auto r = opendojo::cloud::api::delete_my_drill(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't delete that drill. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        if (!r.deleted) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "That drill isn't yours to delete."
                                            : r.error_message,
                                        true);
            return;
        }
        // Patch the local cache so the row disappears instantly
        // without waiting on a list refetch.
        {
            std::lock_guard lk(g_browse.mtx);
            auto& v = g_browse.results;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const opendojo::cloud::api::DrillSummary& d) {
                                       return d.id == drill_id;
                                   }),
                    v.end());
        }
        opendojo::menu::queue_toast("Deleted: " + display_name, false);
    });
}

void kick_update_drill(opendojo::cloud::api::UpdateArgs args) {
    opendojo::cloud::worker::submit([args = std::move(args)]() {
        auto r = opendojo::cloud::api::update_drill(args);
        if (!r.ok || !r.updated) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't save your changes. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        // Patch local cache so the row reflects the new metadata
        // instantly without waiting on a list refetch.
        {
            std::lock_guard lk(g_browse.mtx);
            for (auto& d : g_browse.results) {
                if (d.id == args.drill_id) {
                    d.name = args.name;
                    d.description = args.description;
                    d.categories = args.categories;
                    d.difficulty = args.difficulty;
                    break;
                }
            }
        }
        opendojo::menu::queue_toast("Saved changes.", false);
    });
}

void kick_download(const opendojo::cloud::api::DrillSummary& summary) {
    opendojo::cloud::worker::submit([summary]() {
        const auto& drill_id = summary.id;
        const auto& display_name = summary.name;
        auto r = opendojo::cloud::api::get_drill(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't download that drill. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        const opendojo::commands::DownloadMetadata metadata{summary.author_handle,
                                                            summary.description, r.drill.character,
                                                            summary.cpu_side};
        auto save =
            opendojo::commands::save_drill_text(display_name.empty() ? r.drill.name : display_name,
                                                r.drill.content, drill_id, &metadata);
        if (!save.ok) {
            OPENDOJO_LOG("cloud download: local save failed for %s (%s): %s", display_name.c_str(),
                         drill_id.c_str(), save.message.c_str());
            opendojo::menu::queue_toast("Couldn't save \"" +
                                            (display_name.empty() ? r.drill.name : display_name) +
                                            "\": " + save.message,
                                        true);
            return;
        }
        opendojo::menu::queue_toast("Downloaded: " + save.path.filename().string(), false);
        opendojo::menu::queue_drills_refresh();
        // The local library changed — make the Browse list re-scan so this
        // drill flips to "Downloaded" on the next frame.
        g_local.dirty.store(true);
    });
}

void kick_upload(const std::string& name_in, const std::string& description_in) {
    // Compose the drill payload on the render thread so we read game
    // memory (slot state, CPU detection) under whatever invariants
    // the rest of the menu already relies on. The worker then ships
    // the prepared text without touching the game.
    // Upload status surfaces exclusively in the Share card's persistent
    // status line — no queue_toast() calls here. The global toast would
    // double up with the in-card status whenever the user is on the
    // Export tab (where uploads originate), and on other tabs the status
    // is still visible the next time the user revisits Export.
    auto p = opendojo::commands::build_current_slots_payload(name_in, description_in);
    if (!p.ok) {
        set_upload_status(p.message, true);
        return;
    }

    // Snapshot the tagging inputs on the render thread before
    // handing off — the user could change them mid-flight otherwise.
    std::vector<std::string> picked_categories;
    for (int i = 0; i < kCategoryCount; ++i) {
        if (g_upload.category_picks[i]) picked_categories.emplace_back(kCategories[i].id);
    }
    std::string picked_difficulty;
    if (g_upload.difficulty_idx > 0 && g_upload.difficulty_idx <= kDifficultyCount) {
        picked_difficulty = kDifficulties[g_upload.difficulty_idx - 1].id;
    }
    std::string dll_ver = opendojo::cloud::dll_version();
    // Resolve the author handle once on the render thread. current()
    // touches steam_api64.dll via GetProcAddress; safer to do it here
    // where DLL state is well-defined than from the worker thread.
    std::string author = opendojo::cloud::handle::current();
    if (author.empty()) {
        set_upload_status("Set an author handle in Settings before uploading.", true);
        return;
    }

    g_upload.in_flight.store(true);
    set_upload_status("Uploading...", false);
    opendojo::cloud::worker::submit(
        [payload = std::move(p), categories = std::move(picked_categories),
         difficulty = std::move(picked_difficulty), dll_ver = std::move(dll_ver),
         author = std::move(author)]() {
            opendojo::cloud::api::SubmitArgs args;
            args.name = payload.name;
            args.description = payload.description;
            args.character = payload.character;
            args.cpu_side = payload.cpu_side;
            args.recordings_count = payload.recordings_count;
            args.content = payload.text;
            args.categories = categories;
            args.difficulty = difficulty;
            args.dll_version = dll_ver;
            args.author_handle = author;

            auto r = opendojo::cloud::api::submit_drill(args);
            g_upload.in_flight.store(false);
            if (!r.ok) {
                set_upload_status(r.error_message.empty()
                                      ? "Couldn't upload your drill. Please try again."
                                      : r.error_message,
                                  true);
                return;
            }
            set_upload_status(r.deduped ? "Identical drill already on OpenDojo Cloud"
                                        : "Uploaded to OpenDojo Cloud",
                              false);
        },
        [] {
            g_upload.in_flight.store(false);
            set_upload_status("Couldn't upload your drill. Please try again.", true);
        });
}

}  // namespace

void draw_cloud_tab() {
    if (!opendojo::cloud::configured()) {
        ImGui::TextDisabled("OpenDojo Cloud is not configured in this build.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Source builds ship without cloud access — Cloud browse and Upload "
            "are disabled. Everything else works normally.");
        return;
    }

    // Keep the character filter pointed at whoever you're training
    // against. detect_cpu() is a handful of SEH-guarded reads (the
    // Export tab already calls it every frame), so polling it here is
    // cheap. We act only when the detected character *changes*: that
    // covers the first detection (seed) and every later in-practice
    // character swap (re-point). A manual combo pick doesn't change the
    // detected character, so it survives until you switch characters in
    // game. The re-query only fires in Browse mode, where the filter
    // matters — My uploads ignores it, but the combo index still
    // updates so switching back to Browse lands on the right character.
    auto cpu = opendojo::game_thread::current_cpu();
    if (cpu.detected && cpu.character_name != g_browse.auto_cpu_char) {
        g_browse.auto_cpu_char = cpu.character_name;
        int idx = roster_index_of(cpu.character_name);
        g_browse.character_combo_idx = idx >= 0 ? (kCharComboRosterBase + idx) : kCharComboAll;
        g_browse.offset = 0;
        if (!g_browse.initial_load_done || g_browse.mode == Mode::Browse) {
            g_browse.initial_load_done = true;
            kick_list();
        }
    } else if (!g_browse.initial_load_done) {
        // No CPU detected yet (outside a match) on the first render:
        // still kick the initial query once so the tab isn't empty.
        // The filter defaults to "All characters" until a CPU appears.
        g_browse.initial_load_done = true;
        g_browse.character_combo_idx = kCharComboAll;
        kick_list();
    }

    // ---- Pill toggle: Browse / My uploads ----------------------------
    // The pills swap the entire content area below: Browse shows the
    // community filter row + community table; My uploads hides filters
    // and renders just the caller's own drills with Edit/Delete row
    // actions. Active pill is rendered in the accent color, inactive
    // pill in the default frame color so the choice reads at a glance.
    {
        const ImVec4 active(0.78f, 0.18f, 0.22f, 1.0f);
        const auto set_mode = [](Mode m) {
            if (g_browse.mode == m) return;
            g_browse.mode = m;
            g_browse.offset = 0;
            // Clear the cached result list so we don't briefly flash
            // the wrong content while the new query is in flight.
            {
                std::lock_guard lk(g_browse.mtx);
                g_browse.results.clear();
                g_browse.error.clear();
            }
            kick_list();
        };
        const bool browsing = g_browse.mode == Mode::Browse;
        if (browsing) ImGui::PushStyleColor(ImGuiCol_Button, active);
        if (ImGui::Button("Browse")) set_mode(Mode::Browse);
        opendojo::menu::nav_recenter();
        if (browsing) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (!browsing) ImGui::PushStyleColor(ImGuiCol_Button, active);
        if (ImGui::Button("My uploads")) set_mode(Mode::MyUploads);
        opendojo::menu::nav_recenter();
        if (!browsing) ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    if (g_browse.mode == Mode::MyUploads) {
        ImGui::TextDisabled("Drills you've published to OpenDojo Cloud.");
    } else {
        // ---- Row 1: search box + Search + Clear ------------------------
        int active_filters = (g_browse.character_combo_idx != kCharComboAll) +
                             (g_browse.difficulty_filter_idx != 0);
        for (bool selected : g_browse.category_filter)
            active_filters += selected;
        const std::string filters_label =
            active_filters ? "Filters (" + std::to_string(active_filters) + ")" : "Filters";
        const float controls_width =
            ImGui::CalcTextSize("Search").x + ImGui::CalcTextSize("Clear").x +
            ImGui::CalcTextSize(filters_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 6 +
            ImGui::GetStyle().ItemSpacing.x * 3;
        ImGui::PushItemWidth((std::max)(ImGui::GetFontSize() * 4,
                                        ImGui::GetContentRegionAvail().x - controls_width));
        bool submitted = ImGui::InputTextWithHint("##search", "Search community drills",
                                                  g_browse.search_buf, sizeof(g_browse.search_buf),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Search") || submitted) {
            g_browse.offset = 0;
            kick_list();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            g_browse.search_buf[0] = 0;
            g_browse.character_combo_idx = kCharComboAll;
            g_browse.difficulty_filter_idx = 0;
            for (int i = 0; i < kCategoryCount; ++i)
                g_browse.category_filter[i] = false;
            g_browse.offset = 0;
            kick_list();
        }

        ImGui::SameLine();
        if (ImGui::Button(filters_label.c_str())) ImGui::OpenPopup("Cloud filters");
        if (ImGui::BeginPopup("Cloud filters")) {
            // ---- Row 2: character / difficulty / sort -----------------------------
            ImGui::Spacing();
            ImGui::TextDisabled("Character:");
            ImGui::SameLine();
            {
                std::string current_label;
                int idx = g_browse.character_combo_idx;
                if (idx <= kCharComboAll) {
                    current_label = "All characters";
                } else {
                    int ri = idx - kCharComboRosterBase;
                    const auto& r = roster();
                    current_label = (ri >= 0 && ri < static_cast<int>(r.size())) ? r[ri] : "?";
                }

                const char* kAllLabel[] = {"All characters"};
                ImGui::PushItemWidth(combo_item_width(roster(), kAllLabel, 1));
                if (ImGui::BeginCombo("##character", current_label.c_str())) {
                    if (ImGui::Selectable("All characters", idx == kCharComboAll)) {
                        g_browse.character_combo_idx = kCharComboAll;
                        g_browse.offset = 0;
                        kick_list();
                    }
                    ImGui::Separator();
                    const auto& r = roster();
                    for (int i = 0; i < static_cast<int>(r.size()); ++i) {
                        int combo_idx = kCharComboRosterBase + i;
                        if (ImGui::Selectable(r[i].c_str(), idx == combo_idx)) {
                            g_browse.character_combo_idx = combo_idx;
                            g_browse.offset = 0;
                            kick_list();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Difficulty:");
            ImGui::SameLine();
            ImGui::PushItemWidth(
                combo_item_width(kDifficultyFilterLabels, IM_ARRAYSIZE(kDifficultyFilterLabels)));
            if (ImGui::Combo("##diff_filter", &g_browse.difficulty_filter_idx,
                             kDifficultyFilterLabels, IM_ARRAYSIZE(kDifficultyFilterLabels))) {
                g_browse.offset = 0;
                kick_list();
            }
            ImGui::PopItemWidth();
            ImGui::Spacing();
            ImGui::TextDisabled("Sort:");
            ImGui::SameLine();
            ImGui::PushItemWidth(combo_item_width(kSortLabels, IM_ARRAYSIZE(kSortLabels)));
            if (ImGui::Combo("##sort", &g_browse.sort_idx, kSortLabels,
                             IM_ARRAYSIZE(kSortLabels))) {
                g_browse.offset = 0;
                kick_list();
            }
            ImGui::PopItemWidth();

            // ---- Row 3: tag chips. Checkbox renders close enough to a
            // toggleable chip; one per category. Re-queries on change.
            ImGui::Spacing();
            ImGui::TextDisabled("Tags:");
            for (int i = 0; i < kCategoryCount; ++i) {
                ImGui::PushID(i);
                if (ImGui::Checkbox(kCategories[i].label, &g_browse.category_filter[i])) {
                    g_browse.offset = 0;
                    kick_list();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }  // end of Browse-mode filter row

    ImGui::Spacing();

    // Snapshot under lock so we render a consistent view this frame.
    std::vector<opendojo::cloud::api::DrillSummary> snapshot;
    bool loading;
    std::string error;
    {
        std::lock_guard lk(g_browse.mtx);
        snapshot = g_browse.results;
        loading = g_browse.loading;
        error = g_browse.error;
    }

    if (loading) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Loading...");
        ImGui::Spacing();
    }
    if (!error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.40f, 1), "%s", error.c_str());
        ImGui::Spacing();
    }

    if (snapshot.empty() && !loading) {
        if (g_browse.mode == Mode::MyUploads) {
            ImGui::TextDisabled(
                "You haven't uploaded any drills yet. Open the Export tab and use "
                "\"Share to OpenDojo Cloud\" to publish your first.");
        } else {
            ImGui::TextDisabled("No drills found. Try clearing filters or searching.");
        }
        return;
    }

    // Refresh the "already downloaded" index if a download landed since the
    // last frame (or this is the first Browse render). Cheap no-op when clean.
    refresh_local_index_if_dirty();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    const bool my_uploads = g_browse.mode == Mode::MyUploads;
    // The scrolling child always reserves a gutter, including short result
    // lists. The table therefore never loses width when results start scrolling.
    const float footer_height = ImGui::GetFrameHeightWithSpacing() +
                                ImGui::GetStyle().ItemSpacing.y;
    const float table_height = (std::max)(ImGui::GetTextLineHeight() * 4,
                                          ImGui::GetContentRegionAvail().y - footer_height);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool show_table = ImGui::BeginChild("cloud_results", ImVec2(0, table_height),
                                              ImGuiChildFlags_None,
                                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (show_table && ImGui::BeginTable("cloud_drills", my_uploads ? 3 : 4, flags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.8f);
        const float pad = ImGui::GetStyle().CellPadding.x;
        // Columns 1 + 2 swap roles based on mode. Browse = Like / Download
        // (community actions). My uploads = Edit / Delete (owner actions).
        if (my_uploads) {
            const float edit_w = ImGui::CalcTextSize("Edit").x + pad * 4;
            const float del_w = ImGui::CalcTextSize("Delete").x + pad * 4;
            ImGui::TableSetupColumn("Edit",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    edit_w);
            ImGui::TableSetupColumn("Delete",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    del_w);
        } else {
            const float like_w = ImGui::CalcTextSize("Unlike").x + pad * 4;
            // Size for "Downloaded" (the wider of the two states) so the
            // button text never clips once a drill is in the library.
            const float dl_w = ImGui::CalcTextSize("Downloaded").x + pad * 4;
            ImGui::TableSetupColumn("Like",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    like_w);
            ImGui::TableSetupColumn("Download",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    dl_w);
            ImGui::TableSetupColumn("Report",
                                    ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_NoHeaderLabel,
                                    ImGui::CalcTextSize("Report").x + pad * 4);
        }
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& d = snapshot[i];
            ImGui::PushID(d.id.c_str());
            ImGui::TableNextRow();

            // The whole name cell is a hover/focus target. Use a hidden ID
            // so uploaded names are always displayed as literal text.
            ImGui::TableSetColumnIndex(0);
            const ImVec2 text_pos = ImGui::GetCursorScreenPos();
            const float title_height = ImGui::GetTextLineHeight();
            ImFont* small = opendojo::render_hook::small_font();
            if (small) ImGui::PushFont(small);
            const float meta_height = ImGui::GetTextLineHeight();
            if (small) ImGui::PopFont();
            const float line_gap = ImGui::GetStyle().ItemSpacing.y;
            const float cell_right = text_pos.x + ImGui::GetContentRegionAvail().x;
            const float row_height = title_height + line_gap + meta_height;
            const bool open_details =
                ImGui::Selectable("##details", false, 0, ImVec2(0, row_height));
            const ImVec2 name_min = ImGui::GetItemRectMin();
            const ImVec2 name_max = ImGui::GetItemRectMax();
            auto* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(name_min, name_max, true);
            auto fitted = [](std::string text, float width) {
                if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
                const float dots = ImGui::CalcTextSize("...").x;
                if (width < dots) return std::string();
                while (!text.empty() && ImGui::CalcTextSize(text.c_str()).x + dots > width) {
                    std::size_t last = text.size() - 1;
                    while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80)
                        --last;
                    text.resize(last);
                }
                return text + "...";
            };
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            float title_right = cell_right;
            if (!d.difficulty.empty()) {
                const char* difficulty = difficulty_label(d.difficulty);
                const float x =
                    (std::max)(text_pos.x, cell_right - ImGui::CalcTextSize(difficulty).x);
                draw->AddText(ImVec2(x, text_pos.y),
                              ImGui::GetColorU32(difficulty_color(d.difficulty)), difficulty);
                title_right = x - gap;
            }
            const auto title = fitted(d.name, title_right - text_pos.x);
            draw->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
            if (small) ImGui::PushFont(small);
            const float meta_y = text_pos.y + title_height + line_gap;
            const std::string stats = std::to_string(d.downloads) + " downloads / " +
                                      std::to_string(d.likes) + " likes";
            const float stats_x =
                (std::max)(text_pos.x, cell_right - ImGui::CalcTextSize(stats.c_str()).x);
            draw->AddText(ImVec2(stats_x, meta_y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          stats.c_str());
            const auto author =
                fitted(d.author_handle.empty() ? "Unknown author" : "by " + d.author_handle,
                       stats_x - text_pos.x - gap);
            draw->AddText(ImVec2(text_pos.x, meta_y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          author.c_str());
            std::string tags;
            const auto visible_tags =
                (std::min)(d.categories.size(), static_cast<std::size_t>(kInlineTags));
            for (std::size_t tag = 0; tag < visible_tags; ++tag) {
                if (!tags.empty()) tags += "  ";
                tags += "#";
                tags += category_label(d.categories[tag]);
            }
            if (visible_tags < d.categories.size())
                tags += "  +" + std::to_string(d.categories.size() - visible_tags);
            const float tags_x = text_pos.x + ImGui::CalcTextSize(author.c_str()).x + gap;
            tags = fitted(tags, stats_x - tags_x - gap);
            draw->AddText(ImVec2(tags_x, meta_y), ImGui::GetColorU32(ImVec4(0.55f, 0.75f, 1.0f, 1)),
                          tags.c_str());
            if (small) ImGui::PopFont();
            draw->PopClipRect();

            auto show_details = [&]() {
                const float width = (std::min)(ImGui::GetFontSize() * 32.0f,
                                               ImGui::GetMainViewport()->WorkSize.x * 0.75f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                if (title != d.name) ImGui::TextWrapped("%s", d.name.c_str());
                if (d.character.empty() || d.character == "unknown") {
                    ImGui::TextDisabled("Character not specified");
                } else {
                    ImGui::Text("Character: %s%s%s%s", d.character.c_str(),
                                d.cpu_side.empty() ? "" : " (", d.cpu_side.c_str(),
                                d.cpu_side.empty() ? "" : ")");
                }
                ImGui::Text("%d recordings", d.recordings_count);
                if (!d.categories.empty()) {
                    std::string all_tags;
                    for (const auto& tag : d.categories) {
                        if (!all_tags.empty()) all_tags += "  ";
                        all_tags += "#";
                        all_tags += category_label(tag);
                    }
                    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", all_tags.c_str());
                }
                ImGui::Separator();
                ImGui::TextWrapped("%s", d.description.empty() ? "No description."
                                                               : d.description.c_str());
                ImGui::PopTextWrapPos();
            };
            if (ImGui::BeginItemTooltip()) {
                show_details();
                ImGui::EndTooltip();
            }
            // Keyboard/controller activation or clicking keeps the details open.
            if (open_details) ImGui::OpenPopup("Drill details");
            if (ImGui::BeginPopup("Drill details")) {
                show_details();
                ImGui::EndPopup();
            }

            // ---- Action columns ------------------------------------
            // Browse mode: Like + Download.
            // My uploads mode: Edit + Delete (owner-only actions).
            // Delete here uses the same confirmation modal as before;
            // Edit opens the edit modal pre-filled with this row's
            // current name + description.
            if (my_uploads) {
                ImGui::TableSetColumnIndex(1);
                if (opendojo::ui::cell_action("Edit", row_height)) {
                    g_browse.edit_target_id = d.id;
                    g_browse.edit_target_original_name = d.name;
                    opendojo::ui::copy_text(g_browse.edit_name_buf, d.name);
                    opendojo::ui::copy_text(g_browse.edit_desc_buf, d.description);
                    // Seed tag pickers from current categories.
                    for (int ci = 0; ci < kCategoryCount; ++ci) {
                        g_browse.edit_cat_picks[ci] = false;
                        for (const auto& tag : d.categories) {
                            if (tag == kCategories[ci].id) {
                                g_browse.edit_cat_picks[ci] = true;
                                break;
                            }
                        }
                    }
                    // Seed difficulty combo from the current value;
                    // index 0 = (none).
                    g_browse.edit_difficulty_idx = 0;
                    for (int di = 0; di < kDifficultyCount; ++di) {
                        if (d.difficulty == kDifficulties[di].id) {
                            g_browse.edit_difficulty_idx = di + 1;
                            break;
                        }
                    }
                    g_browse.edit_modal_open_requested = true;
                }
                ImGui::TableSetColumnIndex(2);
                if (opendojo::ui::cell_action("Delete##rowdel", row_height, true)) {
                    g_browse.delete_target_id = d.id;
                    g_browse.delete_target_name = d.name;
                    g_browse.delete_modal_open_requested = true;
                }
            } else {
                ImGui::TableSetColumnIndex(1);
                if (opendojo::ui::cell_action(d.is_liked ? "Unlike" : "Like", row_height)) {
                    kick_toggle_like(d.id);
                }
                ImGui::TableSetColumnIndex(2);
                // Drills already in the local library show a disabled
                // "Downloaded" instead of "Download" — re-downloading would
                // just write a duplicate file (and the server counts a
                // download once per user regardless).
                if (g_local.cloud_ids.count(d.id) > 0) {
                    ImGui::BeginDisabled();
                    opendojo::ui::cell_action("Downloaded", row_height);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Already in your drills");
                    }
                } else if (opendojo::ui::cell_action("Download", row_height)) {
                    kick_download(d);
                }
            }
            if (!my_uploads && !d.is_mine) {
                ImGui::TableSetColumnIndex(3);
                if (opendojo::ui::cell_action("Report", row_height)) {
                    g_browse.report_target_id = d.id;
                    g_browse.report_target_name = d.name;
                    g_browse.report_reason_buf[0] = 0;
                    g_browse.report_modal_open_requested = true;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ---- Delete confirmation modal -----------------------------------
    // OpenPopup must be called at the same ID-stack level as
    // BeginPopupModal; in-row clicks set the flag and we open here.
    if (g_browse.delete_modal_open_requested) {
        g_browse.delete_modal_open_requested = false;
        ImGui::OpenPopup("Delete drill###DeleteCloudDrill");
    }
    center_next_modal_on_menu();
    if (ImGui::BeginPopupModal("Delete drill###DeleteCloudDrill", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete this drill?");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s",
                           g_browse.delete_target_name.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Other players will lose access immediately. This can't be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (destructive_button("Delete", ImVec2(120, 0))) {
            kick_delete_my_drill(g_browse.delete_target_id, g_browse.delete_target_name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Edit modal --------------------------------------------------
    // Owner-only — opened from a row's Edit button in My uploads view.
    // The buffers were pre-filled at click time; Save kicks the API.
    if (g_browse.edit_modal_open_requested) {
        g_browse.edit_modal_open_requested = false;
        ImGui::OpenPopup("Edit drill###EditCloudDrill");
    }
    // SetNextWindowSize must be called immediately before the matching
    // Begin* call — issuing it on the OpenPopup frame doesn't carry
    // over to the BeginPopupModal frame, which is why the previous
    // attempt left the modal at its auto-sized (skinny) default.
    // Width also gives Description real room; the field word-wraps
    // (ImGuiInputTextFlags_WordWrap), so a wider box means fewer wrapped
    // lines and a more readable paragraph.
    center_next_modal_on_menu();
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit drill###EditCloudDrill", nullptr, 0)) {
        // The window title bar carries "Edit drill" (text before ###); the
        // original drill name here orients the user on what they're editing.
        ImGui::TextDisabled("%s", g_browse.edit_target_original_name.c_str());
        ImGui::Spacing();
        // Inputs stretch to the modal width so long lines have the
        // most horizontal room before scrolling.
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::InputText("##editname", g_browse.edit_name_buf, sizeof(g_browse.edit_name_buf));
        ImGui::PopItemWidth();
        ImGui::TextDisabled("Name (1-96 chars)");

        ImGui::Spacing();
        ImGui::InputTextMultiline(
            "##editdesc", g_browse.edit_desc_buf, sizeof(g_browse.edit_desc_buf),
            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2),
            ImGuiInputTextFlags_WordWrap);
        ImGui::TextDisabled("Description (optional, up to 1000 chars)");

        ImGui::Spacing();
        ImGui::TextDisabled("Tags (up to 5)");
        if (ImGui::BeginTable("editTags", 2,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
            for (int i = 0; i < kCategoryCount; ++i) {
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                drill_tag_checkbox(i, g_browse.edit_cat_picks);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Difficulty (optional)");
        ImGui::SameLine();
        ImGui::PushItemWidth(
            combo_item_width(kUploadDifficultyLabels, IM_ARRAYSIZE(kUploadDifficultyLabels)));
        ImGui::Combo("##editdiff", &g_browse.edit_difficulty_idx, kUploadDifficultyLabels,
                     IM_ARRAYSIZE(kUploadDifficultyLabels));
        ImGui::PopItemWidth();

        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        // Save is disabled until there's at least one non-whitespace
        // character in the name — server would reject otherwise.
        const bool name_has_content = [&] {
            for (const char* p = g_browse.edit_name_buf; *p; ++p) {
                if (*p != ' ' && *p != '\t') return true;
            }
            return false;
        }();
        if (!name_has_content) ImGui::BeginDisabled();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            opendojo::cloud::api::UpdateArgs args;
            args.drill_id = g_browse.edit_target_id;
            args.name = g_browse.edit_name_buf;
            args.description = g_browse.edit_desc_buf;
            for (int i = 0; i < kCategoryCount; ++i) {
                if (g_browse.edit_cat_picks[i]) args.categories.emplace_back(kCategories[i].id);
            }
            if (g_browse.edit_difficulty_idx > 0 &&
                g_browse.edit_difficulty_idx <= kDifficultyCount) {
                args.difficulty = kDifficulties[g_browse.edit_difficulty_idx - 1].id;
            }
            kick_update_drill(std::move(args));
            ImGui::CloseCurrentPopup();
        }
        if (!name_has_content) ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    // ---- Report modal ------------------------------------------------
    if (g_browse.report_modal_open_requested) {
        g_browse.report_modal_open_requested = false;
        ImGui::OpenPopup("Report drill###ReportCloudDrill");
    }
    center_next_modal_on_menu();
    if (ImGui::BeginPopupModal("Report drill###ReportCloudDrill", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Report this drill?");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s",
                           g_browse.report_target_name.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Tell us what's wrong (optional).");
        ImGui::Spacing();
        ImGui::PushItemWidth(380);
        ImGui::InputTextMultiline(
            "##reportreason", g_browse.report_reason_buf, sizeof(g_browse.report_reason_buf),
            ImVec2(380, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2),
            ImGuiInputTextFlags_WordWrap);
        ImGui::PopItemWidth();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Submit report", ImVec2(140, 0))) {
            kick_report(g_browse.report_target_id, g_browse.report_target_name,
                        g_browse.report_reason_buf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Pagination. Server caps page at 50; we let users walk forwards
    // and backwards. Hide "next" if we got fewer than the page size.
    constexpr int kPageSize = 50;
    ImGui::Spacing();
    if (g_browse.offset > 0) {
        if (ImGui::Button("< Prev")) {
            g_browse.offset = (g_browse.offset >= kPageSize) ? g_browse.offset - kPageSize : 0;
            kick_list();
        }
        ImGui::SameLine();
    }
    if (static_cast<int>(snapshot.size()) >= kPageSize) {
        if (ImGui::Button("Next >")) {
            g_browse.offset += kPageSize;
            kick_list();
        }
        ImGui::SameLine();
    }
    if (g_browse.offset > 0 || static_cast<int>(snapshot.size()) >= kPageSize) {
        ImGui::TextDisabled("Page %d", g_browse.offset / kPageSize + 1);
    }
}

void draw_share_card_body(bool can_export, const char* name, const char* description) {
    if (!opendojo::cloud::configured()) {
        ImGui::TextWrapped(
            "Upload to OpenDojo Cloud is disabled — this DLL was built without "
            "cloud support.");
        return;
    }

    // Keep publishing visible in the form. Optional metadata lives in a popup.
    const float options_width = ImGui::CalcTextSize("Tags & difficulty").x +
                                ImGui::GetStyle().FramePadding.x * 2;
    const float share_width = ImGui::CalcTextSize("Share to OpenDojo Cloud").x +
                              ImGui::GetStyle().FramePadding.x * 2;
    const bool options_fit = ImGui::GetContentRegionAvail().x >=
                             share_width + ImGui::GetStyle().ItemSpacing.x + options_width;
    const bool in_flight = g_upload.in_flight.load();
    const bool disabled = !can_export || in_flight;
    if (disabled) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
    if (ImGui::Button(in_flight ? "Uploading..." : "Share to OpenDojo Cloud",
                      ImVec2(share_width, 0)))
        kick_upload(name ? name : "", description ? description : "");
    ImGui::PopStyleColor(3);
    opendojo::menu::nav_recenter();
    if (disabled) ImGui::EndDisabled();
    if (options_fit) ImGui::SameLine();
    if (ImGui::Button("Tags & difficulty", ImVec2(options_width, 0)))
        ImGui::OpenPopup("Share options popup");
    opendojo::menu::nav_recenter();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 25, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Share options popup")) {
        // Use three columns when the section is wide enough, otherwise two.
        // Keep the grid aligned with the shared form above it.
        ImGui::TextDisabled("Tags (up to 5)");
        const int tag_columns = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 36 ? 3
                                                                                              : 2;
        if (ImGui::BeginTable("upload_tags", tag_columns,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
            for (int i = 0; i < kCategoryCount; ++i) {
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                drill_tag_checkbox(i, g_upload.category_picks);
                opendojo::menu::nav_recenter();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();

        ImGui::TextDisabled("Difficulty (optional)");
        ImGui::SameLine();
        ImGui::PushItemWidth(
            combo_item_width(kUploadDifficultyLabels, IM_ARRAYSIZE(kUploadDifficultyLabels)));
        ImGui::Combo("##upload_diff", &g_upload.difficulty_idx, kUploadDifficultyLabels,
                     IM_ARRAYSIZE(kUploadDifficultyLabels));
        opendojo::menu::nav_recenter();
        ImGui::PopItemWidth();

        ImGui::Spacing();

        // Show who the upload will be attributed to. Anonymous uploads
        // aren't allowed; the handle module guarantees a non-empty value
        // by re-seeding from Steam when needed.
        const std::string author = opendojo::cloud::handle::current();
        ImGui::TextDisabled("As:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "%s", author.c_str());

        ImGui::Spacing();

        ImGui::EndPopup();
    }

    // Persistent last-upload status — shown until the next upload
    // overwrites it. Toast still fires for visibility from other
    // tabs, but this line means an error never silently disappears.
    std::string msg;
    bool is_error;
    {
        std::lock_guard lk(g_upload.status_mtx);
        msg = g_upload.status_msg;
        is_error = g_upload.status_is_error;
    }
    if (!msg.empty()) {
        ImGui::Spacing();
        const ImVec4 col = is_error ? ImVec4(1.0f, 0.55f, 0.40f, 1.0f)
                                    : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
        if (is_error) {
            ImGui::TextColored(col, "Last upload failed:");
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(col, "%s", msg.c_str());
        }
    }
}

void draw_settings_section() {
    if (!opendojo::cloud::configured()) return;

    // ImGui owns this buffer across frames so a partially-typed name
    // isn't lost between draws. We seed it from the persisted value
    // the first time Settings is opened.
    static bool seeded = false;
    static char handle_buf[64] = "";
    if (!seeded) {
        auto cur = opendojo::cloud::handle::current();
        std::snprintf(handle_buf, sizeof(handle_buf), "%s", cur.c_str());
        seeded = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "OpenDojo Cloud");
    ImGui::Spacing();

    ImGui::TextDisabled("Author handle (stamped on drills you upload)");
    ImGui::PushItemWidth(280);
    ImGui::InputText("##handle", handle_buf, sizeof(handle_buf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Save##handle")) {
        opendojo::cloud::handle::set(handle_buf);
        auto cur = opendojo::cloud::handle::current();
        std::snprintf(handle_buf, sizeof(handle_buf), "%s", cur.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Steam##handle")) {
        opendojo::cloud::handle::reset_to_steam();
        auto cur = opendojo::cloud::handle::current();
        std::snprintf(handle_buf, sizeof(handle_buf), "%s", cur.c_str());
    }
}

void poll_service_message() {
    if (!opendojo::cloud::configured()) return;

    // Fetch once on first call, then refresh at most every 30 minutes.
    // poll_service_message is only ever called from the render thread
    // (menu::draw), so these statics need no synchronization; the
    // cross-thread handoff is g_service_msg below.
    using clock = std::chrono::steady_clock;
    static bool kicked = false;
    static clock::time_point last_kick{};
    const auto now = clock::now();
    if (g_service_msg.in_flight.load()) return;
    if (kicked && now - last_kick < std::chrono::minutes(30)) return;
    kicked = true;
    last_kick = now;

    g_service_msg.in_flight.store(true);
    opendojo::cloud::worker::submit(
        []() {
            auto r = opendojo::cloud::api::get_service_message();
            g_service_msg.in_flight.store(false);
            // On failure keep showing whatever we had and retry next
            // interval — a transient network blip shouldn't blank the bar.
            if (!r.ok) return;
            std::lock_guard lk(g_service_msg.mtx);
            g_service_msg.text = r.present ? r.message : std::string{};
            g_service_msg.update_version = r.update_version;
        },
        [] { g_service_msg.in_flight.store(false); });
}

std::string service_message() {
    std::lock_guard lk(g_service_msg.mtx);
    return g_service_msg.text;
}

std::string update_version() {
    std::lock_guard lk(g_service_msg.mtx);
    return g_service_msg.update_version;
}

void mark_local_library_dirty() {
    g_local.dirty.store(true);
}

}  // namespace opendojo::cloud::ui
