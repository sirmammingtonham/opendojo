#include "ui/menu.hpp"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "autosave.hpp"
#include "cloud/cloud_ui.hpp"
#include "commands.hpp"
#include "config.hpp"
#include "log.hpp"
#include "players.hpp"
#include "hooks/render_hook.hpp"
#include "slot.hpp"
#include "subsystems.hpp"
#include "ui/theme.hpp"

#include <windows.h>

namespace opendojo::menu {

namespace {

constexpr const char* OPENDOJO_VERSION = "v0.5";

using clock = std::chrono::steady_clock;

struct ToastState {
    std::string text;
    bool is_error = false;
    clock::time_point until;
    // Tab index the toast was raised on. The toast is hidden on every
    // other tab — switching tabs clears the toast from view, since the
    // action it confirms belonged to the originating tab.
    int origin_tab = -1;
};

struct State {
    bool drills_dirty = true;
    std::vector<opendojo::commands::DrillHeader> drills;

    // Drills tab: filter rows whose `character` doesn't match the live CPU
    // character. Disabled automatically when no CPU is detected.
    bool show_all_drills = false;

    // Drills tab sort. Autosaves are always pinned to the top regardless;
    // this only controls how the regular drills below them are ordered.
    enum class Sort {
        Name,
        Newest,
    };
    Sort sort_mode = Sort::Name;

    // Export form buffers.
    char export_name[96] = "";
    // Description allows newlines + a few paragraphs. Server caps at
    // 1000 chars; we give the buffer extra headroom for IME / pasted
    // text the user will trim before submitting.
    char export_description[1024] = "";

    // Set whenever the window transitions from hidden -> visible. Used
    // to claim window focus + set initial nav focus on the first frame
    // so keyboard nav can start without a mouse click.
    bool needs_focus = true;

    // Tab state for LB/RB cycling. `active_tab` tracks which tab is
    // currently drawn; `pending_tab` is set to -1 normally and to a
    // target index on the frame we want to force-select via
    // ImGuiTabItemFlags_SetSelected.
    int active_tab = 0;
    int pending_tab = -1;

    ToastState toast;

    // Pending local-drill deletion target — set when the user clicks
    // Delete on any drill in the Drills tab, consumed by the modal
    // popup. Path is the file to remove; name is shown in the prompt.
    // We use a deferred-open flag rather than calling OpenPopup at
    // the click site because the click is inside a PushID(idx) scope;
    // OpenPopup's id binds to the current ID stack, while the
    // BeginPopupModal at the tab root uses the unqualified id.
    std::filesystem::path delete_path;
    std::string delete_name;
    bool delete_modal_open_requested = false;
};

State g_state;

// Pending UI effects published from background threads (cloud worker)
// and drained at the top of each draw(). Keeping a single small mutex
// here rather than spreading thread-safety affordances across every
// piece of menu state.
struct PendingUiOps {
    std::mutex mtx;
    struct QueuedToast {
        std::string text;
        bool is_error;
    };
    std::vector<QueuedToast> toasts;
    std::atomic<bool> drills_dirty{false};
};
PendingUiOps g_pending;

void show_toast(std::string text, bool is_error = false) {
    g_state.toast.text = std::move(text);
    g_state.toast.is_error = is_error;
    g_state.toast.until = clock::now() + std::chrono::seconds(5);
    g_state.toast.origin_tab = g_state.active_tab;
}

// Small destructive-action button. Same shape as Button but
// hot-tinted so the eye notices it before the click. Used for
// Delete affordances in Drills + Cloud + confirmation modals.
bool destructive_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

// Same shape but for SmallButton (used by the autosave row).
bool destructive_small_button(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
    bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
    return clicked;
}

// Renders a small star icon at the current cursor position that toggles
// the drill's pinned state when clicked. Pinned drills float to the top
// of the Drills tab list. Pinned state shows a bright filled ★; unpinned
// shows a very dim ☆ — visible enough to advertise the affordance,
// quiet enough not to clutter every row. The button has no frame or
// background so it reads as an inline icon rather than another action.
//
// `filename` is the in-folder file name (with .drill.txt) used as the
// stable identity in config. Returns true if the pinned state changed.
bool pin_toggle_button(const char* filename, bool currently_pinned) {
    const char* icon = currently_pinned ? "\xE2\x98\x85" /* U+2605 ★ */
                                        : "\xE2\x98\x86" /* U+2606 ☆ */;
    const ImVec4 bright(1.00f, 0.82f, 0.30f, 1.00f);
    const ImVec4 dim(1.00f, 1.00f, 1.00f, 0.18f);
    ImGui::PushStyleColor(ImGuiCol_Text, currently_pinned ? bright : dim);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
    const bool clicked = ImGui::SmallButton(icon);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip(currently_pinned ? "Unpin" : "Pin to top"); }
    if (clicked) opendojo::config::set_drill_pinned(filename, !currently_pinned);
    return clicked;
}

void drain_pending_ui_ops() {
    std::vector<PendingUiOps::QueuedToast> toasts;
    {
        std::lock_guard lk(g_pending.mtx);
        toasts.swap(g_pending.toasts);
    }
    if (g_pending.drills_dirty.exchange(false)) { g_state.drills_dirty = true; }
    // If multiple toasts queued, the last one wins — that matches the
    // single-slot toast UI we already have.
    for (auto& t : toasts) {
        show_toast(std::move(t.text), t.is_error);
    }
}

// After a successful Add/Replace on a saved drill, copy its metadata
// into the Export form. Lets a user follow "load drill → tweak →
// re-export / upload to cloud" without retyping name + description.
// Autosaves intentionally skip this — their names are internal
// ("_autosave_jin") and would just be noise in the Export field.
void seed_export_form_from(const opendojo::commands::DrillHeader& d) {
    if (d.is_autosave) return;
    std::snprintf(g_state.export_name, sizeof(g_state.export_name), "%s", d.name.c_str());
    std::snprintf(g_state.export_description, sizeof(g_state.export_description), "%s",
                  d.description.c_str());
}

void refresh_drills_if_needed() {
    if (!g_state.drills_dirty) return;
    g_state.drills = opendojo::commands::list_drills();
    // Snapshot pinned set once so the comparator below stays O(log n)
    // per call instead of scanning the JSON array for each compare.
    auto pinned_list = opendojo::config::pinned_drills();
    std::set<std::string> pinned(pinned_list.begin(), pinned_list.end());
    // Order: autosaves, then pinned, then the rest — within each group
    // the user-chosen sort (Name or Newest) decides the order.
    std::sort(g_state.drills.begin(), g_state.drills.end(),
              [&pinned](const opendojo::commands::DrillHeader& a,
                        const opendojo::commands::DrillHeader& b) {
                  if (a.is_autosave != b.is_autosave) return a.is_autosave;
                  const bool pa = pinned.count(a.path.filename().string()) > 0;
                  const bool pb = pinned.count(b.path.filename().string()) > 0;
                  if (pa != pb) return pa;
                  if (g_state.sort_mode == State::Sort::Newest) {
                      // Filesystem mtime; descending (newer first).
                      return a.mtime > b.mtime;
                  }
                  return a.name < b.name;
              });
    g_state.drills_dirty = false;
    // The local library just changed (download / delete / export / copy);
    // let the Cloud tab re-scan so its "Downloaded" markers stay accurate
    // — e.g. deleting a downloaded drill re-enables its Download button.
    opendojo::cloud::ui::mark_local_library_dirty();
}

// ---- Tabs ------------------------------------------------------------------

void draw_drills_tab() {
    ImGui::TextDisabled(
        "Drills in opendojo/.  Add fills empty slots; Replace clears all slots first.");
    ImGui::Spacing();

    auto cpu = opendojo::players::detect_cpu();
    const bool can_filter = cpu.detected && !g_state.show_all_drills;

    // Count visible vs total under the current filter.
    std::size_t visible = 0;
    if (can_filter) {
        for (const auto& d : g_state.drills) {
            if (d.character == cpu.character_name) ++visible;
        }
    } else {
        visible = g_state.drills.size();
    }

    if (ImGui::Button("Refresh")) g_state.drills_dirty = true;
    nav_recenter();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("Sort:");
    ImGui::SameLine();
    {
        int sort_idx = static_cast<int>(g_state.sort_mode);
        if (ImGui::RadioButton("Name", &sort_idx, static_cast<int>(State::Sort::Name))) {
            g_state.sort_mode = State::Sort::Name;
            g_state.drills_dirty = true;
        }
        nav_recenter();
        ImGui::SameLine();
        if (ImGui::RadioButton("Newest", &sort_idx, static_cast<int>(State::Sort::Newest))) {
            g_state.sort_mode = State::Sort::Newest;
            g_state.drills_dirty = true;
        }
        nav_recenter();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (cpu.detected) {
        ImGui::Checkbox("Show all", &g_state.show_all_drills);
        nav_recenter();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (can_filter) {
            ImGui::TextDisabled("%zu of %zu drills (filtered to %s)", visible,
                                g_state.drills.size(), cpu.character_name.c_str());
        } else {
            ImGui::TextDisabled("%zu drills (CPU: %s)", g_state.drills.size(),
                                cpu.character_name.c_str());
        }
    } else {
        ImGui::TextDisabled("%zu drills (no CPU detected - filter disabled)",
                            g_state.drills.size());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_state.drills.empty()) {
        ImGui::TextDisabled("No drills saved yet. Use the Export tab to create one.");
        return;
    }

    // Split into autosaves (rendered first with distinct styling) and
    // regular drills (in a standard table).
    std::vector<std::size_t> autosave_idxs;
    std::vector<std::size_t> regular_idxs;
    for (std::size_t i = 0; i < g_state.drills.size(); ++i) {
        const auto& d = g_state.drills[i];
        if (can_filter && d.character != cpu.character_name) continue;
        if (d.is_autosave) {
            autosave_idxs.push_back(i);
        } else {
            regular_idxs.push_back(i);
        }
    }

    // ---- Autosaves block --------------------------------------------------
    if (!autosave_idxs.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Auto-saves");
        ImGui::TextDisabled(
            "Overwritten every time you switch characters or leave practice. "
            "Click \"Save as drill\" to keep a copy.");
        ImGui::Spacing();
        for (std::size_t idx : autosave_idxs) {
            const auto& d = g_state.drills[idx];
            ImGui::PushID(static_cast<int>(idx));
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "%s", d.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu rec)", d.recording_count);
            ImGui::SameLine();
            if (ImGui::Button("Add##autosave_add")) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::AppendToFree);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }
            nav_recenter();
            ImGui::SameLine();
            if (ImGui::Button("Replace##autosave_replace")) {
                auto r = opendojo::commands::load_drill(d.path,
                                                        opendojo::commands::LoadMode::ReplaceAll);
                if (r.ok) opendojo::subsystems::mark_session_loaded(true);
                show_toast(r.message, !r.ok);
            }
            nav_recenter();
            ImGui::SameLine();
            if (ImGui::Button("Save as drill##autosave_save")) {
                std::string new_name = d.character.empty() ? "saved" : d.character + "_saved";
                auto r = opendojo::commands::copy_drill(d.path, new_name);
                show_toast(r.message, !r.ok);
                if (r.ok) g_state.drills_dirty = true;
            }
            nav_recenter();
            ImGui::SameLine();
            if (destructive_button("Delete##autosave_delete")) {
                g_state.delete_path = d.path;
                g_state.delete_name = d.name;
                g_state.delete_modal_open_requested = true;
            }
            nav_recenter();
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ---- Regular drills table --------------------------------------------
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (regular_idxs.empty()) {
        if (!autosave_idxs.empty()) {
            ImGui::TextDisabled("No saved drills yet — use Export or \"Save as drill\" above.");
        }
    } else {
        // Slightly bumped vertical cell padding so SmallButton rows
        // don't read as cramped. 5 px is just enough breathing room
        // without making the table feel sparse.
        constexpr float kCellPadY = 5.0f;
        const float row_h = ImGui::GetTextLineHeightWithSpacing() + kCellPadY * 2;
        // 6 data rows + header.
        const ImVec2 table_size(0, row_h * 7);
        // Scale-aware width for the actions column — wide enough to fit
        // all three buttons inline plus padding between them.
        const auto pad = ImGui::GetStyle().FramePadding.x;
        const auto isp = ImGui::GetStyle().ItemSpacing.x;
        const float add_w = ImGui::CalcTextSize("Add").x + pad * 2;
        const float repl_w = ImGui::CalcTextSize("Replace").x + pad * 2;
        const float del_w = ImGui::CalcTextSize("Delete").x + pad * 2;
        const float actions_w = add_w + repl_w + del_w + isp * 2 + pad * 2;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                            ImVec2(ImGui::GetStyle().CellPadding.x, kCellPadY));
        if (ImGui::BeginTable("drills", 4, flags, table_size)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Recordings", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, actions_w);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (std::size_t idx : regular_idxs) {
                const auto& d = g_state.drills[idx];
                const std::string fname = d.path.filename().string();
                const bool pinned = opendojo::config::is_drill_pinned(fname);
                ImGui::PushID(static_cast<int>(idx));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (pin_toggle_button(fname.c_str(), pinned)) {
                    g_state.drills_dirty = true;  // re-sort: pin/unpin changes group
                }
                nav_recenter();
                ImGui::SameLine();
                ImGui::TextUnformatted(d.name.c_str());
                if (!d.description.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", d.description.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                if (!d.cpu_side.empty()) {
                    ImGui::Text("%s (%s)", d.character.c_str(), d.cpu_side.c_str());
                } else {
                    ImGui::TextUnformatted(d.character.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%zu", d.recording_count);

                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("Add##add")) {
                    auto r = opendojo::commands::load_drill(
                        d.path, opendojo::commands::LoadMode::AppendToFree);
                    if (r.ok) {
                        opendojo::subsystems::mark_session_loaded(true);
                        seed_export_form_from(d);
                    }
                    show_toast(r.message, !r.ok);
                }
                nav_recenter();
                ImGui::SameLine();
                if (ImGui::SmallButton("Replace##replace")) {
                    auto r = opendojo::commands::load_drill(
                        d.path, opendojo::commands::LoadMode::ReplaceAll);
                    if (r.ok) {
                        opendojo::subsystems::mark_session_loaded(true);
                        seed_export_form_from(d);
                    }
                    show_toast(r.message, !r.ok);
                }
                nav_recenter();
                ImGui::SameLine();
                if (destructive_small_button("Delete##delete")) {
                    g_state.delete_path = d.path;
                    g_state.delete_name = d.name;
                    g_state.delete_modal_open_requested = true;
                }
                nav_recenter();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    if (can_filter && visible == 0 && !g_state.drills.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1),
                           "No drills match %s. Toggle \"Show all\" to see every drill.",
                           cpu.character_name.c_str());
    }

    // ---- Delete confirmation modal --------------------------------------
    // OpenPopup must be called at the same ID-stack scope as
    // BeginPopupModal; the in-loop click sites set a flag instead
    // and we consume it here at the tab root.
    if (g_state.delete_modal_open_requested) {
        g_state.delete_modal_open_requested = false;
        ImGui::OpenPopup("DeleteLocalDrill");
    }
    if (ImGui::BeginPopupModal("DeleteLocalDrill", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete this drill?");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1), "%s", g_state.delete_name.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(
            "This removes the .drill.txt from your opendojo/ folder. "
            "This can't be undone.");
        ImGui::Spacing();

        if (ImGui::Button("Cancel", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (destructive_button("Delete", ImVec2(120, 0))) {
            auto r = opendojo::commands::delete_drill(g_state.delete_path);
            show_toast(r.message, !r.ok);
            if (r.ok) g_state.drills_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_recordings_tab() {
    const bool in_practice = opendojo::subsystems::in_practice();
    auto cpu = opendojo::players::detect_cpu();

    // Count populated slots up front so we can show "N/M recordings" on
    // the right of the status row and only render populated entries in
    // the table below.
    std::size_t populated = 0;
    if (in_practice) {
        for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
            if (opendojo::slot::is_populated(i)) ++populated;
        }
    }

    // Status row: practice state + detected character on the left,
    // "N/M recordings" right-aligned on the same line.
    ImGui::TextUnformatted("Status:");
    ImGui::SameLine();
    if (!in_practice) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "not in practice mode");
    } else if (cpu.detected) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s (CPU on %s)",
                           cpu.character_name.c_str(),
                           opendojo::players::side_to_string(cpu.cpu_side));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "ready");
    }
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu/%zu recordings", populated,
                      opendojo::slot::USER_SLOTS);
        const float text_w = ImGui::CalcTextSize(buf).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - text_w);
        ImGui::TextDisabled("%s", buf);
    }

    ImGui::Spacing();

    if (!in_practice) {
        ImGui::TextDisabled("Enter practice mode to record.");
    } else if (populated == 0) {
        ImGui::TextDisabled("No recordings yet. Record a move or pick one from the move list.");
    } else {
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("recordings", 3, flags)) {
            ImGui::TableSetupColumn("Recording", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (std::size_t i = 0; i < opendojo::slot::USER_SLOTS; ++i) {
                auto k = opendojo::slot::kind(i);
                if (k == opendojo::slot::Kind::Empty) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Recording %zu", i + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(opendojo::slot::kind_name(k));
                ImGui::TableSetColumnIndex(2);
                if (k == opendojo::slot::Kind::MoveList) {
                    ImGui::TextUnformatted("saved");
                } else {
                    ImGui::Text("%u events", static_cast<unsigned>(opendojo::slot::event_count(i)));
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Shared name + description for both Save and Share. Stretches
    // full width so users get a real text field, not a cramped
    // 420-pixel one carried over from the old single-column form.
    ImGui::PushItemWidth(-1);
    ImGui::InputText("##export_name", g_state.export_name, sizeof(g_state.export_name));
    nav_recenter();
    ImGui::PopItemWidth();
    ImGui::TextDisabled("Name (leave blank for an auto timestamp)");

    ImGui::Spacing();

    ImGui::InputTextMultiline(
        "##export_desc", g_state.export_description, sizeof(g_state.export_description),
        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2),
        ImGuiInputTextFlags_WordWrap);
    nav_recenter();
    ImGui::TextDisabled("Description (optional, up to 1000 chars)");

    ImGui::Spacing();
    ImGui::Spacing();

    // ---- Two-card layout: Save locally | Share with community ------------
    // Cards are equal width and equal height so neither feels secondary.
    // Card height is fixed to fit the taller of the two (the Share card
    // with tags + difficulty + handle + button + status line); the Save
    // card pads its top so its button bottom-aligns near the Share button.
    const bool can_export = populated > 0;

    // 2-column table for the cards instead of two BeginChild blocks.
    // The crucial difference: table cells render content INTO the
    // current window, so nav_recenter() called on widgets inside
    // (e.g. the Save button) drives the menu window's scroll. With
    // BeginChild the widgets sat in a no-scroll child window and our
    // SetScrollY calls had no effect on the parent — focused widgets
    // at the bottom of the tab couldn't snap the scrollbar to its rail.
    //
    // We use BordersInner/Outer for the card look and bump CellPadding
    // so content sits with the same breathing room a child window's
    // WindowPadding would have given it.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12.0f, 12.0f));
    const ImGuiTableFlags kCardTableFlags = ImGuiTableFlags_BordersInnerV |
                                            ImGuiTableFlags_BordersOuter |
                                            ImGuiTableFlags_SizingStretchSame;
    if (ImGui::BeginTable("cards", 2, kCardTableFlags)) {
        // -- Save locally card --
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Save locally");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Writes a .drill.txt file to your opendojo/ folder so you can "
            "load it again from the Drills tab.");
        ImGui::Spacing();
        if (!can_export) ImGui::BeginDisabled();
        if (ImGui::Button("Save drill", ImVec2(-FLT_MIN, 0))) {
            auto r = opendojo::commands::export_current_slots(
                g_state.export_name, g_state.export_description,
                "" /* character: always autodetected */, "" /* cpu_side: always use detection */);
            show_toast(r.message, !r.ok);
            if (r.ok) g_state.drills_dirty = true;
        }
        nav_recenter();
        if (!can_export) ImGui::EndDisabled();
        if (!can_export) { ImGui::TextDisabled("(record or pick a move first)"); }

        // -- Share with community card --
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Share with community");
        ImGui::Spacing();
        opendojo::cloud::ui::draw_share_card_body(can_export, g_state.export_name,
                                                  g_state.export_description);

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

// Render a user-readable label for a Win32 virtual-key code. Uses
// MapVirtualKey + GetKeyNameText for the OS-localized names so
// keyboards in any layout produce sensible labels. Falls back to
// "VK 0x??" for unmappable codes.
std::string vk_name(std::uint32_t vk) {
    char buf[64] = {};
    // Function keys + arrows have an "extended" bit Windows wants set
    // for proper naming.
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lparam = (scan & 0xFF) << 16;
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN: lparam |= (1 << 24); break;
    }
    if (GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0 && buf[0]) { return buf; }
    std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}

// Human label for an XInput button mask. Matches XINPUT_GAMEPAD_*
// values; the menu only ever shows one button at a time so we don't
// need to handle combined masks.
const char* pad_btn_name(std::uint16_t mask) {
    switch (mask) {
        case 0x1000: return "A";
        case 0x2000: return "B";
        case 0x4000: return "X";
        case 0x8000: return "Y";
        case 0x0010: return "Start";
        case 0x0020: return "Back";
        case 0x0001: return "DPad Up";
        case 0x0002: return "DPad Down";
        case 0x0004: return "DPad Left";
        case 0x0008: return "DPad Right";
        case 0x0100: return "LB";
        case 0x0200: return "RB";
        case 0x0040: return "LS";
        case 0x0080: return "RS";
        default: return "(unbound)";
    }
}

void draw_settings_tab() {
    ImGui::TextDisabled("Persisted to opendojo/config.json");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Autosave");
    ImGui::Spacing();
    {
        bool en = opendojo::autosave::is_enabled();
        if (ImGui::Checkbox("Autosave / autoload per character", &en)) {
            opendojo::autosave::set_enabled(en);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "Menu open binds");
    ImGui::Spacing();

    const auto vk = opendojo::config::toggle_vk();
    const auto pad_btn = opendojo::config::toggle_pad_btn();
    const bool capturing = opendojo::config::is_capturing();
    const bool pad_capturing = opendojo::config::is_pad_capturing();

    const ImGuiTableFlags bind_flags = ImGuiTableFlags_SizingStretchSame;
    if (ImGui::BeginTable("binds", 2, bind_flags)) {
        ImGui::TableNextRow();

        // ---- Keyboard column -------------------------------------------
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Keyboard");
        ImGui::Text("Current: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "%s", vk_name(vk).c_str());
        ImGui::Spacing();
        if (!capturing) {
            if (ImGui::Button("Rebind...##kbd")) { opendojo::config::start_capture(); }
            ImGui::SameLine();
            if (ImGui::Button("Reset##kbd")) {
                opendojo::config::set_toggle_vk(VK_F12);
                show_toast("Toggle key reset to F12");
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Press any key... (Esc cancels)");
            auto pressed = opendojo::config::consume_captured_vk();
            if (pressed != 0) {
                opendojo::config::set_toggle_vk(pressed);
                show_toast(std::string("Toggle key bound to ") + vk_name(pressed));
            }
        }

        // ---- Controller column -----------------------------------------
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Controller");
        ImGui::Text("Current: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.65f, 1), "Back + %s", pad_btn_name(pad_btn));
        ImGui::Spacing();
        if (!pad_capturing) {
            if (ImGui::Button("Rebind...##pad")) { opendojo::config::start_pad_capture(); }
            ImGui::SameLine();
            if (ImGui::Button("Reset##pad")) {
                opendojo::config::set_toggle_pad_btn(0x8000);  // XINPUT_GAMEPAD_Y
                show_toast("Controller chord reset to Back + Y");
            }
        } else {
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "Press any button... (Back cancels)");
            auto pressed_btn = opendojo::config::consume_captured_pad_btn();
            if (pressed_btn != 0) {
                opendojo::config::set_toggle_pad_btn(pressed_btn);
                show_toast(std::string("Controller chord bound to Back + ") +
                           pad_btn_name(pressed_btn));
            }
        }

        ImGui::EndTable();
    }

    // Cloud-related settings (currently just the author handle) live
    // in the cloud module; rendered here so they sit alongside the
    // other persisted preferences.
    opendojo::cloud::ui::draw_settings_section();
}

void draw_about_tab() {
    ImGui::Text("OpenDojo %s - Tekken 8 practice-mode drill tool", OPENDOJO_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Save and share practice-mode recordings as text drill files. "
        "Each drill contains one or more recordings; loading places them "
        "into the in-game recording slots.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Toggle this menu: %s  or  Back + %s  (both rebindable)",
                vk_name(opendojo::config::toggle_vk()).c_str(),
                pad_btn_name(opendojo::config::toggle_pad_btn()));
    ImGui::Spacing();
    ImGui::TextDisabled("Drill files + settings live in opendojo/ next to the game executable.");
    if (ImGui::Button("Open opendojo/ folder")) {
        // Launch the system explorer.exe with the folder as its
        // argument. Avoiding ShellExecute keeps us off shell32 and
        // off the verb-resolution / file-association code path —
        // we always invoke a known absolute exe with a single arg
        // we control. Path is the game-dir/opendojo subfolder; NTFS
        // disallows '"' so simple quoting is safe. Errors are
        // ignored; the user can fall back to Steam's "Browse local
        // files" if launch fails.
        wchar_t windir[MAX_PATH];
        UINT n = GetSystemWindowsDirectoryW(windir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring exe = std::wstring(windir) + L"\\explorer.exe";
            std::wstring cmd = L"\"" + exe + L"\" \"" + opendojo::commands::drills_dir().wstring() +
                               L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
}

// ---- Toast (transient bottom-of-window status line) ------------------------

void draw_toast() {
    if (g_state.toast.text.empty()) return;
    if (clock::now() > g_state.toast.until) {
        g_state.toast.text.clear();
        return;
    }
    // Toast is bound to the tab it was raised on. Switching tabs hides
    // it — the action it confirmed belongs to the originating tab, and
    // a stale "Downloaded: foo" lingering on the Settings tab would be
    // confusing. We clear rather than just skip rendering so a switch
    // back doesn't re-surface a stale toast.
    if (g_state.toast.origin_tab != g_state.active_tab) {
        g_state.toast.text.clear();
        return;
    }
    ImGui::Spacing();
    const ImVec4 col = g_state.toast.is_error ? ImVec4(1.0f, 0.55f, 0.40f, 1.0f)
                                              : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
    ImGui::TextColored(col, "%s", g_state.toast.text.c_str());
}

}  // anonymous namespace

void invalidate() {
    g_state.drills_dirty = true;
    // Window just became visible: reclaim focus + initial nav target.
    g_state.needs_focus = true;
}

void queue_toast(std::string text, bool is_error) {
    std::lock_guard lk(g_pending.mtx);
    g_pending.toasts.push_back({std::move(text), is_error});
}

void queue_drills_refresh() {
    g_pending.drills_dirty.store(true);
}

// Public — called from cloud_ui.cpp's share-card widgets too. See the
// header for behavior. Single static tracks last-focused id across
// every call site in the process; at most one IsItemFocused() returns
// true per frame, so a single tracker is sufficient.
void nav_recenter() {
    if (!ImGui::IsItemFocused() || !ImGui::GetIO().NavActive) return;
    static ImGuiID s_last = 0;
    const ImGuiID id = ImGui::GetItemID();
    if (id == 0 || id == s_last) return;
    s_last = id;

    const float win_h = ImGui::GetWindowHeight();
    const float scroll_max = ImGui::GetScrollMaxY();
    if (scroll_max <= 0.0f) return;
    const float item_top = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y +
                           ImGui::GetScrollY();
    const float item_h = ImGui::GetItemRectSize().y;
    float target = item_top + item_h * 0.5f - win_h * 0.5f;
    if (target < 0.0f) target = 0.0f;
    if (target > scroll_max) target = scroll_max;
    ImGui::SetScrollY(target);
}

void draw() {
    drain_pending_ui_ops();
    refresh_drills_if_needed();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    // Size as a fraction of the display so the menu remains readable
    // at every resolution. Clamped so it doesn't get absurdly small
    // on tiny windows or absurdly large on ultrawide.
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    // Width minimum bumped from 900 to 1100 so the Cloud tab's
    // character combo + tag chip row + sort/difficulty filters fit
    // on one row without wrapping. Height was bumped from 0.60/540
    // so the Export tab's two cards + form sit fully in view on
    // standard 1080p displays without forcing a window scroll —
    // makes gamepad nav much easier since fewer focused widgets
    // park at the viewport edge.
    const ImVec2 size(clampf(vp->WorkSize.x * 0.55f, 1100.0f, 1500.0f),
                      clampf(vp->WorkSize.y * 0.72f, 700.0f, 1150.0f));
    const ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f,
                     vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);

    if (g_state.needs_focus) {
        // SetNextWindowFocus only takes effect once Begin runs. Pair it
        // with SetItemDefaultFocus below on the first interactive widget
        // so the keyboard nav cursor starts somewhere visible.
        ImGui::SetNextWindowFocus();
    }

    const ImGuiWindowFlags wflags = ImGuiWindowFlags_NoCollapse;
    // Operator broadcast (e.g. "New update available") rides in the
    // title bar after the version. Kick the fetch + read the cached
    // text here; both are cheap and safe to call every frame. The
    // visible portion changes when a message arrives, but the window
    // identity stays pinned by the trailing ###opendojo tag so window
    // state (position, size, focus) survives the title change.
    opendojo::cloud::ui::poll_service_message();
    const std::string svc_msg = opendojo::cloud::ui::service_message();
    std::string title = std::string("OpenDojo ") + OPENDOJO_VERSION;
    if (!svc_msg.empty()) title += "   |   " + svc_msg;
    title += "###opendojo";

    // Pass an `open` bool so ImGui draws the X close button. We treat
    // a click on X identically to the toggle hotkey.
    bool open = true;
    if (!ImGui::Begin(title.c_str(), &open, wflags)) {
        ImGui::End();
        if (!open) opendojo::render_hook::toggle_menu();
        return;
    }

    // Gamepad L1/R1 cycle through tabs (rising-edge). The events were
    // fed earlier this frame by render_hook::poll_gamepad, so checking
    // here picks them up in time for the tab bar to honor SetSelected.
    struct TabDef {
        const char* name;
        void (*draw)();
    };
    const TabDef tabs[] = {
        {"Drills", &draw_drills_tab},     {"Cloud", &opendojo::cloud::ui::draw_cloud_tab},
        {"Export", &draw_recordings_tab}, {"Settings", &draw_settings_tab},
        {"About", &draw_about_tab},
    };
    constexpr int kTabCount = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        g_state.pending_tab = (g_state.active_tab - 1 + kTabCount) % kTabCount;
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        g_state.pending_tab = (g_state.active_tab + 1) % kTabCount;
    }

    if (ImGui::BeginTabBar("##tabs")) {
        for (int i = 0; i < kTabCount; ++i) {
            ImGuiTabItemFlags flags = 0;
            if (i == g_state.pending_tab) flags |= ImGuiTabItemFlags_SetSelected;
            const bool tab_open = ImGui::BeginTabItem(tabs[i].name, nullptr, flags);
            // Snap window scroll to the top when nav focus lands on a
            // tab item (e.g. user DPad-up'd past all content). Tab items
            // sit at the top of the window's content area, so this
            // resolves the "nav to top tabs leaves a vertical gap"
            // gap that ImGui's default scroll-into-view would leave.
            nav_recenter();
            if (tab_open) {
                g_state.active_tab = i;
                // First-frame focus on the initial tab: anchor the nav
                // cursor so keyboard/gamepad can move around immediately.
                if (g_state.needs_focus && i == 0) ImGui::SetKeyboardFocusHere();
                tabs[i].draw();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g_state.pending_tab = -1;

    draw_toast();
    ImGui::End();

    if (!open) opendojo::render_hook::toggle_menu();

    g_state.needs_focus = false;
}

}  // namespace opendojo::menu
