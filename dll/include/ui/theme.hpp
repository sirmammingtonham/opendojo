#pragma once

// Tekken-flavored ImGui styling. Dark near-black backgrounds with a crimson
// accent, sharp corners, dense layout. Call once after ImGui::CreateContext
// and before the first frame.

namespace opendojo::theme {

void apply();

// Tekken-red accent used by buttons / headers / selection highlights.
// Exposed so individual widgets can pull the same color when needed.
constexpr float ACCENT_R = 0.78f;
constexpr float ACCENT_G = 0.10f;
constexpr float ACCENT_B = 0.14f;

}  // namespace opendojo::theme
