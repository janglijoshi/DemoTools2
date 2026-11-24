// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// DemoTools2 window look: a dark gunmetal theme with a brass accent, Segoe UI text that stays sharp at any DPI,
// mouse-wheel scrolling that glides instead of jumping, and a few animated widgets.
#include "ui.h"
#include "imgui_internal.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace Ui {
float scale = 1.f;
ImFont* bold = nullptr;

static ImVec4 Hex(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
const ImVec4 kBg = Hex(0x0F1216), kPanel = Hex(0x14181E), kFrame = Hex(0x1B2029);
const ImVec4 kText = Hex(0xE3E6EB), kMuted = Hex(0x858E9C);
const ImVec4 kAccent = Hex(0xE0A33E);
const ImVec4 kGood = Hex(0x5CC48A), kWarn = Hex(0xE08A4A), kBad = Hex(0xE06464);
const ImVec4 kTeamT = Hex(0xD3A964), kTeamCT = Hex(0x6E9BE0);

ImU32 Col(const ImVec4& c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// ------------------------------------------------------------------ fonts
static bool Exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(win, MAX_PATH);
    auto path = [&](const char* file) { return std::string(win) + "\\Fonts\\" + file; };
    // CJK player names: Microsoft YaHei / Malgun Gothic / Yu Gothic fill in glyphs Segoe UI doesn't have.
    auto addFallbacks = [&]() {
        for (const char* f : {"msyh.ttc", "malgun.ttf", "YuGothM.ttc", "seguisym.ttf"}) {
            std::string p = path(f);
            if (!Exists(p.c_str())) continue;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(p.c_str(), 16.f, &cfg);
        }
    };
    ImFont* body = nullptr;
    std::string p = path("segoeui.ttf");
    if (Exists(p.c_str())) {
        body = io.Fonts->AddFontFromFileTTF(p.c_str(), 16.f);
        if (body) addFallbacks();
    }
    if (!body) body = io.Fonts->AddFontDefault();
    for (const char* f : {"seguisb.ttf", "segoeuib.ttf"}) {
        std::string b = path(f);
        if (!Exists(b.c_str())) continue;
        bold = io.Fonts->AddFontFromFileTTF(b.c_str(), 16.f);
        if (bold) { addFallbacks(); break; }
    }
    if (!bold) bold = body;
    io.FontDefault = body;
}

// ------------------------------------------------------------------ theme
void ApplyTheme(float dpiScale) {
    scale = dpiScale > 0.5f ? dpiScale : 1.f;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.FontSizeBase = 16.f;
    st.FontScaleDpi = scale;

    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(8, 8);
    st.ItemInnerSpacing = ImVec2(7, 5);
    st.CellPadding = ImVec2(7, 4);
    st.IndentSpacing = 16;
    st.ScrollbarSize = 10;
    st.GrabMinSize = 12;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 1;
    st.TabBorderSize = 0;
    st.WindowRounding = 0;
    st.ChildRounding = 10;
    st.FrameRounding = 6;
    st.PopupRounding = 10;
    st.ScrollbarRounding = 6;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.SeparatorTextBorderSize = 1;
    st.SelectableTextAlign = ImVec2(0, 0.5f);

    ImVec4* c = st.Colors;
    const ImVec4 frameHi = Hex(0x242A35), frameAct = Hex(0x2B323F), border = Hex(0x232933);
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = kPanel;
    c[ImGuiCol_PopupBg] = Hex(0x191D24, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = frameAct;
    c[ImGuiCol_TitleBg] = kBg;
    c[ImGuiCol_TitleBgActive] = kBg;
    c[ImGuiCol_TitleBgCollapsed] = kBg;
    c[ImGuiCol_MenuBarBg] = kPanel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Hex(0x2E3440);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x3A4250);
    c[ImGuiCol_ScrollbarGrabActive] = Hex(0x4A5363);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = Hex(0xF0B85A);
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = Mix(frameHi, kAccent, 0.12f);
    c[ImGuiCol_ButtonActive] = Mix(frameAct, kAccent, 0.22f);
    c[ImGuiCol_Header] = Hex(0x1B2028);
    c[ImGuiCol_HeaderHovered] = frameHi;
    c[ImGuiCol_HeaderActive] = frameAct;
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = Mix(border, kAccent, 0.5f);
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = Mix(border, kAccent, 0.5f);
    c[ImGuiCol_ResizeGripActive] = kAccent;
    c[ImGuiCol_InputTextCursor] = kAccent;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = Hex(0x1D222B);
    c[ImGuiCol_TabSelected] = Hex(0x1B2027);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = Hex(0x1B2027);
    c[ImGuiCol_TabDimmedSelectedOverline] = Mix(kAccent, kMuted, 0.4f);
    c[ImGuiCol_TableHeaderBg] = kFrame;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = border;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Hex(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextSelectedBg] = Mix(kPanel, kAccent, 0.35f);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);
    st.ScaleAllSizes(scale);
}

// ------------------------------------------------------------------ smooth scrolling
static float g_pendingWheel = 0;   // from the window procedure
static float g_wheel = 0;          // this frame's, until a scrollable area takes it
struct ScrollState { float target = 0, current = 0; int frame = -10; };
static std::unordered_map<ImGuiID, ScrollState> g_scroll;

void AddWheel(float notches) { g_pendingWheel += notches; }
void BeginFrame() { g_wheel = g_pendingWheel; g_pendingWheel = 0; }
void EndFrame() {
    // Nobody scrollable under the mouse took it (a slider, a non-scrolling panel): give it to ImGui as usual.
    if (g_wheel != 0) ImGui::GetIO().AddMouseWheelEvent(0, g_wheel);
    g_wheel = 0;
    if (g_scroll.size() > 512) g_scroll.clear();
}

void SmoothScroll() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || w->SkipItems) return;
    const int frame = ImGui::GetFrameCount();
    ScrollState& s = g_scroll[w->ID];
    // New, not drawn last frame, or moved by something else (scrollbar drag, keyboard, "scroll to end"): follow it.
    if (s.frame != frame - 1 || std::fabs(w->Scroll.y - s.current) > 1.f) s.target = s.current = w->Scroll.y;
    s.frame = frame;
    const float maxY = w->ScrollMax.y;
    if (g_wheel != 0 && maxY > 0 && ImGui::IsWindowHovered()) {
        const float step = std::max(ImGui::GetTextLineHeightWithSpacing() * 3.f, w->InnerRect.GetHeight() * 0.12f);
        s.target -= g_wheel * step;
        g_wheel = 0;
    }
    s.target = std::clamp(s.target, 0.f, maxY);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    s.current += (s.target - s.current) * (1.f - std::exp(-dt * 18.f));
    if (std::fabs(s.target - s.current) < 0.5f) s.current = s.target;
    if (s.current != w->Scroll.y) ImGui::SetScrollY(s.current);
}

// ------------------------------------------------------------------ animation helpers
float Anim(ImGuiID id, float target, float speed) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float* v = st->GetFloatRef(id, target);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    *v += (target - *v) * (1.f - std::exp(-dt * speed));
    if (std::fabs(target - *v) < 0.002f) *v = target;
    return *v;
}

float FadeIn(const char* key, uint64_t contentKey) {
    struct F { uint64_t key = ~0ull; float t = 1.f; };
    static std::unordered_map<std::string, F> fades;
    F& f = fades[key];
    if (f.key != contentKey) { f.key = contentKey; f.t = 0; }
    f.t = std::min(1.f, f.t + std::min(ImGui::GetIO().DeltaTime, 0.05f) / 0.16f);
    const float e = 1.f - (1.f - f.t) * (1.f - f.t);   // ease out
    return e;
}

// ------------------------------------------------------------------ widgets
bool Toggle(const char* label, bool* v) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fh = ImGui::GetFrameHeight();
    const float th = std::round(fh * 0.62f), tw = std::round(th * 1.75f);
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(tw + (textSize.x > 0 ? st.ItemInnerSpacing.x + textSize.x : 0), fh));
    if (clicked) *v = !*v;
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    const float t = Anim(id, *v ? 1.f : 0.f, 16.f);
    const float h = Anim(id + 1, hovered ? 1.f : 0.f, 20.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (fh - th) * 0.5f), b(a.x + tw, a.y + th);
    ImVec4 track = Mix(Mix(Hex(0x2A303B), Hex(0x343B48), h), kAccent, t);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(track), th * 0.5f);
    const float r = th * 0.5f - S(2.5f);
    const ImVec2 knob(a.x + th * 0.5f + (tw - th) * t, a.y + th * 0.5f);
    dl->AddCircleFilled(knob, r, ImGui::GetColorU32(Mix(Hex(0xC9CED6), Hex(0x14171C), t)), 20);
    if (textSize.x > 0)
        ImGui::RenderText(ImVec2(b.x + st.ItemInnerSpacing.x, p.y + (fh - textSize.y) * 0.5f), label);
    return clicked;
}

bool Segmented(const char* id, const char* const* items, int count, int* value) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems || count <= 0) return false;
    ImGui::PushID(id);
    const float fh = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const float each = width / count;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + fh), Col(kFrame), rr);
    const float x = Anim(ImGui::GetID("##ind"), (*value < 0 ? 0 : *value) * each, 18.f);
    const float pad = S(2.f);
    if (*value >= 0 && *value < count) dl->AddRectFilled(ImVec2(p.x + x + pad, p.y + pad), ImVec2(p.x + x + each - pad, p.y + fh - pad), Col(Mix(kFrame, kAccent, 0.28f)), rr - 1);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + i * each, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(each, fh)) && *value != i) { *value = i; changed = true; }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 ts = ImGui::CalcTextSize(items[i], nullptr, true);
        const bool on = *value == i;
        const ImVec2 tp(p.x + i * each + (each - ts.x) * 0.5f, p.y + (fh - ts.y) * 0.5f);
        dl->AddText(tp, Col(on ? kText : (hov ? Mix(kMuted, kText, 0.6f) : kMuted)), items[i], ImGui::FindRenderedTextEnd(items[i]));
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, fh));
    ImGui::PopID();
    return changed;
}

void Heading(const char* text) {
    ImGui::PushFont(bold, 0.f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void StatusPill(const char* text, const ImVec4& color) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float fh = ImGui::GetFrameHeight();
    const float dot = S(4.f), padX = S(10.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(padX * 2 + dot * 2 + S(8) + ts.x, fh);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(color, 0.12f), fh * 0.5f);
    const float pulse = 0.75f + 0.25f * std::sin((float)ImGui::GetTime() * 3.f);
    dl->AddCircleFilled(ImVec2(p.x + padX + dot, p.y + fh * 0.5f), dot, Col(color, pulse), 12);
    dl->AddText(ImVec2(p.x + padX + dot * 2 + S(8), p.y + (fh - ts.y) * 0.5f), Col(color), text);
    ImGui::Dummy(size);
}

bool Row(const char* id, const char* left, bool selected, const RowOpts& o) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetTextLineHeight() + S(11.f);
    const float width = o.width > 0 ? o.width : ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, S(2.f)));
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.f), h));
    ImGui::PopStyleVar();
    if (!ImGui::IsItemVisible()) return pressed;
    const ImGuiID iid = ImGui::GetItemID();
    const bool hov = ImGui::IsItemHovered();
    const float ht = Anim(iid, hov ? 1.f : 0.f, 22.f);
    const float sel = Anim(iid + 1, selected ? 1.f : 0.f, 18.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + width, p.y + h);
    const float rr = S(6.f);
    if (ht > 0.01f) dl->AddRectFilled(p, q, Col(ImVec4(1, 1, 1, 0.045f * ht)), rr);
    if (sel > 0.01f) {
        dl->AddRectFilled(p, q, Col(kAccent, 0.15f * sel), rr);
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + S(3.f), q.y - h * 0.22f), Col(kAccent, sel), S(2.f));
    } else if (o.bar) {
        dl->AddRectFilled(ImVec2(p.x + S(1.f), p.y + h * 0.28f), ImVec2(p.x + S(3.f), q.y - h * 0.28f), o.bar, S(1.f));
    }
    float x = p.x + S(11.f);
    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    if (o.chip) {
        ImGui::PushFont(bold, st.FontSizeBase * 0.8f);
        const ImVec2 cs = ImGui::CalcTextSize(o.chip);
        const float cw = std::max(cs.x + S(10.f), S(28.f)), ch = cs.y + S(3.f);
        const ImVec2 c0(x, p.y + (h - ch) * 0.5f);
        ImVec4 cc = ImGui::ColorConvertU32ToFloat4(o.chipCol);
        dl->AddRectFilled(c0, ImVec2(c0.x + cw, c0.y + ch), Col(cc, 0.18f), S(4.f));
        dl->AddText(ImVec2(c0.x + (cw - cs.x) * 0.5f, c0.y + (ch - cs.y) * 0.5f), o.chipCol, o.chip);
        ImGui::PopFont();
        x += cw + S(8.f);
    }
    // The left text (the item's name) keeps its full width up to 55% of the row; the right text gets the rest.
    const float dotW = o.dot ? S(12.f) : 0;
    const float rightEdge = q.x - S(10.f) - dotW;
    const float leftW = ImGui::CalcTextSize(left, nullptr, true).x;
    const bool hasRight = o.right && *o.right;
    float leftMax = rightEdge;
    if (hasRight) {
        const float rightW = ImGui::CalcTextSize(o.right).x;
        const float room = rightEdge - x;
        leftMax = x + std::min(leftW, std::max(room - rightW - S(14.f), room * 0.55f));
    }
    if (o.leftCol) ImGui::PushStyleColor(ImGuiCol_Text, o.leftCol);
    ImGui::RenderTextEllipsis(dl, ImVec2(x, ty), ImVec2(leftMax, q.y), leftMax, left, ImGui::FindRenderedTextEnd(left), nullptr);
    if (o.leftCol) ImGui::PopStyleColor();
    if (hasRight) {
        const float rightW = ImGui::CalcTextSize(o.right).x;
        const float rightMin = std::max(leftMax + S(14.f), rightEdge - rightW);
        if (rightMin < rightEdge - S(8.f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, o.rightCol ? o.rightCol : Col(kMuted));
            ImGui::RenderTextEllipsis(dl, ImVec2(rightMin, ty), ImVec2(rightEdge, q.y), rightEdge, o.right, nullptr, nullptr);
            ImGui::PopStyleColor();
        }
    }
    if (o.dot) dl->AddCircleFilled(ImVec2(q.x - S(13.f), p.y + h * 0.5f), S(3.5f), o.dot, 12);
    return pressed;
}

// ------------------------------------------------------------------ layout pieces
// Small line icons, drawn (no icon font needed), in a box `size` wide around `center`.
void DrawIcon(ImDrawList* dl, int icon, ImVec2 c, float size, ImU32 col) {
    const float h = size * 0.5f, t = std::max(1.f, S(1.6f));
    switch (icon) {
        case Icon_Camera: {   // body, lens, viewfinder bump
            dl->AddRect(ImVec2(c.x - h, c.y - h * 0.55f), ImVec2(c.x + h, c.y + h * 0.75f), col, S(2.5f), 0, t);
            dl->AddRectFilled(ImVec2(c.x - h * 0.45f, c.y - h * 0.85f), ImVec2(c.x + h * 0.05f, c.y - h * 0.55f), col, S(1.f));
            dl->AddCircle(ImVec2(c.x, c.y + h * 0.1f), h * 0.38f, col, 16, t);
            break;
        }
        case Icon_Cinematics: {   // a camera path through three keyframes
            const ImVec2 a(c.x - h, c.y + h * 0.6f), b(c.x, c.y - h * 0.1f), d(c.x + h, c.y - h * 0.7f);
            dl->AddBezierCubic(a, ImVec2(c.x - h * 0.4f, c.y + h * 0.9f), ImVec2(c.x - h * 0.3f, c.y - h * 0.2f), b, col, t, 16);
            dl->AddBezierCubic(b, ImVec2(c.x + h * 0.3f, c.y + h * 0.0f), ImVec2(c.x + h * 0.4f, c.y - h * 0.9f), d, col, t, 16);
            for (const ImVec2& p : {a, b, d}) dl->AddCircleFilled(p, S(2.4f), col, 10);
            break;
        }
        case Icon_Record: {   // ring + dot
            dl->AddCircle(c, h * 0.92f, col, 20, t);
            dl->AddCircleFilled(c, h * 0.45f, col, 16);
            break;
        }
        case Icon_Skins: {   // a cut gem
            const ImVec2 p[] = {ImVec2(c.x - h * 0.55f, c.y - h * 0.7f), ImVec2(c.x + h * 0.55f, c.y - h * 0.7f), ImVec2(c.x + h, c.y - h * 0.15f),
                                ImVec2(c.x, c.y + h * 0.9f), ImVec2(c.x - h, c.y - h * 0.15f)};
            dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, t);
            dl->AddLine(ImVec2(c.x - h, c.y - h * 0.15f), ImVec2(c.x + h, c.y - h * 0.15f), col, t);
            dl->AddLine(ImVec2(c.x - h * 0.3f, c.y - h * 0.15f), ImVec2(c.x, c.y + h * 0.9f), col, t);
            dl->AddLine(ImVec2(c.x + h * 0.3f, c.y - h * 0.15f), ImVec2(c.x, c.y + h * 0.9f), col, t);
            break;
        }
        case Icon_Status: {   // pulse line
            const ImVec2 p[] = {ImVec2(c.x - h, c.y), ImVec2(c.x - h * 0.45f, c.y), ImVec2(c.x - h * 0.2f, c.y - h * 0.75f),
                                ImVec2(c.x + h * 0.15f, c.y + h * 0.75f), ImVec2(c.x + h * 0.4f, c.y), ImVec2(c.x + h, c.y)};
            dl->AddPolyline(p, 6, col, ImDrawFlags_None, t);
            break;
        }
    }
}

bool NavItem(const char* id, int icon, const char* label, const char* sub, bool selected, int badge, const ImVec4& badgeCol) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const float lh = ImGui::GetTextLineHeight();
    const float h = lh * 2.f + S(16.f);
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    const ImGuiID iid = ImGui::GetItemID();
    const float hv = Anim(iid, ImGui::IsItemHovered() ? 1.f : 0.f, 22.f);
    const float sel = Anim(iid + 1, selected ? 1.f : 0.f, 16.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + width, p.y + h);
    const float rr = S(8.f);
    if (hv > 0.01f) dl->AddRectFilled(p, q, Col(ImVec4(1, 1, 1, 0.04f * hv)), rr);
    if (sel > 0.01f) {
        dl->AddRectFilled(p, q, Col(kAccent, 0.11f * sel), rr);
        const float bh = (h * 0.5f) * sel;
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.5f - bh * 0.5f), ImVec2(p.x + S(3.f), p.y + h * 0.5f + bh * 0.5f), Col(kAccent, sel), S(2.f));
    }
    const float ix = p.x + S(24.f);
    DrawIcon(dl, icon, ImVec2(ix, p.y + h * 0.5f), S(17.f), Col(Mix(Mix(kMuted, kText, hv * 0.5f), kAccent, sel)));
    const float tx = p.x + S(46.f);
    const float right = q.x - S(10.f) - (badge > 0 ? S(26.f) : 0);
    ImGui::PushFont(bold, 0.f);
    dl->PushClipRect(p, ImVec2(right, q.y), true);
    dl->AddText(ImVec2(tx, p.y + S(7.f)), Col(Mix(Mix(kMuted, kText, 0.75f), kText, std::max(sel, hv))), label);
    ImGui::PopFont();
    if (sub && *sub) dl->AddText(ImVec2(tx, p.y + S(9.f) + lh), Col(kMuted, 0.9f), sub);
    dl->PopClipRect();
    if (badge > 0) {
        char b[8];
        std::snprintf(b, sizeof b, "%d", std::min(badge, 99));
        const ImVec2 ts = ImGui::CalcTextSize(b);
        const float bw = std::max(ts.x + S(10.f), S(20.f)), bhh = ts.y + S(2.f);
        const ImVec2 b0(q.x - S(10.f) - bw, p.y + (h - bhh) * 0.5f);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bhh), Col(badgeCol, 0.22f), bhh * 0.5f);
        dl->AddText(ImVec2(b0.x + (bw - ts.x) * 0.5f, b0.y + S(1.f)), Col(badgeCol), b);
    }
    return pressed;
}

void Tag(const char* text, const ImVec4& col) {
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 0.78f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float padX = S(7.f), hh = ts.y + S(4.f);
    const float fh = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 a(p.x, p.y + (fh - hh) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + ts.x + padX * 2, a.y + hh), Col(col, 0.16f), S(4.f));
    dl->AddText(ImVec2(a.x + padX, a.y + S(2.f)), Col(col), text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x + padX * 2, fh));
}

void PageHeader(const char* title, const char* sub, const char* tag, const ImVec4& tagCol) {
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (tag) {
        ImGui::SameLine(0, S(10.f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4.f));
        Tag(tag, tagCol);
    }
    if (sub && *sub) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(sub);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4.f)));
}

void Section(const char* title) {
    ImGui::Dummy(ImVec2(0, S(8.f)));
    ImGui::PushFont(bold, 0.f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    const float x0 = p.x + ts.x + S(10.f), x1 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (x1 > x0 + S(8.f)) ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f), Col(kMuted, 0.22f), 1.f);
    ImGui::Dummy(ImVec2(0, S(1.f)));
}

void StatTile(const char* value, const char* label, const ImVec4& col, float width) {
    const float lh = ImGui::GetTextLineHeight();
    const ImVec2 size(width, lh * 2.6f + S(18.f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(kFrame), S(8.f));
    dl->AddRectFilled(ImVec2(p.x, p.y + S(10.f)), ImVec2(p.x + S(3.f), p.y + size.y - S(10.f)), Col(col), S(2.f));
    ImGui::PushFont(bold, ImGui::GetStyle().FontSizeBase * 1.6f);
    dl->AddText(ImVec2(p.x + S(14.f), p.y + S(7.f)), Col(col), value);
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + S(14.f), p.y + size.y - lh - S(8.f)), Col(kMuted), label);
    ImGui::Dummy(size);
}

bool PrimaryButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, Mix(kFrame, kAccent, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0xF0B85A));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Hex(0xC98E2E));
    ImGui::PushStyleColor(ImGuiCol_Text, Hex(0x15181D));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}
}  // namespace Ui
