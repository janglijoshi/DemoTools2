// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
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

// Window layout pieces (2.0): sidebar navigation, page header, section titles, small tags and number tiles.
enum Icon { Icon_Camera, Icon_Cinematics, Icon_Record, Icon_Skins, Icon_Status };
void DrawIcon(ImDrawList* dl, int icon, ImVec2 center, float size, ImU32 col);
// One sidebar entry: icon, label, a muted line under it; badge > 0 shows a small count on the right.
bool NavItem(const char* id, int icon, const char* label, const char* sub, bool selected, int badge = 0, const ImVec4& badgeCol = ImVec4(0, 0, 0, 0));
void PageHeader(const char* title, const char* sub, const char* tag = nullptr, const ImVec4& tagCol = ImVec4(0, 0, 0, 0));
void Section(const char* title);   // bold title with a thin rule to the right edge
void Tag(const char* text, const ImVec4& col);   // small rounded label on the current line
void StatTile(const char* value, const char* label, const ImVec4& col, float width);
}  // namespace Ui
