#include "cloud/cloud_ui.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
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
#include "players.hpp"
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
    {"throw_break", "Throw Break"},
};
constexpr int kCategoryCount = static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));

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

    // Drills the user has toggled-liked since this session started.
    // The server tracks the real (user, drill) like set; this is just
    // a UI optimization so the heart icon reflects the latest toggle
    // without us refetching the list.
    std::set<std::string> liked_session;

    // Drill ids whose description block is expanded (full text shown
    // inline). Caret toggles membership. Set lives on the render
    // thread; no lock needed since draw_browse_tab is the only writer.
    std::set<std::string> expanded_ids;

    // Currently-active view mode (Browse / MyUploads). Switching modes
    // resets pagination + re-kicks the list query.
    Mode mode = Mode::Browse;

    // User-controlled inputs live on the render thread, so they don't
    // need mtx. ImGui owns the buffer storage.
    char search_buf[96] = "";
    // -1 = uninitialized; first draw seeds from the detected CPU.
    int character_combo_idx = -1;
    bool category_filter[kCategoryCount] = {false, false, false, false, false};
    int difficulty_filter_idx = 0;  // 0 = Any
    int sort_idx = 0;               // 0 Newest, 1 Downloaded, 2 Liked
    int offset = 0;
    bool initial_load_done = false;

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
    char edit_name_buf[96] = "";
    char edit_desc_buf[1024] = "";
    bool edit_cat_picks[kCategoryCount] = {false, false, false, false, false};
    int edit_difficulty_idx = 0;  // matches kUploadDifficultyLabels — 0 = (none)
    bool edit_modal_open_requested = false;
};

BrowseState g_browse;

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
    opendojo::cloud::worker::submit([q]() {
        auto r = opendojo::cloud::api::list_drills(q);
        std::lock_guard lk(g_browse.mtx);
        g_browse.loading = false;
        if (r.ok) {
            g_browse.results = std::move(r.drills);
        } else {
            g_browse.error = r.error_message;
        }
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
        // Flip session-local "i liked this" set, and patch the cached
        // summary so the table updates immediately without a refetch.
        std::lock_guard lk(g_browse.mtx);
        auto it = g_browse.liked_session.find(drill_id);
        if (it == g_browse.liked_session.end()) {
            g_browse.liked_session.insert(drill_id);
        } else {
            g_browse.liked_session.erase(it);
        }
        for (auto& d : g_browse.results) {
            if (d.id == drill_id) {
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

void kick_download(const std::string& drill_id, const std::string& display_name) {
    opendojo::cloud::worker::submit([drill_id, display_name]() {
        auto r = opendojo::cloud::api::get_drill(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty()
                                            ? "Couldn't download that drill. Please try again."
                                            : r.error_message,
                                        true);
            return;
        }
        auto save = opendojo::commands::save_drill_text(
            display_name.empty() ? r.drill.name : display_name, r.drill.content);
        if (!save.ok) {
            opendojo::menu::queue_toast("Downloaded, but couldn't save it locally.", true);
            return;
        }
        opendojo::menu::queue_toast("Downloaded: " + save.path.filename().string(), false);
        opendojo::menu::queue_drills_refresh();
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
    opendojo::cloud::worker::submit([payload = std::move(p),
                                     categories = std::move(picked_categories),
                                     difficulty = std::move(picked_difficulty),
                                     dll_ver = std::move(dll_ver), author = std::move(author)]() {
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

    // First time the tab renders, seed the character filter from the
    // detected CPU and kick off the default search so the user lands
    // on a populated list. After this initial seed the combo is
    // user-controlled.
    if (!g_browse.initial_load_done) {
        g_browse.initial_load_done = true;
        auto cpu = opendojo::players::detect_cpu();
        int seed = roster_index_of(cpu.detected ? cpu.character_name : std::string{});
        g_browse.character_combo_idx = seed >= 0 ? (kCharComboRosterBase + seed) : kCharComboAll;
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
        ImGui::TextDisabled("Search community drills");
        ImGui::Spacing();

        // ---- Row 1: search box + Search + Clear ------------------------
        ImGui::PushItemWidth(360);
        bool submitted = ImGui::InputText("##search", g_browse.search_buf,
                                          sizeof(g_browse.search_buf),
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

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("Difficulty:");
        ImGui::SameLine();
        ImGui::PushItemWidth(
            combo_item_width(kDifficultyFilterLabels, IM_ARRAYSIZE(kDifficultyFilterLabels)));
        if (ImGui::Combo("##diff_filter", &g_browse.difficulty_filter_idx, kDifficultyFilterLabels,
                         IM_ARRAYSIZE(kDifficultyFilterLabels))) {
            g_browse.offset = 0;
            kick_list();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("Sort:");
        ImGui::SameLine();
        ImGui::PushItemWidth(combo_item_width(kSortLabels, IM_ARRAYSIZE(kSortLabels)));
        if (ImGui::Combo("##sort", &g_browse.sort_idx, kSortLabels, IM_ARRAYSIZE(kSortLabels))) {
            g_browse.offset = 0;
            kick_list();
        }
        ImGui::PopItemWidth();

        // ---- Row 3: tag chips. Checkbox renders close enough to a
        // toggleable chip; one per category. Re-queries on change.
        ImGui::Spacing();
        ImGui::TextDisabled("Tags:");
        ImGui::SameLine();
        for (int i = 0; i < kCategoryCount; ++i) {
            ImGui::PushID(i);
            if (ImGui::Checkbox(kCategories[i].label, &g_browse.category_filter[i])) {
                g_browse.offset = 0;
                kick_list();
            }
            ImGui::PopID();
            if (i + 1 < kCategoryCount) ImGui::SameLine();
        }
    }  // end of Browse-mode filter row

    ImGui::Spacing();
    ImGui::Separator();
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

    // Snapshot liked-session set so the table loop reads a
    // consistent view this frame.
    std::set<std::string> liked_now;
    {
        std::lock_guard lk(g_browse.mtx);
        liked_now = g_browse.liked_session;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const bool my_uploads = g_browse.mode == Mode::MyUploads;
    if (ImGui::BeginTable("cloud_drills", 6, flags, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.8f);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Stats", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        const auto pad = ImGui::GetStyle().FramePadding.x;
        // Columns 4 + 5 swap roles based on mode. Browse = Like / Download
        // (community actions). My uploads = Edit / Delete (owner actions).
        if (my_uploads) {
            const float edit_w = ImGui::CalcTextSize("Edit").x + pad * 4;
            const float del_w = ImGui::CalcTextSize("Delete").x + pad * 4;
            ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, edit_w);
            ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, del_w);
        } else {
            const float like_w = ImGui::CalcTextSize("Unlike").x + pad * 4;
            const float dl_w = ImGui::CalcTextSize("Download").x + pad * 4;
            ImGui::TableSetupColumn("Like", ImGuiTableColumnFlags_WidthFixed, like_w);
            ImGui::TableSetupColumn("Download", ImGuiTableColumnFlags_WidthFixed, dl_w);
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& d = snapshot[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // ---- Name cell: expand toggle + name + meta + description ----
            ImGui::TableSetColumnIndex(0);
            const bool expanded = g_browse.expanded_ids.count(d.id) > 0;
            const bool has_desc = !d.description.empty();
            // The collapsed view truncates descriptions longer than this
            // many chars with "…". Anything shorter renders identically
            // collapsed vs expanded, so the toggle would be a no-op and
            // is suppressed.
            constexpr std::size_t kSnippetLimit = 160;
            const bool can_expand = has_desc && d.description.size() > kSnippetLimit;
            // Disclosure triangle: ImGui::ArrowButton, shrunk via
            // FramePadding so it doesn't look like a transport button,
            // and dimmed to ~half-opacity so it reads as a hint rather
            // than a prominent action. Only rendered when expanding
            // would actually reveal more text.
            if (can_expand) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.45f));
                if (ImGui::ArrowButton("##exp", expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
                    if (expanded)
                        g_browse.expanded_ids.erase(d.id);
                    else
                        g_browse.expanded_ids.insert(d.id);
                }
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(d.name.c_str());

            // Meta + description rendered in the small font — these are
            // secondary information and shouldn't compete visually with
            // the name or column headers. Falls back to the default
            // font if the small one didn't load.
            ImFont* small = opendojo::render_hook::small_font();
            if (small) ImGui::PushFont(small);

            // Meta line: "by author" / "Your upload" pill / #tags, wrapped
            // across rows so a drill with many tags doesn't overflow or
            // truncate. Each chunk decides whether to SameLine the next
            // one based on remaining horizontal space (standard ImGui
            // wrap pattern; same shape as the upload tag grid).
            const float right_edge_x = ImGui::GetCursorScreenPos().x +
                                       ImGui::GetContentRegionAvail().x;
            const auto& style = ImGui::GetStyle();
            const float space_w = ImGui::CalcTextSize(" ").x;

            // emit_inline_chunk: render `text` (via fn) and decide whether
            // the NEXT chunk's `next_w` fits on the same line.
            auto fits = [&](float next_w) {
                return ImGui::GetItemRectMax().x + style.ItemSpacing.x + next_w < right_edge_x;
            };

            bool first_meta = true;
            // SameLine() before each chunk except the very first; we
            // place every chunk via a fits() check on the previous item.
            auto maybe_sameline = [&]() {
                if (!first_meta) ImGui::SameLine();
                first_meta = false;
            };

            std::string by_text;
            if (!d.author_handle.empty()) by_text = std::string("by ") + d.author_handle;
            if (!by_text.empty()) {
                maybe_sameline();
                ImGui::TextDisabled("%s", by_text.c_str());
            }

            // No "[Your upload]" tag here — that was Browse-mode chrome.
            // Your own uploads live in the My uploads view now, where
            // every row is yours so the tag would be redundant.

            for (std::size_t ci = 0; ci < d.categories.size(); ++ci) {
                std::string chip = std::string("#") + category_label(d.categories[ci]);
                const float w = ImGui::CalcTextSize(chip.c_str()).x;
                if (!first_meta && !fits(w)) {
                    ImGui::NewLine();
                    first_meta = true;
                }
                maybe_sameline();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", chip.c_str());
            }

            // Report affordance — only in Browse mode, and only on
            // rows the user does NOT own. My uploads view skips this
            // because you can't report your own drills.
            if (!my_uploads && !d.is_mine) {
                const float w = ImGui::CalcTextSize("Report").x + style.FramePadding.x * 2.0f;
                if (!first_meta && !fits(w + space_w)) {
                    ImGui::NewLine();
                    first_meta = true;
                }
                maybe_sameline();
                if (ImGui::SmallButton("Report##rep")) {
                    g_browse.report_target_id = d.id;
                    g_browse.report_target_name = d.name;
                    g_browse.report_reason_buf[0] = 0;
                    g_browse.report_modal_open_requested = true;
                }
            }

            // Description: snippet / expanded / missing-placeholder.
            // Rendered inside the Name cell so the row height grows
            // with the content. ImGui::TextWrapped uses the cell's
            // available width, which we narrow slightly via the
            // table column proportions above.
            if (has_desc) {
                if (expanded) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.85f, 1));
                    ImGui::TextWrapped("%s", d.description.c_str());
                    ImGui::PopStyleColor();
                } else {
                    // Snippet: clip to ~160 chars + ellipsis. Avoids
                    // pulling in a wrap-then-clip helper for what's
                    // a one-line policy.
                    if (d.description.size() <= kSnippetLimit) {
                        ImGui::TextWrapped("%s", d.description.c_str());
                    } else {
                        std::string snip = d.description.substr(0, kSnippetLimit) + "...";
                        ImGui::TextWrapped("%s", snip.c_str());
                    }
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.50f, 0.55f, 1));
                ImGui::TextWrapped("No description.");
                ImGui::PopStyleColor();
            }

            if (small) ImGui::PopFont();

            // ---- Character ------------------------------------------
            ImGui::TableSetColumnIndex(1);
            if (!d.cpu_side.empty()) {
                ImGui::Text("%s (%s)", d.character.c_str(), d.cpu_side.c_str());
            } else {
                ImGui::TextUnformatted(d.character.c_str());
            }

            // ---- Difficulty badge ----------------------------------
            ImGui::TableSetColumnIndex(2);
            if (!d.difficulty.empty()) {
                ImGui::TextColored(difficulty_color(d.difficulty), "%s",
                                   difficulty_label(d.difficulty));
            } else {
                ImGui::TextDisabled("—");
            }

            // ---- Stats: recordings + counts ------------------------
            // Each stat sits on its own line so the column doesn't get
            // pushed out by a popular drill ("12345 saves / 67890 likes"
            // would have overflowed the previous single-line layout).
            // Large counts collapse to K / M to keep the column tight
            // at high scale — "1.2K saves" rather than "1234 saves".
            auto compact = [](long long n) -> std::string {
                char buf[32];
                if (n < 1000) {
                    std::snprintf(buf, sizeof(buf), "%lld", n);
                } else if (n < 1'000'000) {
                    std::snprintf(buf, sizeof(buf), "%.1fK", n / 1000.0);
                } else {
                    std::snprintf(buf, sizeof(buf), "%.1fM", n / 1'000'000.0);
                }
                return buf;
            };
            ImGui::TableSetColumnIndex(3);
            // Stats are tertiary information — render in the small font
            // so they don't dominate the column. Single push wraps all
            // three lines for one PushFont/PopFont pair.
            ImFont* stats_font = opendojo::render_hook::small_font();
            if (stats_font) ImGui::PushFont(stats_font);
            ImGui::Text("%d rec", d.recordings_count);
            ImGui::TextDisabled("%s %s", compact(d.downloads).c_str(),
                                d.downloads == 1 ? "save" : "saves");
            ImGui::TextDisabled("%s %s", compact(d.likes).c_str(), d.likes == 1 ? "like" : "likes");
            if (stats_font) ImGui::PopFont();

            // ---- Action columns ------------------------------------
            // Browse mode: Like + Download.
            // My uploads mode: Edit + Delete (owner-only actions).
            // Delete here uses the same confirmation modal as before;
            // Edit opens the edit modal pre-filled with this row's
            // current name + description.
            if (my_uploads) {
                ImGui::TableSetColumnIndex(4);
                if (ImGui::Button("Edit", ImVec2(-1, 0))) {
                    g_browse.edit_target_id = d.id;
                    g_browse.edit_target_original_name = d.name;
                    std::snprintf(g_browse.edit_name_buf, sizeof(g_browse.edit_name_buf), "%s",
                                  d.name.c_str());
                    std::snprintf(g_browse.edit_desc_buf, sizeof(g_browse.edit_desc_buf), "%s",
                                  d.description.c_str());
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
                ImGui::TableSetColumnIndex(5);
                if (destructive_button("Delete##rowdel", ImVec2(-1, 0))) {
                    g_browse.delete_target_id = d.id;
                    g_browse.delete_target_name = d.name;
                    g_browse.delete_modal_open_requested = true;
                }
            } else {
                ImGui::TableSetColumnIndex(4);
                const bool i_liked = liked_now.count(d.id) > 0;
                if (ImGui::Button(i_liked ? "Unlike" : "Like", ImVec2(-1, 0))) {
                    kick_toggle_like(d.id);
                }
                ImGui::TableSetColumnIndex(5);
                if (ImGui::Button("Download", ImVec2(-1, 0))) { kick_download(d.id, d.name); }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ---- Delete confirmation modal -----------------------------------
    // OpenPopup must be called at the same ID-stack level as
    // BeginPopupModal; in-row clicks set the flag and we open here.
    if (g_browse.delete_modal_open_requested) {
        g_browse.delete_modal_open_requested = false;
        ImGui::OpenPopup("DeleteCloudDrill");
    }
    if (ImGui::BeginPopupModal("DeleteCloudDrill", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete this drill?");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s",
                           g_browse.delete_target_name.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Other players will lose access immediately. This can't be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
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
        ImGui::OpenPopup("EditCloudDrill");
    }
    // SetNextWindowSize must be called immediately before the matching
    // Begin* call — issuing it on the OpenPopup frame doesn't carry
    // over to the BeginPopupModal frame, which is why the previous
    // attempt left the modal at its auto-sized (skinny) default.
    // Width also bumped to give Description real room; ImGui's
    // InputTextMultiline does not word-wrap, so a wider box is the
    // only way to keep typical descriptions visible in one line.
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("EditCloudDrill", nullptr, 0)) {
        ImGui::Text("Edit drill");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", g_browse.edit_target_original_name.c_str());
        ImGui::Spacing();
        // Inputs stretch to the modal width so long lines have the
        // most horizontal room before scrolling.
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::InputText("##editname", g_browse.edit_name_buf, sizeof(g_browse.edit_name_buf));
        ImGui::PopItemWidth();
        ImGui::TextDisabled("Name (1-96 chars)");

        ImGui::Spacing();
        ImGui::InputTextMultiline("##editdesc", g_browse.edit_desc_buf,
                                  sizeof(g_browse.edit_desc_buf),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5 +
                                                       ImGui::GetStyle().FramePadding.y * 2));
        ImGui::TextDisabled(
            "Description (optional, up to 1000 chars — press Enter for paragraph breaks)");

        ImGui::Spacing();
        ImGui::TextDisabled("Tags (optional)");
        if (ImGui::BeginTable("editTags", 2,
                              ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
            for (int i = 0; i < kCategoryCount; ++i) {
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::Checkbox(kCategories[i].label, &g_browse.edit_cat_picks[i]);
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
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
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
        ImGui::OpenPopup("ReportCloudDrill");
    }
    if (ImGui::BeginPopupModal("ReportCloudDrill", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
            ImVec2(380, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2));
        ImGui::PopItemWidth();
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
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

    // ---- Tag chips in a 2-column grid so the second column lines
    // up across rows. SizingStretchSame splits the card width
    // evenly; NoPadOuterX keeps cells flush with the rest of the
    // card content. With 5 categories the last row's right cell is
    // empty — that's fine; the grid stays aligned.
    ImGui::TextDisabled("Tags (optional)");
    if (ImGui::BeginTable("upload_tags", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        for (int i = 0; i < kCategoryCount; ++i) {
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            ImGui::Checkbox(kCategories[i].label, &g_upload.category_picks[i]);
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

    const bool in_flight = g_upload.in_flight.load();
    const bool disabled = !can_export || in_flight;
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button(in_flight ? "Uploading..." : "Share to OpenDojo Cloud",
                      ImVec2(-FLT_MIN, 0))) {
        kick_upload(name ? name : "", description ? description : "");
    }
    opendojo::menu::nav_recenter();
    if (disabled) ImGui::EndDisabled();

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

}  // namespace opendojo::cloud::ui
