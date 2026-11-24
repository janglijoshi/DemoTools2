// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// The DemoTools2 window: a sidebar with five pages.
//   Camera      follow / attach the camera to a player, every calc live, presets, and a dt2_ command line
//   Cinematics  campath sequences (built with the demo paused), path keys, quick moves, hotkeys, recording the path
//   Record      every kill of one player from the playing demo, with motion blur, as MP4
//   Skins       pick a player, pick a gun / knife / gloves / agent, pick the finish
//   Status      what was found in this CS2 build
#include "calc.h"
#include "campath.h"
#include "pov.h"
#include "viewmodel.h"
#include "record.h"
#include "hotkeys.h"
#include <windows.h>
#include <commdlg.h>
#include "catalog.gen.h"
#include "console.h"
#include "extras.gen.h"
#include "game.h"
#include "imgui.h"
#include "skins.h"
#include "ui.h"

#include <algorithm>
#include <shellapi.h>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

namespace Menu {
static void ManualRecordButton(const char* id);
enum Slot : int { SLOT_NONE = 0, SLOT_KNIFE = -1, SLOT_GLOVES = -2, SLOT_AGENT = -3 };   // > 0: a gun's definition index
static const int kCatKnives = 5, kCatGloves = 6;

static uint64_t g_player = 0;
static int g_slot = SLOT_NONE;
static char g_search[64] = {};
static char g_addXuid[24] = {};
static int g_side = Side_Both;        // which loadout the middle and right columns edit
static int g_stickerSlot = 0;
static char g_extraSearch[64] = {};
static ULONGLONG g_saveAt = 0;
static int g_gunTab = 0;            // gun editor: 0 finish, 1 stickers, 2 charm

static const CatWeapon* WeaponByDef(uint16_t def) {
    for (int i = 0; i < kNumWeapons; ++i) if (kWeapons[i].def == def) return &kWeapons[i];
    return nullptr;
}
static const char* ItemName(uint16_t def) {
    if (def == 42 || def == 59) return "Default knife";
    const CatWeapon* w = WeaponByDef(def);
    return w ? w->name : nullptr;
}
static const CatSkin* SkinBy(uint16_t def, int paint) {
    for (int i = 0; i < kNumSkins; ++i) if (kSkins[i].def == def && kSkins[i].paint == paint) return &kSkins[i];
    return nullptr;
}
static ImVec4 RarityColor(int r) {
    switch (r) {
        case 1: return ImVec4(0.69f, 0.76f, 0.85f, 1);
        case 2: return ImVec4(0.37f, 0.60f, 0.85f, 1);
        case 3: return ImVec4(0.29f, 0.41f, 1.00f, 1);
        case 4: return ImVec4(0.53f, 0.28f, 1.00f, 1);
        case 5: return ImVec4(0.83f, 0.17f, 0.90f, 1);
        case 6: return ImVec4(0.92f, 0.29f, 0.29f, 1);
        case 7: return ImVec4(0.89f, 0.68f, 0.22f, 1);
        default: return ImVec4(0.8f, 0.8f, 0.8f, 1);
    }
}
// Case-insensitive "needle in hay", without allocating (the sticker list checks thousands of names each frame).
static bool Contains(const char* hay, const char* needle) {
    if (!*needle) return true;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
static const CatAgent* AgentBy(const std::string& path) {
    for (int i = 0; i < kNumAgents; ++i) if (!_stricmp(kAgents[i].model, path.c_str())) return &kAgents[i];
    return nullptr;
}
static const char* AgentName(const std::string& path) {
    if (path.empty()) return nullptr;
    const CatAgent* a = AgentBy(path);
    return a ? a->name : nullptr;
}
static const CatSticker* StickerBy(int id) {
    for (int i = 0; i < kNumStickers; ++i) if ((int)kStickers[i].id == id) return &kStickers[i];
    return nullptr;
}
static const CatCharm* CharmBy(int id) {
    for (int i = 0; i < kNumCharms; ++i) if ((int)kCharms[i].id == id) return &kCharms[i];
    return nullptr;
}
static void Changed() {
    g_saveAt = GetTickCount64() + 600;
}
static const PlayerInfo* InDemo(uint64_t xuid) {
    for (auto& p : Skins::players) if (p.xuid == xuid) return &p;
    return nullptr;
}

// ------------------------------------------------------------------ shared bits
static ImU32 RarityU32(int r) { return ImGui::GetColorU32(RarityColor(r)); }
static ImU32 TeamU32(int team) { return Ui::Col(team == 2 ? Ui::kTeamT : team == 3 ? Ui::kTeamCT : Ui::kMuted); }
static const char* TeamChip(int team) { return team == 2 ? "T" : team == 3 ? "CT" : "-"; }

// A search box with a hint, the full width.
static bool SearchBox(const char* id, const char* hint, char* buf, size_t size) {
    ImGui::SetNextItemWidth(-1);
    return ImGui::InputTextWithHint(id, hint, buf, size);
}
// A scrollable list area that glides with the mouse wheel.
static void BeginList(const char* id, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(6), Ui::S(6)));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    Ui::SmoothScroll();
}
static void EndList() { ImGui::EndChild(); }
static void Hint(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextV(fmt, ap);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(ap);
}
static void SubHeading(const char* text) {
    ImGui::Dummy(ImVec2(0, Ui::S(2)));
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ left: players
static void PlayersColumn() {
    Ui::Heading("Players");
    Hint(Skins::demoPlaying ? "In this demo" : "Play a demo to see its players");
    ImGui::Spacing();
    for (auto& p : Skins::players) {
        auto r = Skins::rules.find(p.xuid);
        const bool has = r != Skins::rules.end() && !r->second.Empty();
        char id[48];
        std::snprintf(id, sizeof(id), "##pl%llu", (unsigned long long)p.xuid);
        Ui::RowOpts o;
        o.chip = TeamChip(p.team);
        o.chipCol = TeamU32(p.team);
        o.dot = has ? Ui::Col(Ui::kAccent) : 0;
        if (Ui::Row(id, p.name.c_str(), g_player == p.xuid, o)) { g_player = p.xuid; g_slot = SLOT_NONE; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("XUID %llu%s", (unsigned long long)p.xuid, has ? "\nHas skins set" : "");
    }
    if (Skins::demoPlaying && Skins::players.empty()) Hint("No players found yet.");

    bool header = false;
    for (auto& [xuid, pr] : Skins::rules) {
        if (pr.Empty() || InDemo(xuid)) continue;
        if (!header) { ImGui::Spacing(); SubHeading("Saved, not in this demo"); header = true; }
        char id[48];
        std::snprintf(id, sizeof(id), "##sv%llu", (unsigned long long)xuid);
        const std::string name = pr.name.empty() ? std::to_string(xuid) : pr.name;
        Ui::RowOpts o;
        o.leftCol = Ui::Col(Ui::kMuted);
        o.dot = Ui::Col(Ui::kAccent, 0.6f);
        if (Ui::Row(id, name.c_str(), g_player == xuid, o)) { g_player = xuid; g_slot = SLOT_NONE; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("XUID %llu", (unsigned long long)xuid);
    }

    ImGui::Spacing();
    SubHeading("Add a player by XUID");
    ImGui::SetNextItemWidth(-Ui::S(56));
    const bool enter = ImGui::InputTextWithHint("##xuid", "7656119...", g_addXuid, sizeof(g_addXuid),
                                                ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(-1, 0)) || enter) {
        uint64_t x = std::strtoull(g_addXuid, nullptr, 10);
        if (x > 76561197960265728ull) { Skins::rules[x]; g_player = x; g_slot = SLOT_NONE; g_addXuid[0] = 0; }
    }
}

// ------------------------------------------------------------------ middle: what to change
static void SlotRow(int slot, const char* name, const char* right, ImU32 rightCol, bool inHand) {
    char id[32];
    std::snprintf(id, sizeof(id), "##slot%d", slot);
    Ui::RowOpts o;
    o.right = right;
    o.rightCol = rightCol;
    o.dot = inHand ? Ui::Col(Ui::kGood) : 0;
    if (Ui::Row(id, name, g_slot == slot, o)) { g_slot = slot; g_search[0] = 0; g_extraSearch[0] = 0; }
    if (inHand && ImGui::IsItemHovered()) ImGui::SetTooltip("Holding it now");
}

static void GunsColumn() {
    if (!g_player) {
        Ui::Heading("Loadout");
        Hint("Pick a player on the left.");
        return;
    }
    PlayerRules& pr = Skins::rules[g_player];
    const PlayerInfo* info = InDemo(g_player);
    Ui::Heading(info ? info->name.c_str() : (pr.name.empty() ? "Saved player" : pr.name.c_str()));
    Hint("%llu%s", (unsigned long long)g_player, info ? (info->team == 2 ? "  ·  playing T now" : info->team == 3 ? "  ·  playing CT now" : "") : "");
    ImGui::Spacing();

    // Which loadout: both sides, or one side only. A side's own items win; what it doesn't set comes from "Both".
    {
        char t[24], ct[24];
        std::snprintf(t, sizeof(t), "T side%s", pr.t.Empty() ? "" : " \xE2\x80\xA2");
        std::snprintf(ct, sizeof(ct), "CT side%s", pr.ct.Empty() ? "" : " \xE2\x80\xA2");
        const char* items[] = {"Both sides", t, ct};
        if (Ui::Segmented("##side", items, 3, &g_side)) {
            g_search[0] = 0;
            g_extraSearch[0] = 0;
            if (g_side == Side_Both && g_slot == SLOT_AGENT) g_slot = SLOT_NONE;   // no agent on Both sides
        }
        if (g_side) Hint("Only while they play %s. Items marked \"both\" come from Both sides.", g_side == Side_T ? "T" : "CT");
        else Hint("Used on both sides unless the T or CT side sets its own. Agents are set on the T side and CT side tabs.");
    }
    ImGui::Spacing();
    Loadout& L = pr.For(g_side);
    const bool sideView = g_side != Side_Both;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(4), Ui::S(4)));
    ImGui::BeginChild("##slots", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PopStyleVar();
    Ui::SmoothScroll();

    // Knife, gloves and agent first: one each per player.
    {
        char name[128], right[160];
        const ItemRule& kn = L.knife.def || !sideView ? L.knife : pr.knife;
        const bool knBoth = sideView && !L.knife.def && pr.knife.def;
        const CatSkin* s = kn.def ? SkinBy(kn.def, kn.skin.paint) : nullptr;
        std::snprintf(name, sizeof(name), "%s", kn.def ? ItemName(kn.def) : (info && info->knife ? ItemName(info->knife) : "Knife"));
        std::snprintf(right, sizeof(right), "%s%s", s ? s->name : (kn.def ? "no finish" : "unchanged"), knBoth ? "  (both)" : "");
        SlotRow(SLOT_KNIFE, name, right, s ? RarityU32(s->rarity) : 0, false);

        const ItemRule& gl = L.gloves.def || !sideView ? L.gloves : pr.gloves;
        const bool glBoth = sideView && !L.gloves.def && pr.gloves.def;
        s = gl.def ? SkinBy(gl.def, gl.skin.paint) : nullptr;
        std::snprintf(name, sizeof(name), "%s", gl.def ? ItemName(gl.def) : (info && info->gloves && ItemName(info->gloves) ? ItemName(info->gloves) : "Gloves"));
        std::snprintf(right, sizeof(right), "%s%s", s ? s->name : (gl.def ? "no finish" : "unchanged"), glBoth ? "  (both)" : "");
        SlotRow(SLOT_GLOVES, name, right, s ? RarityU32(s->rarity) : 0, false);

        // Agents belong to one side, so they are only set on the T side and CT side tabs.
        if (sideView) {
            const CatAgent* a = L.agent.empty() ? nullptr : AgentBy(L.agent);
            SlotRow(SLOT_AGENT, "Agent", a ? a->name : "unchanged", a ? RarityU32(a->rarity) : 0, false);
        }
    }

    auto gunRow = [&](uint16_t def, bool inHand) {
        const char* n = ItemName(def);
        if (!n) return;
        auto r = L.guns.find(def);
        bool fromBoth = false;
        if (r == L.guns.end() && sideView) { r = pr.guns.find(def); fromBoth = r != pr.guns.end(); if (!fromBoth) r = L.guns.end(); }
        const bool has = fromBoth || r != L.guns.end();
        const CatSkin* s = has ? SkinBy(def, r->second.paint) : nullptr;
        char fin[200] = {};
        if (s) {
            int nSt = 0;
            for (int st : r->second.stickers) nSt += st != 0;
            std::snprintf(fin, sizeof(fin), "%s%s%s%s", s->name, nSt ? "  +stickers" : "", r->second.charm ? "  +charm" : "", fromBoth ? "  (both)" : "");
        }
        SlotRow(def, n, s ? fin : nullptr, s ? RarityU32(s->rarity) : 0, inHand);
    };
    if (info && !info->guns.empty()) {
        SubHeading("Holding now");
        std::vector<uint16_t> shown;
        for (uint16_t def : info->guns) {
            if (std::find(shown.begin(), shown.end(), def) != shown.end()) continue;
            shown.push_back(def);
            gunRow(def, true);
        }
    }
    ImGui::Spacing();
    for (int c = 0; c < kNumCategories; ++c) {
        if (c == kCatKnives || c == kCatGloves) continue;
        // how many guns of this category already have a skin on this side
        int set = 0;
        for (int i = 0; i < kNumWeapons; ++i) if (kWeapons[i].cat == c && L.guns.count(kWeapons[i].def)) ++set;
        char label[96];
        if (set) std::snprintf(label, sizeof(label), "%s  (%d)###cat%d", kCategories[c], set, c);
        else std::snprintf(label, sizeof(label), "%s###cat%d", kCategories[c], c);
        if (!ImGui::CollapsingHeader(label)) continue;
        for (int i = 0; i < kNumWeapons; ++i) if (kWeapons[i].cat == c) gunRow(kWeapons[i].def, false);
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ right: the finish
// Model picker for knife / gloves. Returns true if the model changed.
static bool ModelPicker(ItemRule& rule, int cat, uint16_t current) {
    bool changed = false;
    SubHeading(cat == kCatKnives ? "Knife model" : "Glove type");
    ImGui::SetNextItemWidth(-1);
    const char* preview = rule.def ? ItemName(rule.def) : (cat == kCatKnives ? "Keep the player's knife" : "Keep the player's gloves");
    if (ImGui::BeginCombo("##model", preview, ImGuiComboFlags_HeightLarge)) {
        Ui::SmoothScroll();
        if (Ui::Row("##keep", cat == kCatKnives ? "Keep the player's knife (no change)" : "Keep the player's gloves (no change)", rule.def == 0)) {
            rule = ItemRule{};
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        for (int i = 0; i < kNumWeapons; ++i) {
            const CatWeapon& w = kWeapons[i];
            if (w.cat != cat || w.def == 42 || w.def == 59) continue;
            const auto& have = cat == kCatKnives ? Skins::knivesInDemo : Skins::glovesInDemo;
            const bool inDemo = std::find(have.begin(), have.end(), w.def) != have.end();
            char id[24];
            std::snprintf(id, sizeof(id), "##m%u", w.def);
            Ui::RowOpts o;
            o.right = w.def == current ? "has now" : inDemo ? "in this demo" : nullptr;
            o.rightCol = w.def == current ? Ui::Col(Ui::kGood) : 0;
            if (Ui::Row(id, w.name, rule.def == w.def, o)) {
                if (rule.def != w.def) { rule.def = w.def; rule.skin.paint = 0; changed = true; }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Finish list + wear/seed/StatTrak/name tag. `g` may be null (nothing chosen yet); `make` creates it.
template <class Make>
static void FinishEditor(uint16_t def, GunSkin* g, Make&& make, bool allowStatTrak, bool allowNameTag, float listHeightCut) {
    SearchBox("##search", "Search finishes", g_search, sizeof(g_search));
    BeginList("##finishes", ImVec2(0, g && g->paint ? -Ui::S(listHeightCut) : 0));
    int shown = 0;
    for (int i = 0; i < kNumSkins; ++i) {
        const CatSkin& s = kSkins[i];
        if (s.def != def || !Contains(s.name, g_search)) continue;
        ++shown;
        char id[24], wear[32];
        std::snprintf(id, sizeof(id), "##p%u", s.paint);
        std::snprintf(wear, sizeof(wear), "%.2f - %.2f", s.minWear, s.maxWear);
        Ui::RowOpts o;
        o.bar = RarityU32(s.rarity);
        o.leftCol = ImGui::GetColorU32(ImVec4(RarityColor(s.rarity).x * 0.35f + 0.65f, RarityColor(s.rarity).y * 0.35f + 0.65f,
                                               RarityColor(s.rarity).z * 0.35f + 0.65f, 1));
        o.right = wear;
        if (Ui::Row(id, s.name, g && g->paint == s.paint, o)) {
            if (!g) g = make();
            if (!g->paint) g->wear = std::max(s.minWear, 0.01f);
            g->paint = s.paint;
            g->wear = std::clamp(g->wear, s.minWear, s.maxWear);
            Changed();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint kit %u", s.paint);
    }
    if (!shown) Hint("No finishes match \"%s\".", g_search);
    EndList();
    if (!g || !g->paint) return;

    ImGui::Spacing();
    const CatSkin* s = SkinBy(def, g->paint);
    float lo = s ? s->minWear : 0.f, hi = s ? s->maxWear : 1.f;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##wear", &g->wear, lo, hi, "Wear %.4f")) Changed();
    static const struct { const char* name; float v; } kWears[] = {{"FN", 0.01f}, {"MW", 0.10f}, {"FT", 0.25f}, {"WW", 0.40f}, {"BS", 0.70f}};
    {   // exterior buttons (only the ones this finish's wear range allows)
        const char* names[5];
        int map[5], n = 0, pick = -1;
        const int ext = g->wear < 0.07f ? 0 : g->wear < 0.15f ? 1 : g->wear < 0.38f ? 2 : g->wear < 0.45f ? 3 : 4;
        for (int k = 0; k < 5; ++k) {
            if (kWears[k].v < lo || kWears[k].v > hi) continue;
            if (k == ext) pick = n;
            names[n] = kWears[k].name;
            map[n++] = k;
        }
        if (n > 1 && Ui::Segmented("##wears", names, n, &pick)) { g->wear = kWears[map[pick]].v; Changed(); }
    }
    ImGui::SetNextItemWidth(Ui::S(150));
    if (ImGui::InputInt("Pattern seed", &g->seed)) { g->seed = std::clamp(g->seed, 0, 1000); Changed(); }
    if (allowStatTrak) {
        bool st = g->stattrak >= 0;
        if (Ui::Toggle("StatTrak", &st)) { g->stattrak = st ? 0 : -1; Changed(); }
        if (st) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Ui::S(130));
            if (ImGui::InputInt("kills", &g->stattrak)) { g->stattrak = std::clamp(g->stattrak, 0, 999999); Changed(); }
        }
    }
    if (allowNameTag) {
        char tag[161];
        std::snprintf(tag, sizeof(tag), "%s", g->nametag.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##tag", "Name tag (optional)", tag, sizeof(tag))) { g->nametag = tag; Changed(); }
    }
}

// Agent list: only the agents of the side being edited (T side or CT side tab).
static void AgentPicker(std::string& agent, const PlayerInfo* info) {
    Hint(g_side == Side_T ? "Agent while they play T." : "Agent while they play CT.");
    ImGui::SetNextItemWidth(agent.empty() ? -1 : -Ui::S(130));
    ImGui::InputTextWithHint("##agsearch", "Search agents", g_extraSearch, sizeof(g_extraSearch));
    if (!agent.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Remove agent", ImVec2(-1, 0))) { agent.clear(); Changed(); }
    }
    BeginList("##agents");
    if (Ui::Row("##agkeep", "Keep the player's agent (no change)", agent.empty())) { agent.clear(); Changed(); }
    for (int team = 2; team <= 3; ++team) {
        if (g_side == Side_T && team != 2) continue;
        if (g_side == Side_CT && team != 3) continue;
        SubHeading(team == 2 ? "Terrorists" : "Counter-Terrorists");
        for (int i = 0; i < kNumAgents; ++i) {
            const CatAgent& a = kAgents[i];
            if (a.team != team || !Contains(a.name, g_extraSearch)) continue;
            const bool inDemo = std::find(Skins::agentsInDemo.begin(), Skins::agentsInDemo.end(), std::string(a.model)) != Skins::agentsInDemo.end();
            const bool hasNow = info && !_stricmp(info->agent.c_str(), a.model);
            char id[24];
            std::snprintf(id, sizeof(id), "##ag%d", i);
            Ui::RowOpts o;
            o.bar = RarityU32(a.rarity);
            o.right = hasNow ? "has now" : inDemo ? "in this demo" : nullptr;
            o.rightCol = hasNow ? Ui::Col(Ui::kGood) : 0;
            if (Ui::Row(id, a.name, !_stricmp(agent.c_str(), a.model), o)) { agent = a.model; Changed(); }
        }
    }
    EndList();
}

static void StickersEditor(GunSkin& g) {
    Hint("Pick a slot, then a sticker. The next slot is picked for you.");
    for (int n = 0; n < 5; ++n) {
        const CatSticker* st = g.stickers[n] ? StickerBy(g.stickers[n]) : nullptr;
        char id[16], left[16];
        std::snprintf(id, sizeof(id), "##sl%d", n);
        std::snprintf(left, sizeof(left), "Slot %d", n + 1);
        Ui::RowOpts o;
        o.right = st ? st->name : (g.stickers[n] ? "unknown sticker" : "empty");
        o.rightCol = st ? RarityU32(st->rarity) : 0;
        const float w = ImGui::GetContentRegionAvail().x;
        const float bh = ImGui::GetTextLineHeight() + Ui::S(11);
        o.width = g.stickers[n] ? w - bh - Ui::S(4) : w;
        if (Ui::Row(id, left, g_stickerSlot == n, o)) g_stickerSlot = n;
        if (g.stickers[n]) {
            ImGui::SameLine(0, Ui::S(4));
            char x[16];
            std::snprintf(x, sizeof(x), "x##cl%d", n);
            if (ImGui::Button(x, ImVec2(bh, bh))) { g.stickers[n] = 0; Changed(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty this slot");
        }
    }
    ImGui::Spacing();
    SearchBox("##stsearch", "Search stickers: event, team, player, Holo, Foil...", g_extraSearch, sizeof(g_extraSearch));
    BeginList("##stickers");
    int matches = 0, shown = 0;
    for (int i = 0; i < kNumStickers; ++i) {
        const CatSticker& st = kStickers[i];
        if (!Contains(st.name, g_extraSearch)) continue;
        if (++matches > 300) continue;
        ++shown;
        char id[24];
        std::snprintf(id, sizeof(id), "##st%u", st.id);
        Ui::RowOpts o;
        o.bar = RarityU32(st.rarity);
        if (Ui::Row(id, st.name, g.stickers[g_stickerSlot] == (int)st.id, o)) {
            g.stickers[g_stickerSlot] = (int)st.id;
            if (g_stickerSlot < 4) ++g_stickerSlot;
            Changed();
        }
    }
    if (matches > shown) Hint("%d more. Type more of the name to narrow it down.", matches - shown);
    if (!matches) Hint("No stickers match \"%s\".", g_extraSearch);
    EndList();
}

static void CharmEditor(GunSkin& g) {
    const CatCharm* c = g.charm ? CharmBy(g.charm) : nullptr;
    {
        Ui::RowOpts o;
        o.right = c ? c->name : (g.charm ? "unknown" : "none");
        o.rightCol = c ? RarityU32(c->rarity) : 0;
        Ui::Row("##curcharm", "Charm", false, o);
    }
    if (g.charm) {
        ImGui::SetNextItemWidth(Ui::S(150));
        if (ImGui::InputInt("Pattern", &g.charmSeed)) { g.charmSeed = std::clamp(g.charmSeed, 0, 100000); Changed(); }
        ImGui::SameLine();
        if (ImGui::Button("Remove charm")) { g.charm = 0; Changed(); }
    }
    Hint("A new charm shows once the gun is made again (next round, or Refresh now).");
    SearchBox("##chsearch", "Search charms", g_extraSearch, sizeof(g_extraSearch));
    BeginList("##charms");
    for (int i = 0; i < kNumCharms; ++i) {
        const CatCharm& ch = kCharms[i];
        if (!Contains(ch.name, g_extraSearch)) continue;
        char id[24];
        std::snprintf(id, sizeof(id), "##ch%u", ch.id);
        Ui::RowOpts o;
        o.bar = RarityU32(ch.rarity);
        if (Ui::Row(id, ch.name, g.charm == (int)ch.id, o)) { g.charm = (int)ch.id; Changed(); }
    }
    EndList();
}

static void SkinColumn() {
    if (!g_player || g_slot == SLOT_NONE) {
        Ui::Heading("Finish");
        Hint("Pick a gun, the knife, the gloves or the agent.");
        return;
    }
    PlayerRules& pr = Skins::rules[g_player];
    const PlayerInfo* info = InDemo(g_player);
    Loadout& L = pr.For(g_side);
    const char* sideTag = g_side == Side_T ? "T side" : g_side == Side_CT ? "CT side" : nullptr;
    auto title = [&](const char* name) {
        Ui::Heading(name);
        if (sideTag) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, g_side == Side_T ? Ui::kTeamT : Ui::kTeamCT);
            ImGui::TextUnformatted(sideTag);
            ImGui::PopStyleColor();
        }
    };

    if (g_slot == SLOT_AGENT) {
        title("Agent");
        if (g_side == Side_Both) { Hint("Agents are set per side: pick T side or CT side."); return; }
        AgentPicker(L.agent, info);
        return;
    }

    if (g_slot == SLOT_KNIFE || g_slot == SLOT_GLOVES) {
        const bool knife = g_slot == SLOT_KNIFE;
        ItemRule& rule = knife ? L.knife : L.gloves;
        title(knife ? "Knife" : "Gloves");
        if (g_side != Side_Both && !rule.def && (knife ? pr.knife.def : pr.gloves.def))
            Hint("Uses the Both sides %s now. Choose one here to use another on this side.", knife ? "knife" : "gloves");
        uint16_t current = info ? (knife ? info->knife : info->gloves) : 0;
        if (ModelPicker(rule, knife ? kCatKnives : kCatGloves, current)) Changed();
        if (!rule.def) {
            Hint(knife ? "Choose a knife above to give this player a knife skin." : "Choose a glove type above.");
            return;
        }
        if (knife && current && current != rule.def) Hint("Their %s becomes a %s.", ItemName(current), ItemName(rule.def));
        FinishEditor(rule.def, rule.skin.paint ? &rule.skin : nullptr, [&]() { return &rule.skin; }, knife, knife, knife ? 205.f : 135.f);
        if (ImGui::Button(knife ? "Remove knife" : "Remove gloves")) { rule = ItemRule{}; Changed(); }
        return;
    }

    uint16_t def = (uint16_t)g_slot;
    auto it = L.guns.find(def);
    GunSkin* g = it != L.guns.end() ? &it->second : nullptr;
    title(ItemName(def) ? ItemName(def) : "Gun");
    if (g) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Remove skin").x - ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::SmallButton("Remove skin")) { L.guns.erase(def); Changed(); return; }
    }
    if (!g && g_side != Side_Both && pr.guns.count(def)) {
        Hint("Uses the Both sides skin now.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy it here")) { L.guns[def] = pr.guns[def]; Changed(); return; }
    }
    int& tab = g_gunTab;
    {
        char st[32], ch[32];
        int nSt = 0;
        if (g) for (int x : g->stickers) nSt += x != 0;
        std::snprintf(st, sizeof(st), nSt ? "Stickers (%d)" : "Stickers", nSt);
        std::snprintf(ch, sizeof(ch), g && g->charm ? "Charm (1)" : "Charm");
        const char* items[] = {"Finish", st, ch};
        int t = tab;
        if (Ui::Segmented("##guntabs", items, 3, &t)) { tab = t; g_extraSearch[0] = 0; }
    }
    ImGui::Spacing();
    if (tab == 0) { FinishEditor(def, g, [&]() { return &L.guns[def]; }, true, true, 205.f); return; }
    if (!g || !g->paint) { Hint("Choose a finish first. Stickers and charms go on that skin."); return; }
    if (tab == 1) StickersEditor(*g);
    else CharmEditor(*g);
}

// ------------------------------------------------------------------ Camera tab
static uint64_t g_qfXuid = 0;
static int g_qfMode = 1;
static float g_qfDist = 110, g_qfHeight = 20, g_qfSide = 18, g_qfSpeed = 25, g_qfPosT = 0.25f, g_qfAngT = 0.15f, g_qfFov = 0;
static char g_cmdLine[256] = {};
static char g_presetName[64] = {};
static bool g_scrollOut = false;

static const char* kModes[] = {"Eyes (first person)", "Chase (behind where they look)", "Follow (behind where they move)",
                               "Orbit around them", "Over the shoulder (eyes)", "Their newest grenade",
                               "Gun barrel (muzzle)", "Shoulder bone, from behind", "Face, from the front"};

static void Run(const std::string& line) { Console::Run(line); g_scrollOut = true; }

static void StartFollow() {
    char b[256];
    // "Observed" is locked to whoever is watched right now: the camera switches the demo to third person, where
    // the watched player can't be told from the first-person arms any more.
    uint64_t xuid = g_qfXuid ? g_qfXuid : Calc::ObservedXuid();
    if (xuid) std::snprintf(b, sizeof(b), "dt2_calc handle add qf_target player %llu", (unsigned long long)xuid);
    else std::snprintf(b, sizeof(b), "dt2_calc handle add qf_target observed");
    Run(b);
    switch (g_qfMode) {
        case 0: Run("dt2_calc vecang add qf_view eye qf_target"); break;
        case 1: std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view chase qf_target %.0f %.0f", g_qfDist, g_qfHeight); Run(b); break;
        case 2: std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view follow qf_target %.0f %.0f", g_qfDist, g_qfHeight); Run(b); break;
        case 3:
            Run("dt2_calc vecang add qf_center eye qf_target");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view orbit qf_center %.0f %.0f %.0f", g_qfDist, g_qfHeight, g_qfSpeed);
            Run(b);
            break;
        case 4:
            Run("dt2_calc vecang add qf_eye eye qf_target");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view offset qf_eye %.0f %.0f %.0f", -g_qfDist, g_qfSide, g_qfHeight);
            Run(b);
            break;
        case 5:
            Run("dt2_calc handle add qf_nade grenade qf_target");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view follow qf_nade %.0f %.0f", g_qfDist, g_qfHeight);
            Run(b);
            break;
        case 6:   // position from the barrel's muzzle attachment, aim from the player's eyes
            Run("dt2_calc handle add qf_gun weapon qf_target");
            Run("dt2_calc vecang add qf_muzzle attachment qf_gun muzzle_flash");
            Run("dt2_calc vecang add qf_eye eye qf_target");
            Run("dt2_calc vecang add qf_mix mix qf_muzzle qf_eye");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view offset qf_mix %.0f %.0f %.0f", -g_qfDist, g_qfSide, g_qfHeight);
            Run(b);
            break;
        case 7:   // the shoulder bone, facing where the player looks
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_bone bone qf_target %s", g_qfSide < 0 ? "shoulder_l" : "shoulder_r");
            Run(b);
            Run("dt2_calc vecang add qf_eye eye qf_target");
            Run("dt2_calc vecang add qf_mix mix qf_bone qf_eye");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_view offset qf_mix %.0f %.0f %.0f", -g_qfDist, g_qfSide, g_qfHeight);
            Run(b);
            break;
        case 8:   // in front of the face, looking back at it
            Run("dt2_calc vecang add qf_eye eye qf_target");
            std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_front offset qf_eye %.0f %.0f %.0f", g_qfDist, g_qfSide, g_qfHeight);
            Run(b);
            Run("dt2_calc vecang add qf_view lookat qf_front qf_eye");
            break;
    }
    std::snprintf(b, sizeof(b), "dt2_calc vecang add qf_cam smooth qf_view %.2f %.2f", g_qfPosT, g_qfAngT);
    Run(b);
    Run("dt2_cam source qf_cam");
    if (g_qfFov > 0) {
        std::snprintf(b, sizeof(b), "dt2_calc fov add qf_fov value %.0f", g_qfFov);
        Run(b);
        Run("dt2_cam fov qf_fov");
    } else {
        Run("dt2_cam fov game");
    }
    Run("dt2_cam enabled 1");
}

// ------------------------------------------------------------------ Attach to a part
// The camera is mounted on a part of the player or their gun and moves AND turns with it, like a camera bolted on.
// Every slider changes the mount live while the camera is on.
struct Part { const char* label; const char* kind; const char* arg; };   // kind: att (gun attachment), gun, bone, eye, feet, nade
static const Part kParts[] = {
    {"Gun - muzzle (barrel end)", "att", "muzzle_flash"},
    {"Gun - body", "gun", ""},
    {"Gun - shell eject", "att", "shell_eject"},
    {"Head", "bone", "head"},
    {"Eyes (where they look)", "eye", ""},
    {"Neck", "bone", "neck"},
    {"Chest", "bone", "spine_2"},
    {"Pelvis", "bone", "pelvis"},
    {"Right shoulder", "bone", "shoulder_r"},
    {"Left shoulder", "bone", "shoulder_l"},
    {"Right hand", "bone", "hand_r"},
    {"Left hand", "bone", "hand_l"},
    {"Right elbow", "bone", "elbow_r"},
    {"Left elbow", "bone", "elbow_l"},
    {"Right knee", "bone", "knee_r"},
    {"Left knee", "bone", "knee_l"},
    {"Right foot", "bone", "ankle_r"},
    {"Left foot", "bone", "ankle_l"},
    {"Feet (player position)", "feet", ""},
    {"Their newest grenade", "nade", ""},
    {"Gun - other attachment...", "att", ""},
};
static const char* kTurn[] = {"With the part (rigid mount)", "With their aim", "Level (part's direction, no tilt)", "Always look at their face"};
static int g_atPart = 0, g_atTurn = 0;
static uint64_t g_atXuid = 0;
static float g_atFwd = 30, g_atRight = 0, g_atUp = 0, g_atPitch = 0, g_atYaw = 180, g_atRoll = 0, g_atFov = 0, g_atSmooth = 0;
static char g_atCustom[64] = "muzzle_flash";
static bool g_atLive = false;   // the attach camera is what's on: sliders update it
static bool g_atKeepGun = true; // gun parts: stay on the gun from Attach camera, also when it's holstered

static void ApplyAttach(bool quiet) {
    char b[256];
    Calc::SetQuiet(quiet);
    uint64_t xuid = g_atXuid ? g_atXuid : Calc::ObservedXuid();
    if (xuid) std::snprintf(b, sizeof(b), "dt2_calc handle add at_target player %llu", (unsigned long long)xuid);
    else std::snprintf(b, sizeof(b), "dt2_calc handle add at_target observed");
    Console::Run(b);
    const Part& p = kParts[g_atPart];
    std::string kind = p.kind;
    if (kind == "att" || kind == "gun") {
        if (!quiet) {   // Attach camera: the gun in their hand now is the one it stays on
            std::lock_guard<std::mutex> g(Calc::lock);
            Calc::ResetKeptGuns();
        }
        Console::Run(g_atKeepGun ? "dt2_calc handle add at_gun keptgun at_target" : "dt2_calc handle add at_gun weapon at_target");
    }
    if (kind == "att") {
        std::snprintf(b, sizeof(b), "dt2_calc vecang add at_part attachment at_gun %s", *p.arg ? p.arg : (g_atCustom[0] ? g_atCustom : "muzzle_flash"));
        Console::Run(b);
    } else if (kind == "gun") Console::Run("dt2_calc vecang add at_part origin at_gun");
    else if (kind == "bone") { std::snprintf(b, sizeof(b), "dt2_calc vecang add at_part bone at_target %s", p.arg); Console::Run(b); }
    else if (kind == "eye") Console::Run("dt2_calc vecang add at_part eye at_target");
    else if (kind == "feet") Console::Run("dt2_calc vecang add at_part origin at_target");
    else if (kind == "nade") {
        Console::Run("dt2_calc handle add at_nade grenade at_target");
        Console::Run("dt2_calc vecang add at_part origin at_nade");
    }
    Console::Run("dt2_calc vecang add at_eye eye at_target");
    const char* base = "at_part";
    if (g_atTurn == 1) { Console::Run("dt2_calc vecang add at_base mix at_part at_eye"); base = "at_base"; }
    else if (g_atTurn == 2) { Console::Run("dt2_calc vecang add at_base level at_part"); base = "at_base"; }
    // mountkeys = offset that follows the points below when they are on (else these numbers)
    std::snprintf(b, sizeof(b), "dt2_calc vecang add at_mount mountkeys %s %.1f %.1f %.1f %.1f %.1f %.1f", base, g_atFwd, g_atRight, g_atUp,
                  g_atPitch, g_atYaw, g_atRoll);
    Console::Run(b);
    std::string view = "at_mount";
    if (g_atTurn == 3) { Console::Run("dt2_calc vecang add at_look lookat at_mount at_eye"); view = "at_look"; }
    if (g_atSmooth > 0) {
        std::snprintf(b, sizeof(b), "dt2_calc vecang add at_cam smooth %s %.2f %.2f", view.c_str(), g_atSmooth, g_atSmooth);
        Console::Run(b);
        view = "at_cam";
    }
    Console::Run("dt2_cam source " + view);
    std::snprintf(b, sizeof(b), "dt2_calc fov add at_fov mountkeys %.0f", g_atFov);   // no value (0, no points) = the game's
    Console::Run(b);
    Console::Run("dt2_cam fov at_fov");
    Calc::SetQuiet(false);
}

// ---- points on the mount: where the camera sits on the part at moments of the demo, glided through as it plays
static int g_atKeySel = -1;
static std::string g_atKeyMsg;
static ImVec4 g_atKeyCol;
static void AtValues(float v[7]) { v[0] = g_atFwd; v[1] = g_atRight; v[2] = g_atUp; v[3] = g_atPitch; v[4] = g_atYaw; v[5] = g_atRoll; v[6] = g_atFov; }
static void AtFromValues(const float v[7]) { g_atFwd = v[0]; g_atRight = v[1]; g_atUp = v[2]; g_atPitch = v[3]; g_atYaw = v[4]; g_atRoll = v[5]; g_atFov = v[6]; }

static void MountPointsPanel() {
    Ui::Section("Points on the mount");
    std::vector<Calc::MountKey> keys;
    bool on;
    {
        std::lock_guard<std::mutex> g(Calc::lock);
        keys = Calc::mountKeys;
        on = Calc::mountKeysOn;
    }
    Hint("The camera stays on the part, and glides between points as the demo plays. Pause, set the camera with the numbers "
         "above, Add point; go to a later moment, change the numbers, Add point again.");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (Ui::PrimaryButton("Add point here##atk", ImVec2(half, 0))) {
        float v[7];
        AtValues(v);
        std::string why;
        int i;
        {
            std::lock_guard<std::mutex> g(Calc::lock);
            i = Calc::MountKeysAdd(v, &why);
            if (i >= 0 && Calc::mountKeys.size() >= 2) Calc::mountKeysOn = true;
        }
        if (i < 0) { g_atKeyMsg = why; g_atKeyCol = Ui::kWarn; }
        else {
            g_atKeySel = i;
            g_atKeyMsg = keys.size() + 1 >= 2 ? "Point added. The camera glides through the points as the demo plays."
                                              : "First point added. Go to another moment, move the camera, add the next.";
            g_atKeyCol = Ui::kGood;
            if (!g_atLive) { ApplyAttach(false); Run("dt2_cam enabled 1"); }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The numbers above (position, rotation, FOV) at this moment of the demo.");
    ImGui::SameLine();
    if (ImGui::Button("Clear points##atk", ImVec2(-1, 0))) {
        std::lock_guard<std::mutex> g(Calc::lock);
        Calc::mountKeys.clear();
        Calc::mountKeysOn = false;
        g_atKeySel = -1;
        g_atKeyMsg.clear();
    }
    if (Ui::Toggle("Glide through the points", &on)) {
        std::lock_guard<std::mutex> g(Calc::lock);
        Calc::mountKeysOn = on;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the camera stays where the numbers above put it.");
    if (!g_atKeyMsg.empty()) ImGui::TextColored(g_atKeyCol, "%s", g_atKeyMsg.c_str());
    if (keys.empty()) return;
    ImGui::BeginChild("##atkeys", ImVec2(0, std::min(Ui::S(30) * keys.size() + Ui::S(10), Ui::S(180))), ImGuiChildFlags_Borders);
    const double t0 = keys.front().t;
    int remove = -1;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        ImGui::PushID((int)i);
        char label[96];
        std::snprintf(label, sizeof label, "%zu  tick %d  +%.2f s", i + 1, k.tick, k.t - t0);
        if (ImGui::Selectable(label, g_atKeySel == (int)i, 0, ImVec2(ImGui::GetContentRegionAvail().x - Ui::S(150), 0))) {
            g_atKeySel = (int)i;
            AtFromValues(k.v);   // its numbers in the sliders
            if (g_atLive) ApplyAttach(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("forward %.1f right %.1f up %.1f\npitch %.0f yaw %.0f roll %.0f\nFOV %s", k.v[0], k.v[1], k.v[2], k.v[3], k.v[4], k.v[5],
                              k.v[6] > 0 ? std::to_string((int)k.v[6]).c_str() : "game");
        ImGui::SameLine();
        if (ImGui::SmallButton("Go to")) {
            if (k.tick >= 0) {
                char b[64];
                std::snprintf(b, sizeof b, "demo_gototick %d", k.tick);
                Campath::QueueCommand(b);
            }
            g_atKeySel = (int)i;
            AtFromValues(k.v);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {   // this point = the numbers above
            std::lock_guard<std::mutex> g(Calc::lock);
            if (i < Calc::mountKeys.size()) AtValues(Calc::mountKeys[i].v);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Give this point the numbers above (its moment stays).");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = (int)i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (remove >= 0) {
        std::lock_guard<std::mutex> g(Calc::lock);
        if (remove < (int)Calc::mountKeys.size()) Calc::mountKeys.erase(Calc::mountKeys.begin() + remove);
        if (Calc::mountKeys.empty()) Calc::mountKeysOn = false;
        g_atKeySel = -1;
    }
}

static void AttachPanel() {
    std::string preview = "Observed (whose eyes the demo shows)";
    for (auto& p : Skins::players) if (g_atXuid && p.xuid == g_atXuid) preview = p.name;
    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##atplayer", preview.c_str())) {
        Ui::SmoothScroll();
        if (ImGui::Selectable("Observed (whose eyes the demo shows)", g_atXuid == 0)) { g_atXuid = 0; changed = true; }
        for (auto& p : Skins::players) {
            char label[200];
            std::snprintf(label, sizeof(label), "%s %s##at%llu", p.team == 2 ? "T " : p.team == 3 ? "CT" : "  ", p.name.c_str(), (unsigned long long)p.xuid);
            if (ImGui::Selectable(label, g_atXuid == p.xuid)) { g_atXuid = p.xuid; changed = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##atpart", kParts[g_atPart].label, ImGuiComboFlags_HeightLarge)) {
        Ui::SmoothScroll();
        for (int i = 0; i < IM_ARRAYSIZE(kParts); ++i)
            if (ImGui::Selectable(kParts[i].label, i == g_atPart)) { g_atPart = i; changed = true; }
        ImGui::EndCombo();
    }
    if (std::string(kParts[g_atPart].kind) == "att" && !*kParts[g_atPart].arg) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##atcustom", "attachment name (dt2_calc probe at_gun lists them)", g_atCustom, sizeof(g_atCustom))) changed = true;
    }
    {
        const std::string kind = kParts[g_atPart].kind;
        if (kind == "att" || kind == "gun") {
            if (Ui::Toggle("Stay on this gun", &g_atKeepGun)) changed = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On: the camera stays on the gun that was in their hand when you pressed Attach camera,\n"
                                  "also when they switch to another weapon and it is holstered on their back.\n"
                                  "Off: always the gun in their hand.");
        }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##atturn", &g_atTurn, kTurn, IM_ARRAYSIZE(kTurn))) changed = true;

    ImGui::PushItemWidth(-Ui::S(90));
    ImGui::TextDisabled("Position on the part (units)");
    changed |= ImGui::DragFloat("Forward", &g_atFwd, 0.5f, -500, 500, "%.1f");
    changed |= ImGui::DragFloat("Right", &g_atRight, 0.5f, -500, 500, "%.1f");
    changed |= ImGui::DragFloat("Up", &g_atUp, 0.5f, -500, 500, "%.1f");
    if (g_atTurn != 3) {
        ImGui::TextDisabled("Camera rotation on the part (degrees)");
        changed |= ImGui::SliderFloat("Pitch", &g_atPitch, -180, 180, "%.0f");
        changed |= ImGui::SliderFloat("Yaw", &g_atYaw, -180, 180, "%.0f");
        changed |= ImGui::SliderFloat("Roll", &g_atRoll, -180, 180, "%.0f");
    }
    changed |= ImGui::SliderFloat("FOV", &g_atFov, 0, 150, g_atFov > 0 ? "%.0f deg" : "game");
    changed |= ImGui::SliderFloat("Smooth", &g_atSmooth, 0, 1, g_atSmooth > 0 ? "%.2f s" : "off (rigid)");
    ImGui::PopItemWidth();
    if (ImGui::SmallButton("Facing them")) { g_atPitch = 0; g_atYaw = 180; g_atRoll = 0; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Looking forward")) { g_atPitch = 0; g_atYaw = 0; g_atRoll = 0; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset position")) { g_atFwd = g_atRight = g_atUp = 0; changed = true; }

    if (Ui::PrimaryButton("Attach camera", ImVec2(Ui::S(150), 0))) {
        ApplyAttach(false);
        Run("dt2_cam enabled 1");
        g_atLive = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop camera##at", ImVec2(-1, 0))) { Run("dt2_cam enabled 0"); g_atLive = false; }
    {
        std::lock_guard<std::mutex> g(Calc::lock);
        g_atLive = Calc::cam.enabled && Calc::cam.source.rfind("at_", 0) == 0;   // the attach camera is the one that's on
    }
    if (changed && g_atLive) {   // live while you drag (also over the points while the demo stands still)
        ApplyAttach(true);
        float v[7];
        AtValues(v);
        std::lock_guard<std::mutex> g(Calc::lock);
        Calc::MountOverride(v);
    }
    if (g_atLive) {
        // Is the chosen part there right now?
        bool found = false, known = false;
        for (auto& p : Calc::Snapshot())
            if (p.name == "at_part") { known = true; found = p.ok; }
        if (known && found) ImGui::TextColored(Ui::kGood, "Attached to: %s", kParts[g_atPart].label);
        else if (known)
            ImGui::TextColored(Ui::kBad, "'%s' not found on this player right now -\nthe camera holds its last position. Pick another part.",
                               kParts[g_atPart].label);
    }
    Hint("Drag the numbers while the camera is on to place it. Save it as a preset below.");
    MountPointsPanel();
}

static void FollowPanel() {
    std::string preview = "Observed (whose eyes the demo shows)";
    for (auto& p : Skins::players) if (g_qfXuid && p.xuid == g_qfXuid) preview = p.name;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##qfplayer", preview.c_str())) {
        Ui::SmoothScroll();
        if (ImGui::Selectable("Observed (whose eyes the demo shows)", g_qfXuid == 0)) g_qfXuid = 0;
        for (auto& p : Skins::players) {
            char label[200];
            std::snprintf(label, sizeof(label), "%s %s##%llu", p.team == 2 ? "T " : p.team == 3 ? "CT" : "  ", p.name.c_str(), (unsigned long long)p.xuid);
            if (ImGui::Selectable(label, g_qfXuid == p.xuid)) g_qfXuid = p.xuid;
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##qfmode", &g_qfMode, kModes, IM_ARRAYSIZE(kModes))) {
        // sensible starting distances for each mode: distance, height, side
        static const float kDefaults[][3] = {{0, 0, 0}, {110, 20, 0}, {150, 30, 0}, {150, 40, 0}, {60, 8, 18},
                                             {120, 25, 0}, {10, 2, 3}, {40, 6, 6}, {60, 0, 0}};
        g_qfDist = kDefaults[g_qfMode][0];
        g_qfHeight = kDefaults[g_qfMode][1];
        g_qfSide = kDefaults[g_qfMode][2];
    }
    ImGui::PushItemWidth(-Ui::S(110));
    if (g_qfMode != 0) ImGui::SliderFloat("Distance", &g_qfDist, g_qfMode >= 6 ? 0.f : 10.f, 600, "%.0f units");
    if (g_qfMode != 0) ImGui::SliderFloat("Height", &g_qfHeight, -100, 300, "%.0f units");
    if (g_qfMode == 4 || g_qfMode >= 6) ImGui::SliderFloat("Side", &g_qfSide, -100, 100, "%.0f units");
    if (g_qfMode == 6) ImGui::TextDisabled("Distance 0 = right at the barrel.");
    if (g_qfMode == 7) ImGui::TextDisabled("Side < 0 uses the left shoulder.");
    if (g_qfMode == 3) ImGui::SliderFloat("Speed", &g_qfSpeed, -180, 180, "%.0f deg/s");
    ImGui::SliderFloat("Smooth move", &g_qfPosT, 0, 2, "%.2f s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long the camera takes to catch up with the position. 0 = locked on.");
    ImGui::SliderFloat("Smooth turn", &g_qfAngT, 0, 2, "%.2f s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How long the camera takes to catch up with the angle. 0 = locked on.");
    ImGui::SliderFloat("Field of view", &g_qfFov, 0, 130, g_qfFov > 0 ? "%.0f deg" : "game");
    ImGui::PopItemWidth();
    if (Ui::PrimaryButton("Follow", ImVec2(Ui::S(120), 0))) { StartFollow(); g_atLive = false; }
    ImGui::SameLine();
    if (ImGui::Button("Stop camera", ImVec2(Ui::S(120), 0))) Run("dt2_cam enabled 0");
    ImGui::Spacing();
    ImGui::TextWrapped("The camera only changes the view while a demo plays. Record what you see with Record from here "
                       "below (HLAE or DTRecord - see Recorder in the Record tab).");
}

// Record from now until Stop: the view as it is (DemoTools2 camera, campath, free camera)
static void ManualRecordButton(const char* id) {
    const Record::Progress pr = Record::Status();
    char label[64];
    if (Record::ManualRunning()) {
        std::snprintf(label, sizeof label, "Stop recording##man_%s", id);
        if (Ui::PrimaryButton(label, ImVec2(Ui::S(160), 0))) Record::Stop();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pr.state.c_str());
        return;
    }
    int recorder;
    {
        std::lock_guard<std::mutex> g(Record::lock);
        recorder = Record::set.recorder;
    }
    std::string why;
    const bool can = !Record::RecorderFor(recorder, &why).empty() && Skins::demoPlaying && !pr.running;
    std::snprintf(label, sizeof label, "Record from here##man_%s", id);
    ImGui::BeginDisabled(!can);
    if (ImGui::Button(label, ImVec2(Ui::S(160), 0))) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char name[64];
        std::snprintf(name, sizeof name, "recording_%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
        Record::StartManual(name);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Records the view from the current moment until you press Stop recording (or dt2_record stop),\n"
                          "with the video settings of the Record tab. Console: dt2_record start [name].");
    if (!pr.running && !Record::ManualRunning() && !why.empty() && Skins::demoPlaying) Hint("Can't record: %s.", why.c_str());
    if (!pr.running && !pr.post.empty()) Hint("%s", pr.post.c_str());
}

static void CameraPanel() {
    Ui::Heading("Camera output");
    Calc::CamState c;
    {
        std::lock_guard<std::mutex> g(Calc::lock);
        c = Calc::cam;
    }
    bool enabled = c.enabled;
    if (Ui::Toggle("Camera on", &enabled)) Run(enabled ? "dt2_cam enabled 1" : "dt2_cam enabled 0");
    ImGui::SameLine(0, Ui::S(18));
    bool third = c.thirdPerson;
    if (Ui::Toggle("Third person", &third)) Run(third ? "dt2_cam thirdperson 1" : "dt2_cam thirdperson 0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("While the camera is on, the demo shows the player in third person (chase cam, spec_mode 3):\n"
                          "their own body and gun are drawn, and there is no first-person gun in front of the camera.\n"
                          "Turning the camera off goes back to first person.");
    bool hide = c.hideViewmodel;
    if (Ui::Toggle("Hide 1st-person gun", &hide)) Run(hide ? "dt2_cam hideviewmodel 1" : "dt2_cam hideviewmodel 0");
    auto views = Calc::Names(Calc::Kind::VecAng);
    ImGui::SetNextItemWidth(-Ui::S(110));
    if (ImGui::BeginCombo("Source", c.source.empty() ? "(none)" : c.source.c_str())) {
        Ui::SmoothScroll();
        for (auto& n : views) if (ImGui::Selectable(n.c_str(), n == c.source)) Run("dt2_cam source " + n);
        ImGui::EndCombo();
    }
    auto fovs = Calc::Names(Calc::Kind::Fov);
    ImGui::SetNextItemWidth(-Ui::S(110));
    if (ImGui::BeginCombo("FOV", c.fov.empty() ? "game" : c.fov.c_str())) {
        Ui::SmoothScroll();
        if (ImGui::Selectable("game", c.fov.empty())) Run("dt2_cam fov game");
        for (auto& n : fovs) if (ImGui::Selectable(n.c_str(), n == c.fov)) Run("dt2_cam fov " + n);
        ImGui::EndCombo();
    }
    float blend = c.blend;
    ImGui::SetNextItemWidth(-Ui::S(110));
    if (ImGui::SliderFloat("Blend", &blend, 0, 3, "%.2f s")) {
        char b[64];
        std::snprintf(b, sizeof(b), "dt2_cam blend %.2f", blend);
        Console::Run(b);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When you switch to another source, the camera glides there in this time.");
    ImVec4 col = c.active ? Ui::kGood : Ui::kMuted;
    ImGui::TextColored(col, "%s", c.status.empty() ? "(play a demo)" : c.status.c_str());
    if (c.active) {
        ImGui::Text("pos %.1f %.1f %.1f", c.out.pos.x, c.out.pos.y, c.out.pos.z);
        ImGui::Text("ang %.1f %.1f %.1f   fov %s", c.out.ang.x, c.out.ang.y, c.out.ang.z, c.outFov > 0 ? std::to_string((int)c.outFov).c_str() : "game");
    }

    ImGui::Spacing();
    Ui::Heading("Record");
    {
        Record::Settings rs;
        {
            std::lock_guard<std::mutex> g(Record::lock);
            rs = Record::set;
        }
        Hint("What the camera shows, from now until Stop recording - %s, %d fps%s. Settings: Record tab.",
             rs.recorder == Record::Rec_DTRecord ? "DTRecord" : "HLAE", rs.fps,
             rs.recorder == Record::Rec_DTRecord && rs.sound ? ", with sound" : "");
    }
    ManualRecordButton("camout");
    {
        const std::string out = Record::OutDir();
        Hint("Saved to %s", out.c_str());
        if (ImGui::SmallButton("Open folder##camrec")) {
            CreateDirectoryA((DllFolder() + "recordings").c_str(), nullptr);
            CreateDirectoryA(out.c_str(), nullptr);
            ShellExecuteA(nullptr, "open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    ImGui::Spacing();
    Ui::Heading("Presets");
    ImGui::SetNextItemWidth(-Ui::S(70));
    ImGui::InputTextWithHint("##presetname", "name", g_presetName, sizeof(g_presetName));
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-1, 0)) && g_presetName[0]) Run(std::string("dt2_preset save ") + g_presetName);
    for (auto& n : Calc::PresetNames()) {
        ImGui::PushID(n.c_str());
        if (ImGui::SmallButton("Load")) Run("dt2_preset load " + n);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) Run("dt2_preset delete " + n);
        ImGui::SameLine();
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopID();
    }
}

static void CalcsPanel() {
    Ui::Heading("Calcs");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
    ImGui::TextUnformatted("live values");
    ImGui::PopStyleColor();
    auto list = Calc::Snapshot();
    const float outH = Ui::S(170);
    ImGui::BeginChild("##calcs", ImVec2(0, -outH - ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_None);
    Ui::SmoothScroll();
    if (list.empty()) ImGui::TextWrapped("No calcs yet. Use 'Follow a player', or type commands below (start with dt2_calc for the list).");
    if (ImGui::BeginTable("##calctable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, Ui::S(90));
        ImGui::TableSetupColumn("Definition");
        ImGui::TableSetupColumn("Value now");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Ui::S(30));
        ImGui::TableHeadersRow();
        for (auto& p : list) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", p.def.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(p.ok ? Ui::kGood : Ui::kBad, "%s", p.ok ? p.value.c_str() : "no value");
            ImGui::TableNextColumn();
            ImGui::PushID(p.name.c_str());
            if (ImGui::SmallButton("x")) Run("dt2_calc remove " + p.name);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    static bool refocus = false;
    if (refocus) { ImGui::SetKeyboardFocusHere(); refocus = false; }
    ImGui::SetNextItemWidth(-Ui::S(130));
    bool enter = ImGui::InputTextWithHint("##cmd", "dt2_calc / dt2_cam / dt2_preset command", g_cmdLine, sizeof(g_cmdLine),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run", ImVec2(Ui::S(60), 0)) || enter) && g_cmdLine[0]) {
        std::string line = g_cmdLine;
        if (line.rfind("dt2_", 0) != 0) line = "dt2_calc " + line;
        Run(line);
        g_cmdLine[0] = 0;
        refocus = enter;   // Enter: keep typing the next command (SetKeyboardFocusHere(-1) here focused the Run button)
    }
    ImGui::SameLine();
    if (ImGui::Button("Help", ImVec2(-1, 0))) Run("dt2_calc");
    ImGui::BeginChild("##out", ImVec2(0, outH), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    for (auto& l : Console::RecentOutput()) ImGui::TextUnformatted(l.c_str());
    if (g_scrollOut) { ImGui::SetScrollHereY(1.0f); g_scrollOut = false; }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ Viewmodel (Camera page tab)
static void ViewmodelPanel() {
    Ui::Heading("Position and FOV");
    if (!Viewmodel::Available()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(Ui::kWarn, "%s", Viewmodel::Status().c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    Viewmodel::Settings v = Viewmodel::Get();
    bool changed = false;
    changed |= Ui::Toggle("Custom viewmodel", &v.custom);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: every player's own viewmodel, as the demo recorded it.\n"
                          "On: your position and FOV for everyone's first-person gun.");
    if (!v.custom) ImGui::BeginDisabled();
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (Viewmodel::kNumPresets - 1)) / Viewmodel::kNumPresets;
    for (int i = 0; i < Viewmodel::kNumPresets; ++i) {
        const auto& p = Viewmodel::kPresets[i];
        if (i) ImGui::SameLine();
        if (ImGui::Button(p.name, ImVec2(bw, 0))) { v.x = p.x; v.y = p.y; v.z = p.z; v.fov = p.fov; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f / %.1f / %.1f, FOV %.0f", p.x, p.y, p.z, p.fov);
    }
    float lo[4], hi[4];
    Viewmodel::Limits(v.extended, lo, hi);
    const float lw = Ui::S(110);
    ImGui::SetNextItemWidth(-lw);
    changed |= ImGui::SliderFloat("Left / right", &v.x, lo[0], hi[0], "%.1f");
    ImGui::SetNextItemWidth(-lw);
    changed |= ImGui::SliderFloat("Back / forward", &v.y, lo[1], hi[1], "%.1f");
    ImGui::SetNextItemWidth(-lw);
    changed |= ImGui::SliderFloat("Down / up", &v.z, lo[2], hi[2], "%.1f");
    ImGui::SetNextItemWidth(-lw);
    changed |= ImGui::SliderFloat("FOV", &v.fov, lo[3], hi[3], "%.0f");
    changed |= Ui::Toggle("Past CS2's limits", &v.extended);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CS2 keeps the viewmodel within -2..2.5 / -2..2 / -2..2 and FOV 60..68.\n"
                          "On: -10..10 and FOV 40..120.");
    if (!v.custom) ImGui::EndDisabled();
    if (changed) Viewmodel::Set(v);
    Hint("%s. With the POV HUD on, the gun keeps the player's own position. Console: dt2_viewmodel", Viewmodel::Status().c_str());
}

static void CameraTab() {
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##camleft", ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.38f, Ui::S(340), Ui::S(440)), h), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    bool vmTab = false;
    if (ImGui::BeginTabBar("##camtabs")) {
        if (ImGui::BeginTabItem("Attach to a part")) { AttachPanel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Follow a player")) { FollowPanel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Viewmodel")) { vmTab = true; ViewmodelPanel(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (!vmTab) {   // the camera output belongs to the two camera tabs
        ImGui::Spacing();
        CameraPanel();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##camright", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    CalcsPanel();
    ImGui::EndChild();
}

static void Section(const char* text) { Ui::Section(text); }

// ------------------------------------------------------------------ Cinematics tab
// Campaths: keyframes the camera glides through, timed to the demo; quick orbit / dolly moves; HLAE campath files;
// and HLAE's mirv_pov (POV HUD) switches.
static char g_cpName[64] = "my_campath";
static std::string g_cpMsg;
static ImVec4 g_cpMsgCol;
static float g_cpDuration = 0;
static float g_orbRadius = 250, g_orbHeight = 60, g_orbDegrees = 180, g_orbSeconds = 6;
static int g_orbKeys = 9;
static float g_dollyDist = 300, g_dollySeconds = 4;
static bool g_cpDragging = false;
static void CpNote(const std::string& m, const ImVec4& c) { g_cpMsg = m; g_cpMsgCol = c; }

static void CpLabel(const char* t) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(t);
    ImGui::SameLine(Ui::S(120));
    ImGui::SetNextItemWidth(-1);
}

static void GoToTick(int tick) {
    if (tick < 0) { CpNote("No demo tick known for that key (add keys while the demo plays).", Ui::kWarn); return; }
    char b[64];
    std::snprintf(b, sizeof b, "demo_gototick %d", std::max(0, tick));
    Campath::QueueCommand(b);
}


// Sequence: points made with the demo paused; Generate times them from the start tick.
static int g_cpMode = 0;   // 0 sequence (paused), 1 path keys
static void SequenceLeft() {
    int n, total, start, holdTicks;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        n = (int)Campath::seq.points.size();
        total = Campath::SeqTotalTicks();
        start = Campath::seq.startTick;
        holdTicks = Campath::seq.holdTicks;
    }
    (void)holdTicks;
    Hint("Pause the demo where the shot starts. Fly the camera to each spot and press Add point - the demo stays paused. "
         "Then Generate: DemoTools2 works out how many ticks the camera needs from each point to the next and makes the real path, "
         "starting at the paused tick.");
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    if (start >= 0) ImGui::Text("%d points - %d ticks (%.2f s) - starts at tick %d", n, total, total / 64.0, start);
    else ImGui::Text("%d points", n);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;   // two even columns
    if (Ui::PrimaryButton("Add point here", ImVec2(half, 0))) {
        std::string why;
        if (Campath::SeqAdd(&why)) CpNote("Point added. Move the camera to the next spot and add the next one.", Ui::kGood);
        else CpNote(why, Ui::kWarn);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The view you see now (HLAE's mirv_input camera or the demo's free camera).\n"
                          "It goes after the selected point. The first point also sets the start tick.\nConsole / bind: dt2_campath seq add");
    ImGui::SameLine();
    if (Ui::PrimaryButton("Generate", ImVec2(-1, 0))) {
        std::string why;
        if (Campath::SeqGenerate(&why)) {
            g_cpDuration = 0;
            CpNote("Path generated and switched on. Unpause (or Go to start) to watch it; record with mirv_streams.", Ui::kGood);
        } else CpNote(why, Ui::kWarn);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Makes the real path from the points, timed from the start tick. Console: dt2_campath seq generate");
    if (ImGui::Button("Generate + preview", ImVec2(half, 0))) {
        std::string why;
        if (Campath::SeqGenerate(&why)) { Campath::Preview(); g_cpDuration = 0; CpNote("Generated - previewing in real time.", Ui::kGood); }
        else CpNote(why, Ui::kWarn);
    }
    ImGui::SameLine();
    if (ImGui::Button("Start at this tick", ImVec2(-1, 0))) {
        std::string why;
        if (Campath::SeqUseCurrentStart(&why)) CpNote("The path will start at the tick the demo is on now.", Ui::kGood);
        else CpNote(why, Ui::kWarn);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Normally the start is the tick of the first point. Use this to start somewhere else.");
    if (ImGui::Button("Go to start##seq", ImVec2(half, 0))) GoToTick(start);
    ImGui::SameLine();
    if (ImGui::Button("Edit path##seq", ImVec2(-1, 0))) { Campath::SeqFromPath(); CpNote("The current path is back in the sequence (its timing kept as ticks).", Ui::kGood); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns the current (generated or loaded) path back into points to edit.");
    if (ImGui::Button("Clear points", ImVec2(half, 0))) { Campath::SeqClear(); CpNote("Points cleared.", Ui::kMuted); }

    Section("Auto timing");
    Hint("Used for every point whose Ticks is 0 (auto). The slower of the move and the turn decides.");
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        ImGui::PushItemWidth(-Ui::S(130));
        ImGui::SliderFloat("Camera speed", &Campath::seq.speed, 20, 2000, "%.0f units/s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A running player moves 250 units a second.");
        ImGui::SliderFloat("Turn speed", &Campath::seq.turn, 10, 720, "%.0f deg/s");
        ImGui::SliderInt("Shortest move", &Campath::seq.minTicks, 1, 640, "%d ticks");
        ImGui::SliderInt("Hold before a cut", &Campath::seq.holdTicks, 1, 1280, "%d ticks");
        ImGui::PopItemWidth();
    }
}

static void PointEditor() {
    Campath::Point p;
    int sel, count, autoTicks = 0;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        count = (int)Campath::seq.points.size();
        sel = Campath::seq.selected;
        if (sel >= count) sel = Campath::seq.selected = count - 1;
        if (sel >= 0) {
            p = Campath::seq.points[sel];
            Campath::Point a = p;
            a.ticks = 0;
            std::swap(Campath::seq.points[sel], a);
            autoTicks = Campath::SeqTicks(sel);   // what auto would give
            std::swap(Campath::seq.points[sel], a);
        }
    }
    if (sel < 0) { Hint("Select a point (in the list or on the map) to edit it."); return; }
    ImGui::PushFont(Ui::bold, 0.f);
    ImGui::Text("Point %d of %d", sel + 1, count);
    ImGui::PopFont();
    bool edit = false;
    const bool last = sel + 1 >= count;
    ImGui::PushItemWidth(-Ui::S(120));
    if (!last) {
        int t = p.ticks;
        char fmt[48];
        std::snprintf(fmt, sizeof fmt, t > 0 ? "%%d ticks" : "auto (%d ticks)", autoTicks);
        ImGui::SetNextItemWidth(-Ui::S(170));   // room for the label and the "auto" button
        if (ImGui::DragInt(p.cut ? "Hold for" : "Ticks to next", &t, 1.f, 0, 64 * 120, fmt)) { p.ticks = std::max(0, t); edit = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many demo ticks (64 a second) the camera takes to reach the next point.\n0 = auto (Auto timing).");
        ImGui::SameLine();
        if (ImGui::SmallButton("auto")) { p.ticks = 0; edit = true; }
        edit |= Ui::Toggle("Cut to the next point (new shot)", &p.cut);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The camera holds this view for the ticks above, then jumps to the next point.");
    } else ImGui::TextDisabled("Last point: the path ends here.");
    edit |= ImGui::DragFloat3("Position", p.pos, 1.f, -16384.f, 16384.f, "%.1f");
    edit |= ImGui::DragFloat3("Pitch/Yaw/Roll", p.ang, 0.2f, -360.f, 360.f, "%.1f");
    edit |= ImGui::SliderFloat("Field of view", &p.fov, 10.f, 140.f, "%.1f deg");
    ImGui::PopItemWidth();
    if (edit) {
        p.ang[0] = std::clamp(p.ang[0], -89.f, 89.f);
        std::lock_guard<std::mutex> g(Campath::lock);
        if (sel < (int)Campath::seq.points.size()) Campath::seq.points[sel] = p;
    }
    if (ImGui::Button("Set to current view##pt", ImVec2(Ui::S(160), 0))) {
        if (Campath::SeqSetToView(sel)) CpNote("Point moved to the current view.", Ui::kGood);
        else CpNote("The camera isn't known yet - play the demo for a moment.", Ui::kWarn);
    }
    ImGui::SameLine();
    if (ImGui::Button("Up", ImVec2(Ui::S(46), 0))) Campath::SeqMove(sel, -1);
    ImGui::SameLine();
    if (ImGui::Button("Down", ImVec2(Ui::S(56), 0))) Campath::SeqMove(sel, +1);
    ImGui::SameLine();
    if (ImGui::Button("Delete##pt", ImVec2(-1, 0))) Campath::SeqRemove(sel);
}

static void PointList() {
    std::vector<std::pair<int, bool>> rows;   // ticks to next, cut
    std::vector<bool> autoT;
    int sel, start;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        for (int i = 0; i < (int)Campath::seq.points.size(); ++i) {
            rows.push_back({Campath::SeqTicks(i), Campath::seq.points[i].cut});
            autoT.push_back(Campath::seq.points[i].ticks <= 0);
        }
        sel = Campath::seq.selected;
        start = Campath::seq.startTick;
    }
    int at = start;
    for (size_t i = 0; i < rows.size(); ++i) {
        char left[96], right[64], id[24];
        if (start >= 0) std::snprintf(left, sizeof left, "%2u   tick %d", (unsigned)i + 1, at);
        else std::snprintf(left, sizeof left, "%2u", (unsigned)i + 1);
        if (i + 1 < rows.size())
            std::snprintf(right, sizeof right, "%s %d ticks%s", rows[i].second ? "hold" : "->", rows[i].first, autoT[i] ? " (auto)" : "");
        else std::snprintf(right, sizeof right, "end");
        std::snprintf(id, sizeof id, "##cpp%u", (unsigned)i);
        Ui::RowOpts o;
        o.right = right;
        o.rightCol = Ui::Col(Ui::kMuted);
        if (rows[i].second && i + 1 < rows.size()) { o.chip = "CUT"; o.chipCol = Ui::Col(Ui::kBad); }
        if (Ui::Row(id, left, (int)i == sel, o)) {
            std::lock_guard<std::mutex> g(Campath::lock);
            Campath::seq.selected = (int)i;
        }
        at += rows[i].first;
    }
}

static void HotkeysSection() {
    Section("Hotkeys");
    Hint("They work while CS2 is in front (the menu can stay closed). Click a key to change it: press the new key "
         "(with Ctrl / Shift / Alt if you like), Esc to keep it. Unbind removes it.");
    if (ImGui::SmallButton("Unbind all")) for (int i = 0; i < Hotkeys::Count; ++i) Hotkeys::Set((Hotkeys::Action)i, Hotkeys::Binding{});
    ImGui::SameLine();
    if (ImGui::SmallButton("Default keys")) Hotkeys::ResetDefaults();
    Hotkeys::Action capturing = Hotkeys::Count;
    const bool cap = Hotkeys::Capturing(capturing);
    if (cap) Hotkeys::Capture();
    for (int i = 0; i < Hotkeys::Count; ++i) {
        const Hotkeys::Action a = (Hotkeys::Action)i;
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Hotkeys::Name(a));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Hotkeys::Help(a));
        ImGui::SameLine(Ui::S(170));
        const bool mine = cap && capturing == a;
        const Hotkeys::Binding b = Hotkeys::Get(a);
        const std::string label = mine ? std::string("Press a key...") : Hotkeys::KeyText(b);
        if (mine) ImGui::PushStyleColor(ImGuiCol_Button, Ui::Col(Ui::kAccent, 0.35f));
        if (ImGui::Button(label.c_str(), ImVec2(-Ui::S(78), 0)) && !cap) Hotkeys::BeginCapture(a);
        if (mine) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(!b.vk || cap);
        if (ImGui::Button("Unbind", ImVec2(-1, 0))) Hotkeys::Set(a, Hotkeys::Binding{});
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && b.vk) ImGui::SetTooltip("Remove %s from %s", label.c_str(), Hotkeys::Name(a));
        ImGui::PopID();
    }
}

static void VideoSettings(bool killClips);

// Record the path (same HLAE recording and video settings as the Record tab)
static void CinematicRecordSection() {
    Section("Record the path");
    const Record::Progress pr = Record::Status();
    double t0 = 0, t1 = 0;
    size_t n;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        n = Campath::keys.size();
        if (n >= 2) { t0 = Campath::keys.front().t; t1 = Campath::keys.back().t; }
    }
    const int tick0 = n >= 2 ? Campath::TickFor(t0) : -1, tick1 = n >= 2 ? Campath::TickFor(t1) : -1;
    if (pr.running) {
        Ui::StatusPill("Recording", Ui::kBad);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pr.state.c_str());
        if (Ui::PrimaryButton("Stop recording", ImVec2(Ui::S(150), 0))) Record::Stop();
        Hint("Leave CS2 alone while it records.");
    } else {
        int recorder;
        {
            std::lock_guard<std::mutex> g(Record::lock);
            recorder = Record::set.recorder;
        }
        const bool playing = Skins::demoPlaying, canRec = !Record::RecorderFor(recorder, nullptr).empty();
        const bool ready = n >= 2 && tick0 >= 0 && tick1 > tick0 && playing && canRec;
        ImGui::BeginDisabled(!ready);
        if (Ui::PrimaryButton("Record path", ImVec2(Ui::S(150), 0))) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char name[128];
            std::snprintf(name, sizeof name, "cinematic_%s_%02d%02d%02d", g_cpName[0] ? g_cpName : "path", st.wHour, st.wMinute, st.wSecond);
            {
                std::lock_guard<std::mutex> g(Campath::lock);
                Campath::set.enabled = true;   // the path has to drive the camera while it records
            }
            Campath::StopPreview();
            Record::StartCinematic(tick0, tick1, name);
            CpNote("Recording the path - it goes to the start, records to the end, then the MP4 is finished in the background.", Ui::kGood);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (n >= 2) ImGui::TextDisabled("%.1f s, ticks %d - %d", t1 - t0, tick0, tick1);
        else ImGui::TextDisabled("as the demo plays");
        // the same path, flown while the demo stays paused at this moment (frozen time)
        ImGui::BeginDisabled(!(n >= 2 && playing && canRec));
        if (ImGui::Button("Freeze path record", ImVec2(Ui::S(150), 0))) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char name[128];
            std::snprintf(name, sizeof name, "freeze_%s_%02d%02d%02d", g_cpName[0] ? g_cpName : "path", st.wHour, st.wMinute, st.wSecond);
            Campath::StopPreview();
            Record::StartFreeze(name);
            CpNote("Freeze path record - the demo stays paused here and the camera flies the whole path.", Ui::kGood);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pauses the demo at the current moment and records the whole path (%.1f s) flying through\n"
                              "the frozen scene. Same recorder and video settings; no sound (the demo is paused).", t1 - t0);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("demo paused, time frozen");
        if (n < 2) Hint("Make a path first (a sequence and Generate, or path keys).");
        else if (tick0 < 0) Hint("The path's demo ticks aren't known - add or generate it while the demo plays.");
        else if (!playing) Hint("Play the demo (pausing is fine) to record.");
        else if (!canRec) {
            std::string why;
            Record::RecorderFor(recorder, &why);
            Hint("Can't record: %s.", why.c_str());
        }
        if (!pr.state.empty()) Hint("Last run: %s", pr.state.c_str());
    }
    if (!pr.post.empty()) Hint("%s", pr.post.c_str());
    if (!pr.done.empty()) ImGui::TextDisabled("saved: %s", pr.done.back().c_str());
    if (ImGui::TreeNode("Video settings (shared with the Record tab)")) {
        VideoSettings(false);
        ImGui::TreePop();
    }
    if (ImGui::Button("Open folder##cprec", ImVec2(Ui::S(110), 0))) {
        const std::string out = Record::OutDir();
        CreateDirectoryA((DllFolder() + "recordings").c_str(), nullptr);
        CreateDirectoryA(out.c_str(), nullptr);
        ShellExecuteA(nullptr, "open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Record::FfmpegPath().empty() ? "ffmpeg not found (Record tab)" : "ffmpeg ok");
}

static void CinematicsLeft() {
    const Campath::Live live = Campath::Status();
    size_t n;
    double dur;
    bool enabled;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        n = Campath::keys.size();
        dur = n >= 2 ? Campath::keys.back().t - Campath::keys.front().t : 0;
        enabled = Campath::set.enabled;
    }
    {
        static const char* kModes[] = {"Sequence (demo paused)", "Path keys"};
        ImGui::SetNextItemWidth(-1);
        Ui::Segmented("##cp_mode", kModes, 2, &g_cpMode);
        ImGui::Spacing();
    }
    // status
    {
        const char* st = live.previewing ? "Previewing" : live.playing ? "Playing" : enabled ? "On - waiting" : "Off";
        const ImVec4 col = live.playing ? Ui::kGood : enabled ? Ui::kAccent : Ui::kMuted;
        Ui::StatusPill(st, col);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%u keys, %.2f s", (unsigned)n, dur);
        if (!live.note.empty()) Hint("%s", live.note.c_str());
    }
    ImGui::Spacing();
    if (g_cpMode == 0) {
        SequenceLeft();
        if (ImGui::Button(live.previewing ? "Stop preview" : "Preview path", ImVec2(Ui::S(150), 0))) {
            if (live.previewing) Campath::StopPreview(); else Campath::Preview();
        }
        ImGui::SameLine();
        if (ImGui::Button("Undo generate", ImVec2(-1, 0))) { if (!Campath::Undo()) CpNote("Nothing to undo.", Ui::kMuted); }
    }
    if (g_cpMode == 1 && Ui::PrimaryButton("Add key here", ImVec2(Ui::S(150), 0))) {
        std::string why;
        if (Campath::AddKeyHere(&why)) CpNote("Key added at the current view and demo time.", Ui::kGood);
        else CpNote(why, Ui::kWarn);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Takes the view you see now (HLAE's mirv_input camera, the demo's free camera or any other) at this\n"
                          "moment of the demo. Move the demo on, move the camera, add the next key.\n"
                          "Console / bind: dt2_campath add");
    if (g_cpMode == 1) {
        ImGui::SameLine();
        if (live.previewing) { if (ImGui::Button("Stop", ImVec2(Ui::S(90), 0))) Campath::StopPreview(); }
        else if (ImGui::Button("Preview", ImVec2(Ui::S(90), 0))) Campath::Preview();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Flies the path once in real time, even with the demo paused.");
        ImGui::SameLine();
        if (ImGui::Button("Undo", ImVec2(Ui::S(70), 0))) { if (!Campath::Undo()) CpNote("Nothing to undo.", Ui::kMuted); }
    }
    if (!g_cpMsg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_cpMsgCol);
        ImGui::TextWrapped("%s", g_cpMsg.c_str());
        ImGui::PopStyleColor();
    }

    Section("Playback");
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        Ui::Toggle("Play the path in the demo", &Campath::set.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While the demo is inside the path's time, the path is the camera.\nRecord with HLAE's mirv_streams as usual.");
        Ui::Toggle("Hold the first / last view outside the path", &Campath::set.hold);
        Ui::Toggle("Ease in and out (gentle start and stop)", &Campath::set.easeEnds);
        Ui::Toggle("Hide the HUD while it plays", &Campath::set.hideHud);
        Ui::Toggle("Hide the first-person gun while it plays", &Campath::set.hideViewmodel);
    }

    CinematicRecordSection();

    HotkeysSection();

    Section("Smoothness");
    {
        static const char* kI[] = {"Smooth", "Linear"};
        std::lock_guard<std::mutex> g(Campath::lock);
        CpLabel("Position");
        Ui::Segmented("##cp_pi", kI, 2, &Campath::set.posInterp);
        CpLabel("Angles");
        Ui::Segmented("##cp_ai", kI, 2, &Campath::set.angInterp);
        CpLabel("Field of view");
        Ui::Segmented("##cp_fi", kI, 2, &Campath::set.fovInterp);
    }

    if (g_cpMode == 1) {
    Section("Timing");
    {
        if (g_cpDuration <= 0 && dur > 0) g_cpDuration = (float)dur;
        CpLabel("Length");
        ImGui::SetNextItemWidth(-Ui::S(70));
        ImGui::DragFloat("##cp_dur", &g_cpDuration, 0.05f, 0.1f, 600.f, "%.2f s");
        ImGui::SameLine();
        if (ImGui::Button("Set", ImVec2(-1, 0))) { Campath::SetDuration(g_cpDuration); CpNote("Path stretched to the new length.", Ui::kGood); }
        if (ImGui::Button("Even speed", ImVec2(Ui::S(110), 0))) { Campath::EvenSpeed(); CpNote("Keys re-timed: the camera now moves at an even speed.", Ui::kGood); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the first and last key's time and moves the others so the camera\ntravels at the same speed the whole way (no rushing or crawling between keys).");
        ImGui::SameLine();
        if (ImGui::Button("Start here", ImVec2(Ui::S(100), 0))) { Campath::StartNow(); CpNote("The whole path now starts at the current demo moment.", Ui::kGood); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the whole path (all keys) so it starts at the demo moment you are at now.");
        ImGui::SameLine();
        if (ImGui::Button("Go to start", ImVec2(-1, 0))) {
            double t0 = 0;
            { std::lock_guard<std::mutex> g(Campath::lock); if (!Campath::keys.empty()) t0 = Campath::keys.front().t; }
            const int tick = Campath::TickFor(t0);
            GoToTick(tick < 0 ? -1 : tick - 64);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jumps the demo to one second before the path starts.");
    }

    Section("Aim");
    {
        static const char* kAim[] = {"Keyframes", "Look at a player"};
        std::lock_guard<std::mutex> g(Campath::lock);
        Ui::Segmented("##cp_aim", kAim, 2, &Campath::set.aim);
        if (Campath::set.aim == 1) {
            std::string preview = "Choose a player";
            for (auto& p : Skins::players) if (p.xuid == Campath::set.aimXuid) preview = p.name;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##cp_aimp", preview.c_str())) {
                Ui::SmoothScroll();
                for (auto& p : Skins::players) {
                    char label[200];
                    std::snprintf(label, sizeof(label), "%s %s##%llu", p.team == 2 ? "T " : p.team == 3 ? "CT" : "  ", p.name.c_str(), (unsigned long long)p.xuid);
                    if (ImGui::Selectable(label, Campath::set.aimXuid == p.xuid)) Campath::set.aimXuid = p.xuid;
                }
                ImGui::EndCombo();
            }
            static const char* kPart[] = {"Eyes", "Chest", "Feet"};
            Ui::Segmented("##cp_part", kPart, 3, &Campath::set.aimPart);
            Hint("The path moves the camera; it always turns to face this player (roll and field of view still come from the keys).");
        }
    }

    Section("Quick moves");
    Hint("Each one replaces the path with a ready-made move that starts now, from the current view. Undo brings the old path back.");
    ImGui::PushItemWidth(-Ui::S(110));
    ImGui::SliderFloat("Radius", &g_orbRadius, 30, 1500, "%.0f units");
    ImGui::SliderFloat("Height", &g_orbHeight, -300, 600, "%.0f units");
    ImGui::SliderFloat("Turn", &g_orbDegrees, -720, 720, "%.0f deg");
    ImGui::SliderFloat("Time##orb", &g_orbSeconds, 0.5f, 60, "%.1f s");
    ImGui::SliderInt("Keys", &g_orbKeys, 3, 49);
    ImGui::PopItemWidth();
    if (ImGui::Button("Orbit what I look at", ImVec2(-1, 0))) {
        float pos[3], ang[3];
        if (!Game::ViewPosition(pos, ang)) CpNote("The camera isn't known yet - play the demo for a moment.", Ui::kWarn);
        else {
            const float p = ang[0] * 3.14159265f / 180.f, y = ang[1] * 3.14159265f / 180.f;
            const float c[3] = {pos[0] + std::cos(p) * std::cos(y) * g_orbRadius, pos[1] + std::cos(p) * std::sin(y) * g_orbRadius,
                                pos[2] - std::sin(p) * g_orbRadius};
            Campath::Orbit(c, g_orbRadius, g_orbHeight, g_orbDegrees, g_orbSeconds, g_orbKeys);
            CpNote("Orbit made around the spot the camera looked at (Radius units ahead). Turn on \"Play the path\" or Preview.", Ui::kGood);
        }
    }
    ImGui::PushItemWidth(-Ui::S(110));
    ImGui::SliderFloat("Distance", &g_dollyDist, -2000, 2000, "%.0f units");
    ImGui::SliderFloat("Time##dolly", &g_dollySeconds, 0.5f, 60, "%.1f s");
    ImGui::PopItemWidth();
    if (ImGui::Button("Dolly straight ahead", ImVec2(-1, 0))) {
        float pos[3];
        if (!Game::ViewPosition(pos)) CpNote("The camera isn't known yet - play the demo for a moment.", Ui::kWarn);
        else { Campath::Dolly(g_dollyDist, g_dollySeconds); CpNote("Dolly made (negative distance moves backwards).", Ui::kGood); }
    }
    }   // path mode

    Section("Save / load");
    ImGui::SetNextItemWidth(-Ui::S(80));
    ImGui::InputText("##cp_name", g_cpName, sizeof g_cpName);
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-1, 0))) {
        std::string why;
        if (Campath::Save(g_cpName, &why)) CpNote(std::string("Saved as ") + g_cpName + ".xml", Ui::kGood);
        else CpNote(why, Ui::kWarn);
    }
    {
        const auto names = Campath::SavedNames();
        if (names.empty()) Hint("No saved campaths yet. Files go to %s (HLAE campath XML, so mirv_campath load works on them too, and HLAE's files load here).",
                                Campath::Folder().c_str());
        for (const std::string& nm : names) {
            ImGui::PushID(nm.c_str());
            if (ImGui::Button("Load", ImVec2(Ui::S(60), 0))) {
                std::string why;
                if (Campath::Load(nm, &why)) { CpNote("Loaded " + nm + ".", Ui::kGood); std::snprintf(g_cpName, sizeof g_cpName, "%s", nm.c_str()); g_cpDuration = 0; }
                else CpNote(why, Ui::kWarn);
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(nm.c_str());
            ImGui::PopID();
        }
        if (ImGui::Button("Open folder", ImVec2(Ui::S(110), 0))) {
            CreateDirectoryA(Campath::Folder().c_str(), nullptr);
            ShellExecuteA(nullptr, "open", Campath::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    Section("POV HUD (DemoTools2, test)");
    {
        bool on = Pov::Mode() != 0;
        if (Ui::Toggle("POV HUD", &on)) Pov::SetMode(on ? 1 : 0, true);
        Hint("%s", Pov::Status().c_str());
        if (ImGui::Button("Reload view", ImVec2(Ui::S(110), 0))) Pov::ReloadView();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Jumps the demo to where it is, so CS2 builds the gun, hands and screen effects again.\n"
                              "Also done by itself when you switch the POV HUD off.");
        Hint("Shows the watched player's own HUD on a GOTV demo, as in their POV demo - DemoTools2's own version, no special "
             "HLAE needed. Watch a player in first person (not free / chase camera). Test stage: tell us what works. "
             "Console: dt2_pov 1|0");
    }

    Section("POV HUD (HLAE mirv_pov)");
    if (!Campath::PovAvailable()) {
        Hint("Only for HLAE builds that have mirv_pov; the loaded HLAE doesn't. DemoTools2's own POV HUD (above) doesn't need it.");
    } else {
        bool changed = false;
        {
            std::lock_guard<std::mutex> g(Campath::lock);
            changed |= Ui::Toggle("POV HUD (mirv_pov)", &Campath::pov.on);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("HLAE's POV HUD on a GOTV demo: the watched player's own HUD, radar, kill feed and prompts,\n"
                                  "as if it were their POV demo. Works with DemoTools2's skins and camera.");
            ImGui::BeginDisabled(!Campath::pov.on);
            changed |= Ui::Toggle("Scoreboard sync (mirv_pov_scoreboard)", &Campath::pov.scoreboard);
            changed |= Ui::Toggle("Buy menu (mirv_pov_buymenu)", &Campath::pov.buymenu);
            ImGui::EndDisabled();
        }
        if (changed) { Campath::ApplyPov(); CpNote("Sent to HLAE - its console shows the result.", Ui::kGood); }
        Hint("While a campath plays with \"Hide the HUD\" on, the POV HUD is hidden too and comes back after.");
    }
}

// Top-down view of the path: line, keys (click to select), and where the camera is now.
static void PathMap(float height, bool sequence) {
    std::vector<float> pts;
    if (sequence) Campath::SampleSequence(240, pts);
    else Campath::Sample(240, pts);
    std::vector<Campath::Key> ks;
    int sel;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        if (sequence) {
            for (const Campath::Point& p : Campath::seq.points) {
                Campath::Key k;
                std::memcpy(k.pos, p.pos, sizeof k.pos);
                std::memcpy(k.ang, p.ang, sizeof k.ang);
                k.cut = p.cut;
                ks.push_back(k);
            }
            sel = Campath::seq.selected;
        } else {
            ks = Campath::keys;
            sel = Campath::selected;
        }
    }
    const Campath::Live live = Campath::Status();
    ImGui::BeginChild("##cp_map", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos(), sz = ImGui::GetContentRegionAvail();
    if (ks.empty()) {
        Hint(sequence ? "Top-down view of the sequence. Add points and the move shows here." : "Top-down view of the path. Add keys and the path shows here.");
        ImGui::EndChild();
        return;
    }
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    auto grow = [&](float x, float y) { lo[0] = std::min(lo[0], x); lo[1] = std::min(lo[1], y); hi[0] = std::max(hi[0], x); hi[1] = std::max(hi[1], y); };
    for (size_t i = 0; i + 2 < pts.size(); i += 3) grow(pts[i], pts[i + 1]);
    for (auto& k : ks) grow(k.pos[0], k.pos[1]);
    if (live.camKnown) grow(live.camPos[0], live.camPos[1]);
    const float span = std::max({hi[0] - lo[0], hi[1] - lo[1], 200.f}) * 1.15f;
    const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;
    const float scale = std::min(sz.x, sz.y) / span;
    auto toScreen = [&](float x, float y) { return ImVec2(o.x + sz.x * 0.5f + (x - cx) * scale, o.y + sz.y * 0.5f - (y - cy) * scale); };
    // a light grid every 256 units (CS2 maps are built on it)
    const ImU32 grid = Ui::Col(Ui::kMuted, 0.10f);
    for (float gx = std::floor((cx - span) / 256.f) * 256.f; gx < cx + span; gx += 256.f) {
        const ImVec2 a = toScreen(gx, cy - span), b = toScreen(gx, cy + span);
        dl->AddLine(ImVec2(a.x, o.y), ImVec2(b.x, o.y + sz.y), grid);
    }
    for (float gy = std::floor((cy - span) / 256.f) * 256.f; gy < cy + span; gy += 256.f) {
        const ImVec2 a = toScreen(cx - span, gy);
        dl->AddLine(ImVec2(o.x, a.y), ImVec2(o.x + sz.x, a.y), grid);
    }
    for (size_t i = 3; i + 2 < pts.size(); i += 3)
        dl->AddLine(toScreen(pts[i - 3], pts[i - 2]), toScreen(pts[i], pts[i + 1]), Ui::Col(Ui::kAccent), Ui::S(2));
    for (size_t i = 0; i < ks.size(); ++i) {
        const ImVec2 p = toScreen(ks[i].pos[0], ks[i].pos[1]);
        const float yaw = ks[i].ang[1] * 3.14159265f / 180.f;
        dl->AddLine(p, ImVec2(p.x + std::cos(yaw) * Ui::S(14), p.y - std::sin(yaw) * Ui::S(14)), Ui::Col(Ui::kText, 0.6f), Ui::S(1.5f));
        const bool s = (int)i == sel;
        dl->AddCircleFilled(p, Ui::S(s ? 6.f : 4.5f), s ? Ui::Col(Ui::kWarn) : Ui::Col(Ui::kText));
        if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            if ((m.x - p.x) * (m.x - p.x) + (m.y - p.y) * (m.y - p.y) < Ui::S(9) * Ui::S(9)) {
                std::lock_guard<std::mutex> g(Campath::lock);
                if (sequence) Campath::seq.selected = (int)i;
                else Campath::selected = (int)i;
            }
        }
        if (ks[i].cut) {   // a cut: a small bar across the point
            dl->AddLine(ImVec2(p.x - Ui::S(7), p.y + Ui::S(8)), ImVec2(p.x + Ui::S(7), p.y + Ui::S(8)), Ui::Col(Ui::kBad), Ui::S(2.5f));
        }
    }
    if (live.camKnown) {
        const ImVec2 c = toScreen(live.camPos[0], live.camPos[1]);
        dl->AddCircle(c, Ui::S(7), Ui::Col(live.playing ? Ui::kGood : Ui::kMuted), 0, Ui::S(2));
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x + Ui::S(6), o.y + sz.y - ImGui::GetTextLineHeight() - Ui::S(4)));
    ImGui::TextDisabled(sequence ? "top-down, grid 256 units - dots: points (click one), red bar: cut, ring: camera now"
                                 : "top-down, grid 256 units - dots: keys (click one), red bar: cut, ring: camera now");
    ImGui::EndChild();
}

static void KeyEditor() {
    Campath::Key k;
    int sel, count;
    double t0 = 0;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        sel = Campath::selected;
        count = (int)Campath::keys.size();
        if (sel >= count) sel = Campath::selected = count - 1;
        if (count) t0 = Campath::keys.front().t;
        if (sel >= 0) k = Campath::keys[sel];
    }
    if (sel < 0) { Hint("Select a key (in the list or on the map) to edit it."); return; }
    ImGui::PushFont(Ui::bold, 0.f);
    ImGui::Text("Key %d of %d", sel + 1, count);
    ImGui::PopFont();
    bool edit = false;
    float rel = (float)(k.t - t0);
    ImGui::PushItemWidth(-Ui::S(90));
    if (sel > 0) edit |= ImGui::DragFloat("Time", &rel, 0.01f, 0.f, 3600.f, "+%.2f s");
    else ImGui::TextDisabled("Time: the path starts here (move it with Start here)");
    edit |= ImGui::DragFloat3("Position", k.pos, 1.f, -16384.f, 16384.f, "%.1f");
    edit |= ImGui::DragFloat3("Pitch/Yaw/Roll", k.ang, 0.2f, -360.f, 360.f, "%.1f");
    edit |= ImGui::SliderFloat("Field of view", &k.fov, 10.f, 140.f, "%.1f deg");
    if (sel + 1 < count) edit |= Ui::Toggle("Cut to the next key (hold this view until then)", &k.cut);
    ImGui::PopItemWidth();
    if (edit) {
        k.ang[0] = std::clamp(k.ang[0], -89.f, 89.f);
        if (sel > 0) k.t = t0 + std::max(0.02f, rel);
        if (!g_cpDragging) { Campath::SaveUndoStep(); g_cpDragging = true; }   // one undo step per drag, not per frame
        Campath::UpdateKey(sel, k);
    }
    if (!ImGui::IsAnyItemActive()) g_cpDragging = false;
    if (ImGui::Button("Set to current view", ImVec2(Ui::S(160), 0))) {
        if (Campath::SetKeyToView(sel)) CpNote("Key moved to the current view.", Ui::kGood);
        else CpNote("The camera isn't known yet - play the demo for a moment.", Ui::kWarn);
    }
    ImGui::SameLine();
    if (ImGui::Button("Go to", ImVec2(Ui::S(70), 0))) GoToTick(Campath::TickFor(k.t));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jumps the demo to this key's moment.");
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(Ui::S(70), 0))) Campath::RemoveKey(sel);
    ImGui::SameLine();
    if (ImGui::Button("Clear all", ImVec2(-1, 0))) { Campath::Clear(); CpNote("All keys removed (Undo brings them back).", Ui::kMuted); }
}

static void KeyList() {
    std::vector<Campath::Key> ks;
    int sel;
    {
        std::lock_guard<std::mutex> g(Campath::lock);
        ks = Campath::keys;
        sel = Campath::selected;
    }
    for (size_t i = 0; i < ks.size(); ++i) {
        char left[96], right[48], id[24];
        std::snprintf(left, sizeof left, "%2u   +%.2f s", (unsigned)i + 1, ks[i].t - ks.front().t);
        std::snprintf(right, sizeof right, "fov %.0f", ks[i].fov);
        std::snprintf(id, sizeof id, "##cpk%u", (unsigned)i);
        Ui::RowOpts o;
        o.right = right;
        o.rightCol = Ui::Col(Ui::kMuted);
        if (Ui::Row(id, left, (int)i == sel, o)) {
            std::lock_guard<std::mutex> g(Campath::lock);
            Campath::selected = (int)i;
        }
    }
}

static void CinematicsTab() {
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##cineleft", ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, Ui::S(360), Ui::S(470)), h), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    CinematicsLeft();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##cineright", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    PathMap(std::max(Ui::S(180), h * 0.42f), g_cpMode == 0);
    ImGui::Spacing();
    if (g_cpMode == 0) PointEditor();
    else KeyEditor();
    ImGui::Spacing();
    ImGui::BeginChild("##cp_keys", ImVec2(0, 0), ImGuiChildFlags_None);
    Ui::SmoothScroll();
    if (g_cpMode == 0) PointList();
    else KeyList();
    ImGui::EndChild();
    ImGui::EndChild();
}

// ------------------------------------------------------------------ Record tab
// Kill recorder: scan the demo file, pick a player, choose clips, record with HLAE (motion blur, MP4).
static std::vector<Record::Clip> g_rcClips;
static uint64_t g_rcClipsFor = 0;
static std::string g_rcClipsScan;
static float g_rcBuiltBefore = -1, g_rcBuiltAfter = -1;
static bool g_rcBuiltMerge = false;
static char g_rcPath[MAX_PATH] = "";

static bool BrowseDemo(char* path) {
    char file[MAX_PATH];
    std::snprintf(file, sizeof file, "%s", path);
    std::string dir = Game::GameFolder();
    OPENFILENAMEA o{};
    o.lStructSize = sizeof o;
    o.lpstrFilter = "CS2 demos (*.dem)\0*.dem\0All files\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.lpstrInitialDir = path[0] ? nullptr : (dir.empty() ? nullptr : dir.c_str());
    o.lpstrTitle = "The demo you are watching";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&o)) return false;
    std::snprintf(path, MAX_PATH, "%s", file);
    return true;
}

// Motion blur, frame rate, stretch, quality - shared by the Record tab and the Cinematics tab (same settings)
static bool g_rcSaveRecorder = false;   // the recorder choice changed: save it (outside Record::lock)
static void VideoSettings(bool killClips) {
    if (g_rcSaveRecorder) { g_rcSaveRecorder = false; Record::SaveSettings(); }
    int gw = 0, gh = 0, sw = 0, sh = 0;   // CS2's size, looked up outside the lock (see Record::GameSize)
    const bool known = Record::GameSize(gw, gh);
    const bool narrow = known && Record::StretchSize(gw, gh, sw, sh);
    {
        std::lock_guard<std::mutex> g(Record::lock);
        // which recorder: HLAE's mirv_streams or DTRecord (DemoTools2's own) - the one chosen here is used
        static const char* kRec[] = {"HLAE", "DTRecord"};
        CpLabel("Recorder");
        if (Ui::Segmented("##rc_rec", kRec, 2, &Record::set.recorder)) g_rcSaveRecorder = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("HLAE: HLAE's recording (mirv_streams), as before.\n"
                              "DTRecord: DemoTools2's own recorder. It doesn't use HLAE's recording, so it keeps working when\n"
                              "HLAE isn't updated for the newest CS2 yet. It reads the picture from DirectX 11, does the motion\n"
                              "blur itself, and records the game sound with it.");
        {
            std::string why;
            const std::string use = Record::RecorderFor(Record::set.recorder, &why);
            if (use.empty()) ImGui::TextColored(Ui::kWarn, "Can't record: %s.", why.c_str());
            else if (use == "HLAE") {
                Hint("Records with HLAE's recording (mirv_streams).");
                const std::string warn = Record::HlaeWarning();
                if (!warn.empty()) ImGui::TextColored(Ui::kWarn, "%s", warn.c_str());
            } else Hint("Records with DTRecord, DemoTools2's own recorder (HLAE's recording isn't used).");
            if (Record::set.recorder == Record::Rec_DTRecord) {
                if (Ui::Toggle("Game sound", &Record::set.sound)) Record::SaveSettings();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Records CS2's own sound into the clips (Windows 10 version 2004 or newer).\n"
                                      "Off: clips without sound.");
                if (Record::set.sound) {
                    static const char* kPass[] = {"With the picture", "Second pass"};
                    int sel = Record::set.samePass ? 0 : 1;
                    CpLabel("Sound");
                    if (Ui::Segmented("##rc_pass", kPass, 2, &sel)) {
                        Record::set.samePass = sel == 0;
                        Record::SaveSettings();
                    }
                    if (Record::set.samePass)
                        Hint("Each clip plays once in real time; picture and sound are recorded together and saved as one MP4. "
                             "Motion blur blends the frames CS2 shows, so it is smoother the higher CS2's FPS.");
                    else
                        Hint("The picture is recorded at a fixed game step (exact motion blur), then the clip plays again in "
                             "real time for the sound; both are joined into one MP4.");
                }
            }
        }
        Ui::Toggle("Motion blur", &Record::set.blur);
        ImGui::PushItemWidth(-Ui::S(130));
        static const char* kFps[] = {"30", "60", "120"};
        int fpsSel = Record::set.fps <= 30 ? 0 : Record::set.fps >= 120 ? 2 : 1;
        CpLabel("Frame rate");
        if (Ui::Segmented("##rc_fps", kFps, 3, &fpsSel)) Record::set.fps = fpsSel == 0 ? 30 : fpsSel == 2 ? 120 : 60;
        const bool realTime = Record::set.recorder == Record::Rec_DTRecord && Record::set.sound && Record::set.samePass;
        if (Record::set.blur && realTime) {
            ImGui::SliderFloat("Blur amount", &Record::set.blurAmount, 0.1f, 1.f, "%.2f");
            Hint("Recording in real time: the blur blends the frames CS2 shows in each video frame's time - the higher CS2's FPS, "
                 "the more frames (300 FPS, 60 fps video, amount 0.5: 2-3). For the exact fixed-step blur, set Sound to Second pass.");
        } else if (Record::set.blur) {
            static const char* kSmp[] = {"4", "8", "16", "32"};
            int smpSel = Record::set.samples <= 4 ? 0 : Record::set.samples <= 8 ? 1 : Record::set.samples <= 16 ? 2 : 3;
            CpLabel("Blur samples");
            if (Ui::Segmented("##rc_smp", kSmp, 4, &smpSel)) Record::set.samples = 4 << smpSel;
            ImGui::SliderFloat("Blur amount", &Record::set.blurAmount, 0.1f, 1.f, "%.2f");
            Hint("More samples = smoother blur, slower to record (the game runs at frame rate x samples).");
        }
        {
            Ui::Toggle("Stretch 4:3 to 16:9", &Record::set.stretch);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("When CS2 runs in a 4:3 (or 5:4) resolution, the clips are stretched to 16:9 - the stretched look players use.");
            if (!known) Hint("CS2's resolution isn't known yet.");
            else if (narrow) Hint(Record::set.stretch ? "CS2 is %dx%d (4:3): clips come out stretched to %dx%d." : "CS2 is %dx%d (4:3): clips stay 4:3.", gw, gh, sw, sh);
            else Hint("CS2 is %dx%d (wide): nothing to stretch.", gw, gh);
        }
        ImGui::SliderInt("Quality (CRF)", &Record::set.crf, 12, 30, "%d");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lower = better and bigger files. 18 looks clean.");
        ImGui::PopItemWidth();
        if (killClips) {
            Ui::Toggle("Save each clip as its own MP4", &Record::set.clips);
            Ui::Toggle("Join all clips into one MP4", &Record::set.combined);
            if (!Record::set.clips && !Record::set.combined) Record::set.combined = true;
        }
    }

}

static void RecordLeft() {
    const Record::Scan sc = Record::ScanState();
    const Record::Progress pr = Record::Status();

    Hint("Records every kill of one player from the demo you are watching, with motion blur, as MP4 files - through HLAE's "
         "recording or DTRecord, DemoTools2's own (Recorder, under 5. Video).");
    Section("1. The demo");
    {
        std::string file = sc.path;
        const size_t sl = file.find_last_of("\\/");
        if (sl != std::string::npos) file = file.substr(sl + 1);
        if (sc.busy) Hint("Reading %s for kills...", file.c_str());
        else if (sc.result.ok) {
            ImGui::TextUnformatted(file.c_str());
            Hint("%s - %u rounds, %u kills.", sc.result.map.c_str(), (unsigned)sc.result.roundStarts.size(), (unsigned)sc.result.kills.size());
            if (ImGui::SmallButton("Not this demo? Choose the file") && BrowseDemo(g_rcPath)) Record::StartScan(g_rcPath);
        } else if (!sc.path.empty()) Hint("Couldn't read %s: %s", file.c_str(), sc.result.error.c_str());
        else if (!Skins::demoPlaying) Hint("Play a demo - DemoTools2 finds its file and reads every kill by itself.");
        else if (Record::AutoFindFailed()) {
            Hint("CS2 didn't show which file it is playing. Pick it once:");
            if (ImGui::Button("Choose the demo file", ImVec2(Ui::S(170), 0)) && BrowseDemo(g_rcPath)) Record::StartScan(g_rcPath);
        } else Hint("Finding the demo CS2 is playing...");
    }

    Section("2. The player");
    {
        const auto who = Record::PlayersWithKills();
        int cur;
        {
            std::lock_guard<std::mutex> g(Record::lock);
            cur = Record::set.slot;
        }
        std::string preview = "Choose a player";
        for (const auto& w : who) if (w.slot == cur) preview = w.name + " - " + std::to_string(w.kills) + " kills";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##rc_player", preview.c_str())) {
            Ui::SmoothScroll();
            for (const auto& w : who) {
                char label[220];
                std::snprintf(label, sizeof label, "%s %s - %d kills##%d", w.team == 2 ? "T " : w.team == 3 ? "CT" : "  ", w.name.c_str(), w.kills, w.slot);
                if (ImGui::Selectable(label, cur == w.slot)) {
                    std::lock_guard<std::mutex> g(Record::lock);
                    Record::set.slot = w.slot;
                    Record::set.xuid = w.xuid;
                }
            }
            ImGui::EndCombo();
        }
        if (who.empty()) Hint("Read the demo file first.");
    }

    Section("3. Clips");
    {
        std::lock_guard<std::mutex> g(Record::lock);
        ImGui::PushItemWidth(-Ui::S(130));
        ImGui::SliderFloat("Before a kill", &Record::set.before, 0.5f, 10.f, "%.1f s");
        ImGui::SliderFloat("After a kill", &Record::set.after, 0.5f, 10.f, "%.1f s");
        ImGui::PopItemWidth();
        Ui::Toggle("Kills close together in one clip (multi-kills)", &Record::set.merge);
    }

    Section("4. View");
    {
        static const char* kView[] = {"POV with HUD", "Clean (no HUD)"};
        std::lock_guard<std::mutex> g(Record::lock);
        Ui::Segmented("##rc_view", kView, 2, &Record::set.view);
        if (Record::set.view == 1) Ui::Toggle("Show the gun and hands", &Record::set.hands);
        else Hint("The player's own HUD through DemoTools2's POV HUD (Cinematics tab).");
    }

    Section("5. Video");
    VideoSettings(true);

    Section("Output");
    {
        const std::string out = Record::OutDir();
        Hint("%s", out.c_str());
        if (ImGui::Button("Open folder##rc", ImVec2(Ui::S(110), 0))) {
            CreateDirectoryA((DllFolder() + "recordings").c_str(), nullptr);
            CreateDirectoryA(out.c_str(), nullptr);
            ShellExecuteA(nullptr, "open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        const std::string ff = Record::FfmpegPath();
        if (ff.empty()) Hint("ffmpeg.exe not found automatically (looked in: %s). Pick it below - HLAE's ffmpeg folder "
                             "(ffmpeg\\bin\\ffmpeg.exe), or put ffmpeg.exe in DemoTools2's folder.", Record::FfmpegSearched().c_str());
        else Hint("ffmpeg: %s", ff.c_str());
        if (ImGui::Button("Choose ffmpeg.exe", ImVec2(Ui::S(150), 0))) {
            char file[MAX_PATH] = "";
            OPENFILENAMEA o{};
            o.lStructSize = sizeof o;
            o.lpstrFilter = "ffmpeg.exe\0ffmpeg.exe\0Programs\0*.exe\0";
            o.lpstrFile = file;
            o.nMaxFile = MAX_PATH;
            o.lpstrTitle = "ffmpeg.exe";
            o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&o)) {
                std::lock_guard<std::mutex> g(Record::lock);
                Record::set.ffmpeg = file;
            }
        }
    }
    (void)pr;
}

static void RecordRight() {
    const Record::Scan sc = Record::ScanState();
    const Record::Progress pr = Record::Status();
    uint64_t xuid;
    float before, after;
    bool merge;
    {
        std::lock_guard<std::mutex> g(Record::lock);
        xuid = (uint64_t)(Record::set.slot + 1);   // "who" for the rebuild check (slot based)
        before = Record::set.before;
        after = Record::set.after;
        merge = Record::set.merge;
    }
    // rebuild the list when the player, the scan or the timing changed (keeps the ticks you set off)
    if (!pr.running && (xuid != g_rcClipsFor || sc.path + std::to_string(sc.result.kills.size()) != g_rcClipsScan || before != g_rcBuiltBefore ||
                        after != g_rcBuiltAfter || merge != g_rcBuiltMerge)) {
        std::vector<bool> off;
        for (auto& c : g_rcClips) off.push_back(!c.on);
        auto fresh = Record::BuildClips();
        if (xuid == g_rcClipsFor && fresh.size() == off.size())
            for (size_t i = 0; i < fresh.size(); ++i) fresh[i].on = !off[i];
        if (!fresh.empty() || xuid != g_rcClipsFor) g_rcClips = fresh;
        g_rcClipsFor = xuid;
        g_rcClipsScan = sc.path + std::to_string(sc.result.kills.size());
        g_rcBuiltBefore = before;
        g_rcBuiltAfter = after;
        g_rcBuiltMerge = merge;
    }
    int on = 0;
    float secs = 0;
    for (auto& c : g_rcClips) if (c.on) { ++on; secs += (c.end - c.start) / 64.f; }

    if (pr.running) {
        Ui::StatusPill("Recording", Ui::kBad);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("clip %d of %d", pr.clip, pr.clips);
        Hint("%s", pr.state.c_str());
        if (Ui::PrimaryButton("Stop", ImVec2(Ui::S(120), 0))) Record::Stop();
        Hint("Leave CS2 alone while it records: it jumps through the demo by itself.");
    } else {
        char label[64];
        std::snprintf(label, sizeof label, "Record %d clip%s", on, on == 1 ? "" : "s");
        const bool playing = Skins::demoPlaying;
        int recorder;
        {
            std::lock_guard<std::mutex> g(Record::lock);
            recorder = Record::set.recorder;
        }
        std::string why;
        const bool canRec = !Record::RecorderFor(recorder, &why).empty();
        ImGui::BeginDisabled(on == 0 || !canRec || !playing);
        if (Ui::PrimaryButton(label, ImVec2(Ui::S(170), 0))) Record::Start(g_rcClips);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%.0f s of video", secs);
        if (on > 0 && !playing) Hint("Play this demo in CS2 (pausing is fine) - recording jumps through it.");
        else if (on > 0 && !canRec) ImGui::TextColored(Ui::kWarn, "Can't record: %s.", why.c_str());
        if (!pr.state.empty()) Hint("Last run: %s", pr.state.c_str());
    }
    if (!pr.post.empty()) Hint("%s", pr.post.c_str());
    if (pr.done.size() > 3) ImGui::TextDisabled("%u files saved, newest:", (unsigned)pr.done.size());
    for (size_t i = pr.done.size() > 3 ? pr.done.size() - 3 : 0; i < pr.done.size(); ++i) ImGui::TextDisabled("saved: %s", pr.done[i].c_str());
    ImGui::Spacing();

    if (g_rcClips.empty()) {
        if (!sc.result.ok) Hint("Read the demo file first (left), then choose a player.");
        else if (xuid == 0) Hint("Choose the player whose kills to record.");
        else Hint("No kills for this player in this demo.");
        return;
    }
    if (ImGui::SmallButton("All")) for (auto& c : g_rcClips) c.on = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) for (auto& c : g_rcClips) c.on = false;
    ImGui::BeginChild("##rc_list", ImVec2(0, 0), ImGuiChildFlags_None);
    Ui::SmoothScroll();
    if (ImGui::BeginTable("##rc_clips", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Round", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Kills", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        // pr.clip counts the clips that are on: find the row it is
        int current = -1;
        if (pr.running)
            for (int i = 0, k = 0; i < (int)g_rcClips.size(); ++i)
                if (g_rcClips[i].on && ++k == pr.clip) { current = i; break; }
        for (size_t i = 0; i < g_rcClips.size(); ++i) {
            Record::Clip& c = g_rcClips[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(pr.running);
            ImGui::Checkbox("##on", &c.on);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (c.round > 0) ImGui::Text("%d", c.round);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if ((int)i == current) ImGui::TextColored(Ui::kAccent, "%s", c.summary.c_str());
            else if (c.on) ImGui::TextUnformatted(c.summary.c_str());
            else ImGui::TextDisabled("%s", c.summary.c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%.1f s", (c.end - c.start) / 64.f);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(pr.running);
            if (ImGui::SmallButton("Watch")) GoToTick(std::max(0, c.start - 64));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Jump the demo to one second before this clip.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static void RecordTab() {
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##recleft", ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, Ui::S(360), Ui::S(470)), h), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    RecordLeft();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##recright", ImVec2(0, h), ImGuiChildFlags_Borders);
    RecordRight();
    ImGui::EndChild();
}

// ------------------------------------------------------------------ Status tab
// What was found in this CS2 build. After a CS2 or HLAE update, parts that weren't found switch themselves off and say
// so here; the menu and everything else keep working.
static void StatusTab() {
    const auto feats = Features();
    int nOk = 0, nLim = 0, nOff = 0;
    for (const FeatureInfo& f : feats) (f.state == Feat_Ok ? nOk : f.state == Feat_Limited ? nLim : nOff)++;
    ImGui::BeginChild("##status", ImVec2(0, 0), ImGuiChildFlags_None);
    Ui::SmoothScroll();
    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = std::max(Ui::S(120), (ImGui::GetContentRegionAvail().x - sp * 2) / 3.f);
        char a[16], b[16], c[16];
        std::snprintf(a, sizeof a, "%d", nOk);
        std::snprintf(b, sizeof b, "%d", nLim);
        std::snprintf(c, sizeof c, "%d", nOff);
        Ui::StatTile(a, "Working", Ui::kGood, w);
        ImGui::SameLine();
        Ui::StatTile(b, "Limited", nLim ? Ui::kWarn : Ui::kMuted, w);
        ImGui::SameLine();
        Ui::StatTile(c, "Off", nOff ? Ui::kBad : Ui::kMuted, w);
    }
    Ui::Section("Features");
    if (feats.empty()) Hint("Starting...");
    for (const FeatureInfo& f : feats) {
        const ImVec4 col = f.state == Feat_Ok ? Ui::kGood : f.state == Feat_Limited ? Ui::kWarn : Ui::kBad;
        const char* word = f.state == Feat_Ok ? "Working" : f.state == Feat_Limited ? "Limited" : "Off";
        char id[48];
        std::snprintf(id, sizeof(id), "##feat_%s", f.key.c_str());
        Ui::RowOpts o;
        o.right = word;
        o.rightCol = Ui::Col(col);
        o.dot = Ui::Col(col);
        Ui::Row(id, f.label.c_str(), false, o);
        if (!f.note.empty()) {
            ImGui::Indent(Ui::S(11));
            Hint("%s", f.note.c_str());
            ImGui::Unindent(Ui::S(11));
        }
    }
    const auto crashed = CrashedSteps();
    if (!crashed.empty()) {
        Ui::Section("Switched off for this session");
        Hint("These crashed last time, so they stay off until CS2 restarts. If one keeps crashing, send DemoTools2.previous.log.");
        for (auto& c : crashed) {
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::kWarn);
            ImGui::BulletText("%s", c.c_str());
            ImGui::PopStyleColor();
        }
    }
    Ui::Section("This copy");
    if (g_gameBuild) Hint("DemoTools2 %s, CS2 build %d.", DEMOTOOLS2_VERSION, g_gameBuild);
    else Hint("DemoTools2 %s.", DEMOTOOLS2_VERSION);
    Hint("After a CS2 or HLAE update, anything that wasn't found switches itself off and shows here; the rest keeps working. "
         "Skin parts may need a DemoTools2 update. If something is off, send DemoTools2.log from the DemoTools2 folder.");
    ImGui::Spacing();
    if (ImGui::Button("Open DemoTools2 folder")) ShellExecuteA(nullptr, "open", DllFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Open the log")) ShellExecuteA(nullptr, "open", (DllFolder() + "DemoTools2.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::EndChild();
}

// ------------------------------------------------------------------ the window
enum Page { Page_Camera, Page_Cinematics, Page_Record, Page_Skins, Page_Status, Page_Count };
static int g_page = Page_Skins;

static void SkinsPage() {
    // toolbar
    {
        Ui::Toggle("Skins on", &Skins::settings.enabled);
        ImGui::SameLine(0, Ui::S(18));
        if (Ui::Toggle("Knife models", &Skins::settings.knifeModels)) g_saveAt = GetTickCount64();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: knives keep their model, only the finish changes.");
        ImGui::SameLine(0, Ui::S(18));
        if (Ui::Toggle("Gloves", &Skins::settings.gloves)) g_saveAt = GetTickCount64();
        ImGui::SameLine(0, Ui::S(18));
        if (Ui::Toggle("Agents", &Skins::settings.agents)) g_saveAt = GetTickCount64();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: everyone keeps their own player model.");
        ImGui::SameLine(0, Ui::S(18));
        if (Ui::Toggle("Any model", &Skins::settings.anyModel)) g_saveAt = GetTickCount64();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Allow knives, gloves and agents nobody in this demo has.\n"
                              "DemoTools2 then sets ag2_use_networked_serialization_context_demo 0 so CS2 uses its own\n"
                              "animation data instead of the demo's (without it, those models crash CS2).\n"
                              "If player animations look wrong in a demo, turn this off.\n"
                              "In demos from an older CS2 version this stays off (CS2 crashes there),\n"
                              "unless 'any_model_old_demos=1' is set in DemoTools2.cfg.");
        const char* refresh = "Refresh now";
        const float bw = ImGui::CalcTextSize(refresh).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Ui::S(12), ImGui::GetContentRegionMax().x - bw));
        if (ImGui::Button(refresh)) Skins::RequestSeek();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reloads this moment of the demo (jumps 1 tick back). Skins normally update by themselves.");
        if (!Skins::knifeNote.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::kWarn);
            ImGui::TextWrapped("%s", Skins::knifeNote.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::Spacing();

    const float h = ImGui::GetContentRegionAvail().y;
    const float total = ImGui::GetContentRegionAvail().x;
    const float wPlayers = std::clamp(total * 0.26f, Ui::S(200), Ui::S(300));
    const float wSlots = std::clamp(total * 0.32f, Ui::S(250), Ui::S(380));
    ImGui::BeginChild("##players", ImVec2(wPlayers, h), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    PlayersColumn();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##guns", ImVec2(wSlots, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        const float f = Ui::FadeIn("slots", g_player);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.3f + 0.7f * f));
        GunsColumn();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##skin", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        const float f = Ui::FadeIn("skin", g_player * 131 + (uint64_t)(g_slot + 8) * 7 + (uint64_t)g_side);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.3f + 0.7f * f));
        SkinColumn();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
}

static void Sidebar(int offCount, int limitedCount) {
    ImGui::PushFont(Ui::bold, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("DemoTools2");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
    ImGui::Text("Version %s", DEMOTOOLS2_VERSION);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Ui::S(10)));

    struct Item { int page; int icon; const char* label; const char* sub; };
    static const Item kItems[] = {
        {Page_Camera, Ui::Icon_Camera, "Camera", "Follow and attach"},
        {Page_Cinematics, Ui::Icon_Cinematics, "Cinematics", "Smooth campaths"},
        {Page_Record, Ui::Icon_Record, "Record", "Kill clips to MP4"},
        {Page_Skins, Ui::Icon_Skins, "Skins", "Finishes and knives"},
        {Page_Status, Ui::Icon_Status, "Status", "This CS2 build"},
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, Ui::S(3)));
    for (const Item& it : kItems) {
        char id[24];
        std::snprintf(id, sizeof id, "##nav%d", it.page);
        int badge = 0;
        ImVec4 bc = Ui::kMuted;
        if (it.page == Page_Status) {
            if (offCount) { badge = offCount; bc = Ui::kBad; }
            else if (limitedCount) { badge = limitedCount; bc = Ui::kWarn; }
        }
        if (it.page == Page_Record && Record::Status().running) { badge = 1; bc = Ui::kBad; }
        if (Ui::NavItem(id, it.icon, it.label, it.sub, g_page == it.page, badge, bc)) g_page = it.page;
        if (it.page == Page_Record && badge && ImGui::IsItemHovered()) ImGui::SetTooltip("Recording now");
    }
    ImGui::PopStyleVar();

    // bottom: what DemoTools2 is doing right now
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const float footer = ImGui::GetFrameHeight() + lh * 3.2f + Ui::S(8);
    const float y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - footer;
    if (y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(y);
    const ImVec4 col = !Skins::demoPlaying ? Ui::kWarn : Skins::anyModelBlocked ? Ui::kWarn : Ui::kGood;
    Ui::StatusPill(Skins::demoPlaying ? "Demo playing" : "No demo", col);
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(Skins::demoPlaying && Skins::anyModelBlocked
                               ? "Older demo: only knife, glove and agent models someone in it has can be used."
                               : Skins::lastStatus.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void Draw(bool fillWindow) {
    std::lock_guard<std::mutex> guard(Skins::lock);
    if (g_saveAt && GetTickCount64() >= g_saveAt) { g_saveAt = 0; Skins::Save(); }

    if (fillWindow) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    } else {
        ImGui::SetNextWindowSize(ImVec2(Ui::S(1180), Ui::S(720)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("DemoTools2", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     (fillWindow ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus : 0));
    ImGui::PopStyleVar();

    int offCount = 0, limitedCount = 0;
    for (auto& f : Features()) {
        if (f.state == Feat_Off && f.key != "camera" && f.key != "console") ++offCount;
        if (f.state == Feat_Limited) ++limitedCount;
    }

    // sidebar
    const float sideW = Ui::S(224);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.043f, 0.055f, 0.067f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(12), Ui::S(16)));
    ImGui::BeginChild("##side", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Sidebar(offCount, limitedCount);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(b.x - 1, a.y), ImVec2(b.x - 1, b.y), Ui::Col(ImVec4(1, 1, 1, 0.06f)));
    }
    ImGui::SameLine(0, 0);

    // page
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Ui::S(20), Ui::S(16)));
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    {
        static const char* kTitle[] = {"Camera", "Cinematics", "Record", "Skins", "Status"};
        static const char* kSub[] = {
            "Follow a player or bolt the camera to a part of them. Every move is a dt2_calc you can change live.",
            "Build a campath with the demo paused, let DemoTools2 time it, then preview and record it.",
            "Every kill of one player in the demo you are watching, with motion blur, saved as MP4.",
            "Give any player any finish, knife, gloves or agent. Saved per player and used in every demo.",
            "What DemoTools2 found in this CS2 build."};
        const bool beta = g_page == Page_Camera;
        Ui::PageHeader(kTitle[g_page], kSub[g_page], beta ? "Beta" : nullptr, Ui::kWarn);
    }
    const float fade = Ui::FadeIn("main", (uint64_t)g_page);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f + 0.7f * fade);
    switch (g_page) {
        case Page_Camera: CameraTab(); break;
        case Page_Cinematics: CinematicsTab(); break;
        case Page_Record: RecordTab(); break;
        case Page_Status: StatusTab(); break;
        default: SkinsPage(); break;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
}
}  // namespace Menu
