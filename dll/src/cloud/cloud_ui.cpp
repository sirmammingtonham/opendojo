#include "cloud_ui.hpp"

#include "imgui.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../commands.hpp"
#include "../menu.hpp"
#include "../players.hpp"
#include "api.hpp"
#include "cloud.hpp"
#include "worker.hpp"

namespace opendojo::cloud::ui {

namespace {

// Shared between the render thread (read for drawing) and the cloud
// worker thread (write on completion). Every field is guarded by
// `mtx`; we copy out to locals for the draw pass to keep lock scope
// short.
struct BrowseState {
    std::mutex mtx;
    std::vector<opendojo::cloud::api::DrillSummary> results;
    bool loading = false;
    std::string error;

    // User-controlled inputs live on the render thread, so they don't
    // need mtx. ImGui owns the buffer storage.
    char search_buf[96] = "";
    bool filter_to_cpu = true;
    int sort_idx = 0;  // 0 = Newest, 1 = Most downloaded
    int offset = 0;
    bool initial_load_done = false;
};

BrowseState g_browse;

// Two flags rather than one — the upload may be kicked off the Export
// tab or anywhere else later. The render thread observes this to show
// a "uploading..." disabled state on the button.
struct UploadState {
    std::atomic<bool> in_flight{false};
};
UploadState g_upload;

const char* kSortLabels[] = {"Newest", "Most downloaded"};

opendojo::cloud::api::SortOrder sort_from_idx(int i) {
    return i == 1 ? opendojo::cloud::api::SortOrder::MostDownloaded
                  : opendojo::cloud::api::SortOrder::NewestFirst;
}

void kick_list() {
    opendojo::cloud::api::ListQuery q;
    q.search_query = g_browse.search_buf;
    if (g_browse.filter_to_cpu) {
        auto cpu = opendojo::players::detect_cpu();
        if (cpu.detected) q.character_filter = cpu.character_name;
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

    g_upload.in_flight.store(true);
    opendojo::cloud::worker::submit([payload = std::move(p)]() {
        opendojo::cloud::api::SubmitArgs args;
        args.name = payload.name;
        args.description = payload.description;
        args.character = payload.character;
        args.cpu_side = payload.cpu_side;
        args.recordings_count = payload.recordings_count;
        args.content = payload.text;

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
            "If you built the DLL yourself, pass -DOPENDOJO_SUPABASE_URL=... and "
            "-DOPENDOJO_SUPABASE_ANON_KEY=... at configure time. See "
            "supabase/README.md in the repo.");
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
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    auto cpu = opendojo::players::detect_cpu();
    if (cpu.detected) {
        if (ImGui::Checkbox("Filter to current CPU", &g_browse.filter_to_cpu)) {
            g_browse.offset = 0;
            kick_list();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", cpu.character_name.c_str());
    } else {
        bool disabled = true;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Filter to current CPU (no CPU detected)", &disabled);
        ImGui::EndDisabled();
    }

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

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("cloud_drills", 5, flags, ImVec2(0, 360))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Recordings", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Downloads", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        const auto pad = ImGui::GetStyle().FramePadding.x;
        const float dl_w = ImGui::CalcTextSize("Download").x + pad * 4;
        ImGui::TableSetupColumn("Download", ImGuiTableColumnFlags_WidthFixed, dl_w);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            const auto& d = snapshot[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(d.name.c_str());
            if (!d.description.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", d.description.c_str());
            }
            if (!d.author_handle.empty()) { ImGui::TextDisabled("by %s", d.author_handle.c_str()); }

            ImGui::TableSetColumnIndex(1);
            if (!d.cpu_side.empty()) {
                ImGui::Text("%s (%s)", d.character.c_str(), d.cpu_side.c_str());
            } else {
                ImGui::TextUnformatted(d.character.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", d.recordings_count);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%lld", static_cast<long long>(d.downloads));

            ImGui::TableSetColumnIndex(4);
            if (ImGui::Button("Download", ImVec2(-1, 0))) { kick_download(d.id, d.name); }
            ImGui::PopID();
        }
        ImGui::EndTable();
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
    ImGui::TextDisabled(
        "5 uploads/day per user. Drills are deduplicated by content — "
        "re-uploading the same drill is free.");
}

}  // namespace opendojo::cloud::ui
