#include "cloud_ui.hpp"

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

#include "../commands.hpp"
#include "../menu.hpp"
#include "../players.hpp"
#include "api.hpp"
#include "cloud.hpp"
#include "handle.hpp"
#include "worker.hpp"

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
//   1      = "current CPU (...)" — auto-detected, disabled when none
//   2..N+1 = explicit roster pick — index into players::character_roster()
constexpr int kCharComboAll = 0;
constexpr int kCharComboCpu = 1;
constexpr int kCharComboRosterBase = 2;

// Cached copy of the sorted character list. Built once on first call;
// keeps draw_browse_tab from hitting the loop in character_roster()
// every frame. The list only changes if the DLL is rebuilt with new
// ids in players::character_name_internal.
const std::vector<std::string>& roster() {
    static const std::vector<std::string> g_roster = opendojo::players::character_roster();
    return g_roster;
}

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

    // User-controlled inputs live on the render thread, so they don't
    // need mtx. ImGui owns the buffer storage.
    char search_buf[96] = "";
    int character_combo_idx = kCharComboCpu;  // default: match current CPU
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
};

BrowseState g_browse;

// Upload-side state. The Tag/Difficulty pickers in the Export tab
// write here; kick_upload reads at submit time.
struct UploadState {
    std::atomic<bool> in_flight{false};
    bool category_picks[kCategoryCount] = {false, false, false, false, false};
    int difficulty_idx = 0;  // 0 = (none)
};
UploadState g_upload;

const char* kSortLabels[] = {"Newest", "Most downloaded", "Most liked"};

opendojo::cloud::api::SortOrder sort_from_idx(int i) {
    using S = opendojo::cloud::api::SortOrder;
    switch (i) {
        case 1: return S::MostDownloaded;
        case 2: return S::MostLiked;
        default: return S::NewestFirst;
    }
}

// Resolve the character-combo selection to the actual filter string
// the API expects. Returns "" if "All" is selected or if "current CPU"
// is selected but no CPU is detected.
std::string resolve_character_filter() {
    int idx = g_browse.character_combo_idx;
    if (idx == kCharComboAll) return {};
    if (idx == kCharComboCpu) {
        auto cpu = opendojo::players::detect_cpu();
        return cpu.detected ? cpu.character_name : std::string{};
    }
    int rosterIdx = idx - kCharComboRosterBase;
    const auto& r = roster();
    if (rosterIdx >= 0 && rosterIdx < static_cast<int>(r.size())) return r[rosterIdx];
    return {};
}

void kick_list() {
    opendojo::cloud::api::ListQuery q;
    q.search_query = g_browse.search_buf;
    q.character_filter = resolve_character_filter();
    for (int i = 0; i < kCategoryCount; ++i) {
        if (g_browse.category_filter[i]) q.category_filter.emplace_back(kCategories[i].id);
    }
    if (g_browse.difficulty_filter_idx > 0 && g_browse.difficulty_filter_idx <= kDifficultyCount) {
        q.difficulty_filter = kDifficulties[g_browse.difficulty_filter_idx - 1].id;
    }
    q.sort = sort_from_idx(g_browse.sort_idx);
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
            opendojo::menu::queue_toast(r.error_message.empty() ? "like failed" : r.error_message,
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
            opendojo::menu::queue_toast(r.error_message.empty() ? "report failed" : r.error_message,
                                        true);
            return;
        }
        if (r.reported) {
            opendojo::menu::queue_toast("Reported: " + display_name, false);
        } else {
            opendojo::menu::queue_toast("Already reported — thanks.", false);
        }
    });
}

void kick_delete_my_drill(const std::string& drill_id, const std::string& display_name) {
    opendojo::cloud::worker::submit([drill_id, display_name]() {
        auto r = opendojo::cloud::api::delete_my_drill(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty() ? "delete failed" : r.error_message,
                                        true);
            return;
        }
        if (!r.deleted) {
            opendojo::menu::queue_toast(
                r.error_message.empty() ? "drill not found or not yours" : r.error_message, true);
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

void kick_download(const std::string& drill_id, const std::string& display_name) {
    opendojo::cloud::worker::submit([drill_id, display_name]() {
        auto r = opendojo::cloud::api::get_drill(drill_id);
        if (!r.ok) {
            opendojo::menu::queue_toast(
                r.error_message.empty() ? "download failed" : r.error_message, true);
            return;
        }
        auto save = opendojo::commands::save_drill_text(
            display_name.empty() ? r.drill.name : display_name, r.drill.content);
        if (!save.ok) {
            opendojo::menu::queue_toast("downloaded but " + save.message, true);
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
    auto p = opendojo::commands::build_current_slots_payload(name_in, description_in);
    if (!p.ok) {
        opendojo::menu::queue_toast(p.message, true);
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
    std::string version = opendojo::cloud::game_version();
    // Resolve the author handle once on the render thread. current()
    // touches steam_api64.dll via GetProcAddress; safer to do it here
    // where DLL state is well-defined than from the worker thread.
    std::string author = opendojo::cloud::handle::current();

    g_upload.in_flight.store(true);
    opendojo::cloud::worker::submit([payload = std::move(p),
                                     categories = std::move(picked_categories),
                                     difficulty = std::move(picked_difficulty),
                                     version = std::move(version), author = std::move(author)]() {
        opendojo::cloud::api::SubmitArgs args;
        args.name = payload.name;
        args.description = payload.description;
        args.character = payload.character;
        args.cpu_side = payload.cpu_side;
        args.recordings_count = payload.recordings_count;
        args.content = payload.text;
        args.categories = categories;
        args.difficulty = difficulty;
        args.game_version = version;
        args.author_handle = author;

        auto r = opendojo::cloud::api::submit_drill(args);
        g_upload.in_flight.store(false);
        if (!r.ok) {
            opendojo::menu::queue_toast(r.error_message.empty() ? "upload failed" : r.error_message,
                                        true);
            return;
        }
        if (r.deduped) {
            opendojo::menu::queue_toast("Already on OpenDojo Cloud — nice.", false);
        } else {
            opendojo::menu::queue_toast("Uploaded to OpenDojo Cloud", false);
        }
    });
}

}  // namespace

void draw_browse_tab() {
    if (!opendojo::cloud::configured()) {
        ImGui::TextDisabled("OpenDojo Cloud is not configured in this build.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Source builds ship without cloud access — Browse and Upload are "
            "disabled. Everything else works normally.");
        return;
    }

    // First time the tab renders, kick off a default search so the
    // user lands on a populated list instead of an empty pane.
    if (!g_browse.initial_load_done) {
        g_browse.initial_load_done = true;
        kick_list();
    }

    ImGui::TextDisabled("Search community drills");
    ImGui::Spacing();

    // ---- Row 1: search box + Search + Clear --------------------------------
    ImGui::PushItemWidth(360);
    bool submitted = ImGui::InputText("##search", g_browse.search_buf, sizeof(g_browse.search_buf),
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
    auto cpu = opendojo::players::detect_cpu();
    ImGui::Spacing();
    ImGui::TextDisabled("Character:");
    ImGui::SameLine();
    {
        // Build the visible label of the currently-selected entry so
        // the combo header reads sensibly without rebuilding the list
        // every frame.
        std::string current_label;
        int idx = g_browse.character_combo_idx;
        if (idx == kCharComboAll) {
            current_label = "All characters";
        } else if (idx == kCharComboCpu) {
            current_label = cpu.detected ? "current CPU (" + cpu.character_name + ")"
                                         : "current CPU (none detected)";
        } else {
            int ri = idx - kCharComboRosterBase;
            const auto& r = roster();
            current_label = (ri >= 0 && ri < static_cast<int>(r.size())) ? r[ri] : "?";
        }

        ImGui::PushItemWidth(200);
        if (ImGui::BeginCombo("##character", current_label.c_str())) {
            if (ImGui::Selectable("All characters", idx == kCharComboAll)) {
                g_browse.character_combo_idx = kCharComboAll;
                g_browse.offset = 0;
                kick_list();
            }
            // "current CPU" — only enabled when we have a detection.
            if (cpu.detected) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "current CPU (%s)", cpu.character_name.c_str());
                if (ImGui::Selectable(buf, idx == kCharComboCpu)) {
                    g_browse.character_combo_idx = kCharComboCpu;
                    g_browse.offset = 0;
                    kick_list();
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::Selectable("current CPU (none detected)", false);
                ImGui::EndDisabled();
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
    ImGui::PushItemWidth(140);
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
    ImGui::PushItemWidth(160);
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
        ImGui::TextDisabled("No drills found. Try clearing filters or searching.");
        return;
    }

    // Snapshot liked-session set + current game version once so the
    // table loop reads consistent values.
    std::set<std::string> liked_now;
    {
        std::lock_guard lk(g_browse.mtx);
        liked_now = g_browse.liked_session;
    }
    const std::string& current_version = opendojo::cloud::game_version();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("cloud_drills", 6, flags, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.8f);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Stats", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        const auto pad = ImGui::GetStyle().FramePadding.x;
        const float like_w = ImGui::CalcTextSize("Unlike").x + pad * 4;
        const float dl_w = ImGui::CalcTextSize("Download").x + pad * 4;
        ImGui::TableSetupColumn("Like", ImGuiTableColumnFlags_WidthFixed, like_w);
        ImGui::TableSetupColumn("Download", ImGuiTableColumnFlags_WidthFixed, dl_w);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& d = snapshot[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // ---- Name cell: expand caret + name + meta + description ----
            ImGui::TableSetColumnIndex(0);
            const bool expanded = g_browse.expanded_ids.count(d.id) > 0;
            const bool has_desc = !d.description.empty();
            // Reserve caret space even when there's nothing to expand,
            // so the names align vertically across rows.
            if (has_desc) {
                if (ImGui::ArrowButton("##exp", expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
                    if (expanded)
                        g_browse.expanded_ids.erase(d.id);
                    else
                        g_browse.expanded_ids.insert(d.id);
                }
            } else {
                // Phantom of the same size; keeps the indent.
                float fs = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(fs, fs));
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(d.name.c_str());

            // Meta line: "by author" / "Your upload" pill / #tags.
            bool any_meta = false;
            if (!d.author_handle.empty()) {
                ImGui::TextDisabled("by %s", d.author_handle.c_str());
                any_meta = true;
            }
            if (d.is_mine) {
                if (any_meta) ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "[Your upload]");
                any_meta = true;
            }
            for (std::size_t ci = 0; ci < d.categories.size(); ++ci) {
                if (any_meta || ci > 0) ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "#%s",
                                   category_label(d.categories[ci]));
                any_meta = true;
            }
            // Report affordance — shown on every row the user does NOT
            // own. SmallButton fits inline; click opens the reason
            // modal at the tab root.
            if (!d.is_mine) {
                if (any_meta) ImGui::SameLine();
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
                    constexpr std::size_t kSnippetLimit = 160;
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

            // ---- Character + game version --------------------------
            ImGui::TableSetColumnIndex(1);
            if (!d.cpu_side.empty()) {
                ImGui::Text("%s (%s)", d.character.c_str(), d.cpu_side.c_str());
            } else {
                ImGui::TextUnformatted(d.character.c_str());
            }
            if (!d.game_version.empty()) {
                const bool mismatch = !current_version.empty() && d.game_version != current_version;
                ImGui::TextColored(mismatch ? ImVec4(1.0f, 0.55f, 0.40f, 1)
                                            : ImVec4(0.65f, 0.65f, 0.65f, 1),
                                   "v%s", d.game_version.c_str());
                if (mismatch && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Recorded on Tekken v%s — your build targets v%s. "
                        "Loading may fail or play back incorrectly.",
                        d.game_version.c_str(), current_version.c_str());
                }
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
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d rec", d.recordings_count);
            ImGui::TextDisabled("%lld dl / %lld likes", static_cast<long long>(d.downloads),
                                static_cast<long long>(d.likes));

            // ---- Like toggle ---------------------------------------
            ImGui::TableSetColumnIndex(4);
            const bool i_liked = liked_now.count(d.id) > 0;
            if (ImGui::Button(i_liked ? "Unlike" : "Like", ImVec2(-1, 0))) {
                kick_toggle_like(d.id);
            }

            // ---- Download (+ owner-only Delete stacked beneath) ----
            ImGui::TableSetColumnIndex(5);
            if (ImGui::Button("Download", ImVec2(-1, 0))) { kick_download(d.id, d.name); }
            if (d.is_mine) {
                if (destructive_button("Delete##rowdel", ImVec2(-1, 0))) {
                    g_browse.delete_target_id = d.id;
                    g_browse.delete_target_name = d.name;
                    g_browse.delete_modal_open_requested = true;
                }
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
        ImGui::TextWrapped(
            "Other players will lose access immediately. Their likes "
            "and downloads are removed too. This can't be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (destructive_button("Delete", ImVec2(120, 0))) {
            kick_delete_my_drill(g_browse.delete_target_id, g_browse.delete_target_name);
            ImGui::CloseCurrentPopup();
        }
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
        ImGui::TextWrapped(
            "Tell us what's wrong (optional). The admin reviews reports; "
            "a drill with several reports is auto-hidden pending review.");
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

    ImGui::Spacing();
    ImGui::TextDisabled(
        "Downloaded drills land in opendojo/ alongside your local ones — "
        "use the Drills tab to load them.");
}

void draw_upload_export_row(bool can_export, const char* name, const char* description) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Share");
    ImGui::Spacing();

    if (!opendojo::cloud::configured()) {
        ImGui::TextDisabled(
            "Upload to OpenDojo Cloud is disabled — this DLL was built without "
            "cloud credentials.");
        return;
    }

    // ---- Tagging form (categories + difficulty). Everything here
    // is optional — uploads with zero tags / no difficulty go through
    // fine. Persists across uploads so a user grinding through ten
    // "punishment" drills in a row doesn't re-check each box.
    ImGui::TextDisabled("Tags (optional, pick any):");
    ImGui::SameLine();
    for (int i = 0; i < kCategoryCount; ++i) {
        ImGui::PushID(i);
        ImGui::Checkbox(kCategories[i].label, &g_upload.category_picks[i]);
        ImGui::PopID();
        if (i + 1 < kCategoryCount) ImGui::SameLine();
    }

    ImGui::TextDisabled("Difficulty (optional):");
    ImGui::SameLine();
    ImGui::PushItemWidth(160);
    ImGui::Combo("##upload_diff", &g_upload.difficulty_idx, kUploadDifficultyLabels,
                 IM_ARRAYSIZE(kUploadDifficultyLabels));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Game version: v%s", opendojo::cloud::game_version().c_str());

    ImGui::Spacing();

    bool in_flight = g_upload.in_flight.load();
    bool disabled = !can_export || in_flight;

    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button(in_flight ? "Uploading..." : "Upload to OpenDojo Cloud")) {
        kick_upload(name ? name : "", description ? description : "");
    }
    if (disabled) ImGui::EndDisabled();

    if (!can_export) {
        ImGui::SameLine();
        ImGui::TextDisabled("(record or pick a move first)");
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
    ImGui::SameLine();
    if (ImGui::Button("Clear##handle")) {
        handle_buf[0] = 0;
        opendojo::cloud::handle::set("");
    }

    auto effective = opendojo::cloud::handle::current();
    if (!effective.empty()) {
        ImGui::Text("Uploading as:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "%s", effective.c_str());
    } else {
        ImGui::TextDisabled("Uploads will be anonymous until you set a handle.");
    }
}

}  // namespace opendojo::cloud::ui
