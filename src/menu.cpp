// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
// The DemoTools2 window. Skins tab: pick a player, pick a gun / knife / gloves, pick the finish.
// Camera tab: follow a player with one click, see every calc live, presets, and a dt2_ command line.
#include "calc.h"
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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Menu {
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
static int g_openTab = -1;
static int g_gunTab = 0;            // gun editor: 0 finish, 1 stickers, 2 charm          // switch to this main tab on the next frame (-1: leave it)

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

static void ApplyAttach(bool quiet) {
    char b[256];
    Calc::SetQuiet(quiet);
    uint64_t xuid = g_atXuid ? g_atXuid : Calc::ObservedXuid();
    if (xuid) std::snprintf(b, sizeof(b), "dt2_calc handle add at_target player %llu", (unsigned long long)xuid);
    else std::snprintf(b, sizeof(b), "dt2_calc handle add at_target observed");
    Console::Run(b);
    const Part& p = kParts[g_atPart];
    std::string kind = p.kind;
    if (kind == "att" || kind == "gun") Console::Run("dt2_calc handle add at_gun weapon at_target");
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
    std::snprintf(b, sizeof(b), "dt2_calc vecang add at_mount offset %s %.1f %.1f %.1f %.1f %.1f %.1f", base, g_atFwd, g_atRight, g_atUp, g_atPitch,
                  g_atYaw, g_atRoll);
    Console::Run(b);
    std::string view = "at_mount";
    if (g_atTurn == 3) { Console::Run("dt2_calc vecang add at_look lookat at_mount at_eye"); view = "at_look"; }
    if (g_atSmooth > 0) {
        std::snprintf(b, sizeof(b), "dt2_calc vecang add at_cam smooth %s %.2f %.2f", view.c_str(), g_atSmooth, g_atSmooth);
        Console::Run(b);
        view = "at_cam";
    }
    Console::Run("dt2_cam source " + view);
    if (g_atFov > 0) {
        std::snprintf(b, sizeof(b), "dt2_calc fov add at_fov value %.0f", g_atFov);
        Console::Run(b);
        Console::Run("dt2_cam fov at_fov");
    } else {
        Console::Run("dt2_cam fov game");
    }
    Calc::SetQuiet(false);
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
    if (changed && g_atLive) ApplyAttach(true);   // live while you drag
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
    ImGui::TextWrapped("Record it with HLAE as usual (mirv_streams). The camera only changes the view while a demo plays.");
}

static void CameraPanel() {
    Ui::Heading("Camera (WIP)");
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
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Ui::S(22));
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
    ImGui::SetNextItemWidth(-Ui::S(130));
    bool enter = ImGui::InputTextWithHint("##cmd", "dt2_calc / dt2_cam / dt2_preset command", g_cmdLine, sizeof(g_cmdLine),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Run", ImVec2(Ui::S(60), 0)) || enter) && g_cmdLine[0]) {
        std::string line = g_cmdLine;
        if (line.rfind("dt2_", 0) != 0) line = "dt2_calc " + line;
        Run(line);
        g_cmdLine[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Help", ImVec2(-1, 0))) Run("dt2_calc");
    ImGui::BeginChild("##out", ImVec2(0, outH), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    for (auto& l : Console::RecentOutput()) ImGui::TextUnformatted(l.c_str());
    if (g_scrollOut) { ImGui::SetScrollHereY(1.0f); g_scrollOut = false; }
    ImGui::EndChild();
}

static void CameraTab() {
    ImGui::PushStyleColor(ImGuiCol_Text, Ui::kWarn);
    ImGui::TextWrapped("Work in progress: the camera tools are still being built and may change or not work in every demo.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##camleft", ImVec2(std::clamp(ImGui::GetContentRegionAvail().x * 0.38f, Ui::S(340), Ui::S(440)), h), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    if (ImGui::BeginTabBar("##camtabs")) {
        if (ImGui::BeginTabItem("Attach to a part")) { AttachPanel(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Follow a player")) { FollowPanel(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    CameraPanel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##camright", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    CalcsPanel();
    ImGui::EndChild();
}

// ------------------------------------------------------------------ Status tab
// What was found in this CS2 build. After a CS2 or HLAE update, parts that weren't found switch themselves off and say
// so here; the menu and everything else keep working.
static void StatusTab() {
    const auto feats = Features();
    ImGui::BeginChild("##status", ImVec2(0, 0), ImGuiChildFlags_Borders);
    Ui::SmoothScroll();
    Ui::Heading("Status");
    if (g_gameBuild) Hint("DemoTools2 %s  ·  CS2 build %d", DEMOTOOLS2_VERSION, g_gameBuild);
    else Hint("DemoTools2 %s", DEMOTOOLS2_VERSION);
    ImGui::Spacing();
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
        ImGui::Spacing();
        SubHeading("Switched off for this session (crashed last time)");
        for (auto& c : crashed) Hint("%s", c.c_str());
        Hint("They come back on the next start. If one keeps crashing, send DemoTools2.previous.log.");
    }
    ImGui::Spacing();
    Hint("After a CS2 or HLAE update: anything that wasn't found switches itself off and shows here, the rest keeps "
         "working. Skin parts may need a DemoTools2 update. Send DemoTools2.log from the DemoTools2 folder if something is off.");
    ImGui::Spacing();
    if (ImGui::Button("Open DemoTools2 folder")) ShellExecuteA(nullptr, "open", DllFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::EndChild();
}

void Draw(bool fillWindow) {
    std::lock_guard<std::mutex> guard(Skins::lock);
    if (g_saveAt && GetTickCount64() >= g_saveAt) { g_saveAt = 0; Skins::Save(); }

    if (fillWindow) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    } else {
        ImGui::SetNextWindowSize(ImVec2(Ui::S(1080), Ui::S(680)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("DemoTools2", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     (fillWindow ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus : 0));

    // Header: name, version, and what DemoTools2 is doing right now.
    {
        ImGui::PushFont(Ui::bold, ImGui::GetStyle().FontSizeBase * 1.3f);
        ImGui::TextUnformatted("DemoTools2");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, Ui::kMuted);
        ImGui::TextUnformatted(DEMOTOOLS2_VERSION);
        ImGui::PopStyleColor();
        const std::string& st = Skins::lastStatus;
        const ImVec4 col = !Skins::demoPlaying ? Ui::kWarn : Skins::anyModelBlocked ? Ui::kWarn : Ui::kGood;
        const float w = ImGui::CalcTextSize(st.c_str()).x + Ui::S(36);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Ui::S(16), ImGui::GetContentRegionMax().x - w));
        Ui::StatusPill(st.c_str(), col);
        if (Skins::demoPlaying && Skins::anyModelBlocked && ImGui::IsItemHovered())
            ImGui::SetTooltip("Older demo: only knife, glove and agent models someone in this demo has can be used.");
    }
    ImGui::Spacing();

    static int mainTab = 0;
    if (!ImGui::BeginTabBar("##tabs")) { ImGui::End(); return; }
    if (ImGui::BeginTabItem("Camera (WIP)###camera", nullptr, g_openTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { mainTab = 0; ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Skins", nullptr, g_openTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { mainTab = 1; ImGui::EndTabItem(); }
    {
        bool anyOff = false;
        for (auto& f : Features()) anyOff |= f.state == Feat_Off && f.key != "camera" && f.key != "console";
        if (ImGui::BeginTabItem(anyOff ? "Status (!)###status" : "Status###status", nullptr, g_openTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            mainTab = 2;
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
    g_openTab = -1;

    const float fade = Ui::FadeIn("main", (uint64_t)mainTab);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f + 0.7f * fade);
    if (mainTab == 2) {
        StatusTab();
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }
    if (mainTab == 0) {
        CameraTab();
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    // Skins toolbar
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
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
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
    const float wPlayers = std::clamp(total * 0.24f, Ui::S(200), Ui::S(300));
    const float wSlots = std::clamp(total * 0.30f, Ui::S(250), Ui::S(380));
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
    ImGui::PopStyleVar();
    ImGui::End();
}
}  // namespace Menu
