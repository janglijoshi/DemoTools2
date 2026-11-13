// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
// DemoTools2 window look and small widgets: theme, fonts, smooth scrolling, animated toggles, segmented buttons and
// list rows. Everything here runs on the window thread only.
#include "imgui.h"
#include <cstdint>

namespace Ui {
extern float scale;                // DPI scale of the window (1 = 96 dpi)
extern ImFont* bold;               // Segoe UI Semibold (or the body font)
inline float S(float px) { return px * scale; }

// palette
extern const ImVec4 kText, kMuted, kAccent, kGood, kWarn, kBad, kTeamT, kTeamCT, kBg, kPanel, kFrame;
ImU32 Col(const ImVec4& c, float alpha = 1.f);

void LoadFonts();                  // once, before the first frame
void ApplyTheme(float dpiScale);   // at start and when the window moves to a screen with another DPI

// Mouse wheel: the window procedure hands notches over here instead of to ImGui; scrollable areas glide to the target.
void AddWheel(float notches);
void BeginFrame();                 // call after ImGui::NewFrame
void EndFrame();                   // call before ImGui::Render
void SmoothScroll();               // right after BeginChild / BeginCombo / BeginPopup of a scrollable area

float Anim(ImGuiID id, float target, float speed = 14.f);   // eases a value towards target, per frame
float FadeIn(const char* key, uint64_t contentKey);         // 0..1, restarts when contentKey changes

bool Toggle(const char* label, bool* v);
bool Segmented(const char* id, const char* const* items, int count, int* value);
void Heading(const char* text);
void StatusPill(const char* text, const ImVec4& color);
// One list row: optional team chip, left text, right text (own colour), a thin colour bar on the left (rarity), and a
// small dot (e.g. "holding now"). Returns true when clicked.
struct RowOpts {
    const char* right = nullptr;
    ImU32 rightCol = 0;
    ImU32 leftCol = 0;
    ImU32 bar = 0;
    const char* chip = nullptr;
    ImU32 chipCol = 0;
    ImU32 dot = 0;
    float width = 0;   // 0 = the full width
};
bool Row(const char* id, const char* left, bool selected, const RowOpts& o = {});
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));
}  // namespace Ui
