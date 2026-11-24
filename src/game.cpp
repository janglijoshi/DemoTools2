// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#include "game.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdlib>

// Fallback offsets: CS2 build 14196 (2026-11-09). Replaced by live schema values in Init().
Offsets O = {
    /*identity_designerName*/ 0x20, /*identitySize*/ 0x70,
    /*ent_pEntity*/ 0x10, /*ent_iTeamNum*/ 0x3e7, /*ent_hOwnerEntity*/ 0x520,
    /*ctrl_steamID*/ 0x788, /*ctrl_iszPlayerName*/ 0x6fc,
    /*pawn_hController*/ 0x14bc,
    /*econ_AttributeManager*/ 0x1290, /*econ_FallbackPaintKit*/ 0x18a8, /*econ_FallbackSeed*/ 0x18ac,
    /*econ_FallbackWear*/ 0x18b0, /*econ_FallbackStatTrak*/ 0x18b4,
    /*econ_OrigXuidLow*/ 0x18a0, /*econ_OrigXuidHigh*/ 0x18a4,
    /*container_Item*/ 0x50,
    /*item_DefIndex*/ 0x1ba, /*item_EntityQuality*/ 0x1bc, /*item_ItemIDHigh*/ 0x1d0, /*item_ItemIDLow*/ 0x1d4,
    /*item_AccountID*/ 0x1d8, /*item_CustomName*/ 0x2f8,
    /*item_AttributeList*/ 0x208, /*item_NetworkedDynamicAttributes*/ 0x280,
    /*attrlist_Attributes*/ 0x8,
    /*attr_DefIndex*/ 0x30, /*attr_Value*/ 0x34, /*attr_InitialValue*/ 0x38, /*attrSize*/ 0x48,
    /*weapon_CustomEconReloadEventId*/ 0x1b24,
    /*item_Initialized*/ 0x1e8,
    /*ctrl_hPlayerPawn*/ 0x92c,
    /*pawn_EconGloves*/ 0x1770, /*pawn_NeedToReApplyGloves*/ 0x176d, /*pawn_EconGlovesChanged*/ 0x1d20,
    /*ent_nSubclassID*/ 0x380, /*ent_pGameSceneNode*/ 0x330,
    /*skeleton_modelState*/ 0x140, /*modelState_ModelName*/ 0xa8,
    /*ent_VData*/ 0x388,
    /*modelState_MeshGroupMask*/ 0x208, /*item_ItemID*/ 0x1c8,
    /*modelState_BodyGroupChoices*/ 0x258,
};

namespace Game {
Indexes I;
void* client = nullptr;
void* engine = nullptr;
void* resourceService = nullptr;

using CreateInterfaceFn = void* (*)(const char*, int*);

static int g_interfaceFallbacks = 0;
static void* Interface(const char* module, const char* name) {
    HMODULE m = GetModuleHandleA(module);
    if (!m) { Log("ERROR: %s is not loaded.", module); return nullptr; }
    auto fn = (CreateInterfaceFn)GetProcAddress(m, "CreateInterface");
    if (!fn) { Log("ERROR: %s has no CreateInterface.", module); return nullptr; }
    void* p = fn(name, nullptr);
    if (p) return p;
    // A CS2 update can bump an interface's version number (Source2Client002 -> 003). Try the neighbours, newest first.
    const size_t n = std::strlen(name);
    if (n > 3 && std::isdigit((unsigned char)name[n - 1]) && std::isdigit((unsigned char)name[n - 2]) && std::isdigit((unsigned char)name[n - 3])) {
        const std::string base(name, n - 3);
        for (int v = 9; v >= 1 && !p; --v) {
            char alt[96];
            std::snprintf(alt, sizeof(alt), "%s%03d", base.c_str(), v);
            if (!std::strcmp(alt, name)) continue;
            if ((p = fn(alt, nullptr))) {
                Log("WARNING: interface %s not found in %s - using %s (CS2 update?)", name, module, alt);
                g_interfaceFallbacks++;
            }
        }
    }
    if (!p) Log("ERROR: interface %s not found in %s.", name, module);
    return p;
}

// ---------------------------------------------------------------- schema
// Layout of the schema system's tables (stable since 2024):
//   CSchemaSystem          +0x190 scope count (u64)   +0x198 scope array
//   CSchemaSystemTypeScope +0x008 module name          +0x470 declared class count (u16)  +0x478 entries (0x18 each, class at +0x10)
//   CSchemaDeclaredClass   +0x020 CSchemaClass*
//   CSchemaClass           +0x008 name  +0x020 size (u32)  +0x024 field count (u16)  +0x030 fields (0x20 each: name, type, offset u32)
struct SchemaClass { uint32_t size = 0; std::unordered_map<std::string, uint32_t> fields; };
static std::unordered_map<std::string, SchemaClass> g_schema;

static bool ReadSchema() {
    uint8_t* sys = (uint8_t*)Interface("schemasystem.dll", "SchemaSystem_001");
    if (!sys || !Readable(sys + 0x190, 16)) return false;
    uint64_t scopeCount = Get<uint64_t>(sys, 0x190);
    uint8_t** scopes = Get<uint8_t**>(sys, 0x198);
    if (scopeCount == 0 || scopeCount > 256 || !Readable(scopes, scopeCount * 8)) { Log("schema: bad scope table"); return false; }

    // client.dll first (its classes win when two modules declare the same name), then every other module's classes
    // that client.dll doesn't have (e.g. the model data structures, for reading a model's skeleton).
    auto readScope = [&](uint8_t* scope, bool onlyNew) -> int {
        uint16_t n = Get<uint16_t>(scope, 0x470);
        uint8_t* entries = Get<uint8_t*>(scope, 0x478);
        if (!Readable(entries, (size_t)n * 0x18)) return -1;
        int added = 0;
        for (uint16_t j = 0; j < n; ++j) {
            uint8_t* declared = Get<uint8_t*>(entries, j * 0x18 + 0x10);
            if (!Readable(declared, 0x28)) continue;
            uint8_t* cls = Get<uint8_t*>(declared, 0x20);
            if (!Readable(cls, 0x38)) continue;
            const char* name = Get<const char*>(cls, 0x8);
            if (!Readable(name, 1)) continue;
            if (onlyNew && g_schema.count(name)) continue;
            SchemaClass sc;
            sc.size = Get<uint32_t>(cls, 0x20);
            uint16_t nf = Get<uint16_t>(cls, 0x24);
            uint8_t* fields = Get<uint8_t*>(cls, 0x30);
            if (nf && Readable(fields, (size_t)nf * 0x20)) {
                for (uint16_t k = 0; k < nf; ++k) {
                    const char* fn = Get<const char*>(fields, k * 0x20);
                    if (Readable(fn, 1)) sc.fields[fn] = Get<uint32_t>(fields, k * 0x20 + 0x10);
                }
            }
            g_schema[name] = std::move(sc);
            ++added;
        }
        return added;
    };
    bool client = false;
    for (uint64_t i = 0; i < scopeCount; ++i) {
        uint8_t* scope = scopes[i];
        if (!Readable(scope, 0x480) || strncmp((const char*)scope + 8, "client.dll", 11) != 0) continue;
        if (readScope(scope, false) < 0) { Log("schema: bad class table"); return false; }
        client = true;
        Log("schema: %u classes from client.dll", (unsigned)g_schema.size());
        break;
    }
    if (!client) { Log("schema: client.dll scope not found"); return false; }
    int others = 0;
    for (uint64_t i = 0; i < scopeCount; ++i) {
        uint8_t* scope = scopes[i];
        if (!Readable(scope, 0x480) || !strncmp((const char*)scope + 8, "client.dll", 11)) continue;
        const int n = readScope(scope, true);
        if (n > 0) others += n;
    }
    Log("schema: %d more classes from other modules", others);
    return !g_schema.empty();
}

static int g_schemaMisses = 0;
static void Field(ptrdiff_t& out, const char* cls, const char* field) {
    auto c = g_schema.find(cls);
    if (c != g_schema.end()) {
        auto f = c->second.fields.find(field);
        if (f != c->second.fields.end()) {
            if ((ptrdiff_t)f->second != out) Log("offset %s::%s = 0x%x (built-in 0x%x)", cls, field, f->second, (unsigned)out);
            out = f->second;
            return;
        }
    }
    ++g_schemaMisses;
    Log("WARNING: %s::%s not in schema, using built-in 0x%x", cls, field, (unsigned)out);
}
bool SchemaField(const char* cls, const char* field, ptrdiff_t& out) {
    auto c = g_schema.find(cls);
    if (c == g_schema.end()) return false;
    auto f = c->second.fields.find(field);
    if (f == c->second.fields.end()) return false;
    out = f->second;
    return true;
}
static void Size(ptrdiff_t& out, const char* cls) {
    auto c = g_schema.find(cls);
    if (c != g_schema.end() && c->second.size) out = c->second.size;
}

void* attributeVtable = nullptr;
static void* g_memAlloc = nullptr;   // IMemAlloc*: [0] destructor, [1] Alloc(size), [2] Realloc, [3] Free

void* Alloc(size_t size) {
    if (!g_memAlloc) return nullptr;
    using Fn = void* (*)(void*, size_t);
    return ((Fn)VFunc(g_memAlloc, 1))(g_memAlloc, size);
}

// MSVC RTTI: the class name sits in a TypeDescriptor (name at +0x10); a CompleteObjectLocator (signature 1 on x64,
// offset 0) points to it by RVA and to itself; the vtable is the slot right after a pointer to that locator.
void* FindVtable(const char* module, const char* mangled) {
    auto base = (uint8_t*)GetModuleHandleA(module);
    if (!base) return nullptr;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *rdata = nullptr, *data = nullptr;
    size_t rdataSize = 0, dataSize = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!std::memcmp(sec[i].Name, ".rdata", 6)) { rdata = base + sec[i].VirtualAddress; rdataSize = sec[i].Misc.VirtualSize; }
        if (!std::memcmp(sec[i].Name, ".data", 6)) { data = base + sec[i].VirtualAddress; dataSize = sec[i].Misc.VirtualSize; }
    }
    if (!rdata || !data) return nullptr;
    const size_t len = std::strlen(mangled) + 1;
    uint8_t* name = nullptr;
    for (size_t i = 0; i + len <= dataSize; ++i)
        if (data[i] == '.' && !std::memcmp(data + i, mangled, len)) { name = data + i; break; }
    if (!name) return nullptr;
    const uint32_t tdRva = (uint32_t)(name - 0x10 - base);
    for (size_t i = 0; i + 24 <= rdataSize; i += 4) {
        uint32_t* col = (uint32_t*)(rdata + i);
        if (col[0] != 1 || col[1] != 0 || col[3] != tdRva || col[5] != (uint32_t)((uint8_t*)col - base)) continue;
        const uint64_t colAddr = (uint64_t)col;
        for (size_t j = 0; j + 16 <= rdataSize; j += 8)
            if (*(uint64_t*)(rdata + j) == colAddr) return rdata + j + 8;
    }
    return nullptr;
}

// ---------------------------------------------------------------- functions inside client.dll
struct Text { uint8_t* base = nullptr; uint8_t* start = nullptr; size_t size = 0; uint8_t* rdata = nullptr; size_t rdataSize = 0; };
static Text ClientText() {
    Text t;
    t.base = (uint8_t*)GetModuleHandleA("client.dll");
    if (!t.base) return t;
    auto nt = (IMAGE_NT_HEADERS64*)(t.base + ((IMAGE_DOS_HEADER*)t.base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!std::memcmp(sec[i].Name, ".text", 5)) { t.start = t.base + sec[i].VirtualAddress; t.size = sec[i].Misc.VirtualSize; }
        if (!std::memcmp(sec[i].Name, ".rdata", 6)) { t.rdata = t.base + sec[i].VirtualAddress; t.rdataSize = sec[i].Misc.VirtualSize; }
    }
    return t;
}

bool ClientCode(uint8_t*& start, size_t& size) {
    const Text t = ClientText();
    start = t.start;
    size = t.size;
    return start && size;
}

// "48 8B ?? 05" style pattern; returns the first match or nullptr (and logs if there is more than one).
static uint8_t* FindPattern(const Text& t, const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { bytes.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        bytes.push_back((int)std::strtol(p, (char**)&p, 16));
    }
    uint8_t* found = nullptr;
    int hits = 0;
    for (size_t i = 0; i + bytes.size() <= t.size; ++i) {
        size_t k = 0;
        while (k < bytes.size() && (bytes[k] < 0 || t.start[i + k] == bytes[k])) ++k;
        if (k == bytes.size()) { if (!found) found = t.start + i; ++hits; }
    }
    if (hits > 1) Log("pattern matched %d times, using the first: %s", hits, pattern);
    return found;
}

static uint8_t* FindString(const Text& t, const char* s) {
    const size_t len = std::strlen(s) + 1;
    for (size_t i = 0; i + len <= t.rdataSize; ++i)
        if (t.rdata[i] == (uint8_t)s[0] && !std::memcmp(t.rdata + i, s, len) && (i == 0 || t.rdata[i - 1] == 0)) return t.rdata + i;
    return nullptr;
}

// Every "lea r64, [rip+x]" whose target is `target`.
template <class F> static void ForEachLea(const Text& t, const uint8_t* target, F&& f) {
    for (size_t i = 0; i + 7 <= t.size; ++i) {
        uint8_t* ins = t.start + i;
        if ((ins[0] != 0x48 && ins[0] != 0x4C) || ins[1] != 0x8D || (ins[2] & 0xC7) != 0x05) continue;
        if (ins + 7 + *(int32_t*)(ins + 3) == target) f(ins);
    }
}
static uint8_t* LeaTarget(uint8_t* ins) { return ins + 7 + *(int32_t*)(ins + 3); }

using SetModelFn = void (*)(void*, const char*);
using SubclassChangedFn = void (*)(void*);
using CommandFn = void (*)(void*, void*);
static SetModelFn g_setModel = nullptr;
static SubclassChangedFn g_subclassChanged = nullptr;
static CommandFn g_regenerate = nullptr;

static void FindWeaponRebuild();
static void FindGloveGates(const struct Text& t);
static void FindGloveHolder(const struct Text& t);
static void FindAnimContextSettings(const struct Text& t);
static void FindPrecache(const struct Text& t);
static void FindMeshMaskHandler(const struct Text& t);
static void FindPostDataUpdateStage(const struct Text& t);
static void FindHudModel(const struct Text& t);
static void FindBodyGroups(const struct Text& t);
static void FindDemoVersion(const struct Text& t);
static void FindGlobals();
static void FindAttachments(const struct Text& t);

static void FindFunctions() {
    Text t = ClientText();
    if (!t.start || !t.rdata) { Log("ERROR: client.dll sections not found"); return; }

    // C_BaseModelEntity::SetModel(const char* path): resolves the model by name, then applies it.
    g_setModel = (SetModelFn)FindPattern(t, "40 53 48 83 EC 20 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 50 ?? 48 8B 54 24 ?? 48 8B CB E8");

    // The network callback for m_nSubclassID. It is registered next to its name: lea rax,"OnSubclassIDChanged"; mov ...; lea rax,fn
    if (uint8_t* name = FindString(t, "OnSubclassIDChanged")) {
        ForEachLea(t, name, [&](uint8_t* ins) {
            if (g_subclassChanged || ins[2] != 0x05) return;   // lea rax only (the registration table)
            for (uint8_t* p = ins + 7; p < ins + 0x20; ++p)
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { g_subclassChanged = (SubclassChangedFn)LeaTarget(p); break; }
        });
    }
    if (!g_subclassChanged)
        g_subclassChanged = (SubclassChangedFn)FindPattern(t, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB ?? ?? ?? ?? 00 74 ?? 48 8B 03 BA 01 00 00 00");
    // Its code checks the entity's VData pointer: cmp qword ptr [rbx+disp32], 0
    if (g_subclassChanged) {
        uint8_t* p = (uint8_t*)g_subclassChanged;
        bool ok = false;
        for (int i = 0; i < 0x20; ++i)
            if (p[i] == 0x48 && p[i + 1] == 0x83 && p[i + 2] == 0xBB && p[i + 7] == 0x00) { O.ent_VData = *(int32_t*)(p + i + 3); ok = true; break; }
        if (!ok) { Log("WARNING: subclass callback has unexpected code, knife model swap off"); g_subclassChanged = nullptr; }
    }

    // regenerate_weapon_skins: registered as lea rax,handler ... lea rdx,"regenerate_weapon_skins"
    if (uint8_t* name = FindString(t, "regenerate_weapon_skins")) {
        ForEachLea(t, name, [&](uint8_t* ins) {
            if (g_regenerate || ins[0] != 0x48 || ins[2] != 0x15) return;   // lea rdx
            for (uint8_t* p = ins - 7; p > ins - 0x60; --p)
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) {
                    uint8_t* fn = LeaTarget(p);
                    if (fn >= t.start && fn < t.start + t.size && fn[0] == 0x48 && fn[1] == 0x83 && fn[2] == 0xEC) g_regenerate = (CommandFn)fn;
                    break;
                }
        });
    }

    FindWeaponRebuild();
    FindGloveGates(t);
    FindGloveHolder(t);
    FindAnimContextSettings(t);
    FindPrecache(t);
    FindMeshMaskHandler(t);
    FindPostDataUpdateStage(t);
    FindHudModel(t);
    FindBodyGroups(t);
    FindDemoVersion(t);
    FindAttachments(t);
    Log("SetModel %s, subclass change %s (VData at 0x%x), regenerate_weapon_skins %s",
        g_setModel ? "found" : "NOT FOUND", g_subclassChanged ? "found" : "NOT FOUND", (unsigned)O.ent_VData,
        g_regenerate ? "found" : "NOT FOUND");
}

// Inside regenerate_weapon_skins, each weapon is rebuilt with:
//   if (w[a] > 0 || w[b] > 0) { Prepare(w + c, true); Rebuild(w, 0); }
// We do exactly that for one weapon, instead of rebuilding every weapon on the map (which stutters).
using PrepareFn = void (*)(void*, bool);
using RebuildFn = void (*)(void*, int);
static PrepareFn g_wPrepare = nullptr;
static RebuildFn g_wRebuild = nullptr;
static int32_t g_wCountA = 0, g_wCountB = 0, g_wPrepareOff = 0;

static void FindWeaponRebuild() {
    if (!g_regenerate) return;
    uint8_t* p = (uint8_t*)g_regenerate;
    for (int i = 0; i < 0x180; ++i) {
        uint8_t* q = p + i;
        if (q[0] == 0x83 && q[1] == 0xBB && q[6] == 0x00 && q[7] == 0x7F && q[9] == 0x83 && q[10] == 0xBB && q[15] == 0x00 && q[16] == 0x7E &&
            q[18] == 0x48 && q[19] == 0x8D && q[20] == 0x8B && q[25] == 0xB2 && q[26] == 0x01 && q[27] == 0xE8 && q[32] == 0x33 && q[33] == 0xD2 &&
            q[34] == 0x48 && q[35] == 0x8B && q[36] == 0xCB && q[37] == 0xE8) {
            g_wCountA = *(int32_t*)(q + 2);
            g_wCountB = *(int32_t*)(q + 11);
            g_wPrepareOff = *(int32_t*)(q + 21);
            g_wPrepare = (PrepareFn)(q + 32 + *(int32_t*)(q + 28));
            g_wRebuild = (RebuildFn)(q + 42 + *(int32_t*)(q + 38));
            Log("single-weapon rebuild found (0x%x/0x%x/0x%x)", g_wCountA, g_wCountB, g_wPrepareOff);
            return;
        }
    }
    Log("single-weapon rebuild not found: skins will rebuild all weapons at once");
}

void MaterialCounts(void* weapon, int& a, int& b) {
    a = g_wCountA ? At<int32_t>(weapon, g_wCountA) : -1;
    b = g_wCountB ? At<int32_t>(weapon, g_wCountB) : -1;
}

int RegenerateWeapon(void* weapon, bool force) {
    if (!g_wPrepare || !g_wRebuild) return -1;
    if (!force && At<int32_t>(weapon, g_wCountA) <= 0 && At<int32_t>(weapon, g_wCountB) <= 0) return 0;   // materials not made yet
    g_wPrepare((uint8_t*)weapon + g_wPrepareOff, true);
    g_wRebuild(weapon, 0);
    return 1;
}

// The three checks CS2 makes before it builds econ gloves (found next to the "gloves/paints/" text):
//   Skip(item) must be false, ModelPath(item) must give a model, PaintKit(item) must give the finish.
using GloveSkipFn = bool (*)(void*);
using GloveModelFn = const char* (*)(void*);
using GlovePaintFn = void* (*)(void*);
static GloveSkipFn g_gSkip = nullptr;
static GloveModelFn g_gModel = nullptr;
static GlovePaintFn g_gPaint = nullptr;
static uint8_t* g_gloveCode = nullptr;      // inside the function that builds a player's econ gloves
static int32_t g_gloveHolderOff = -1;       // where on the player that function's object lives (it starts with the glove entity's handle)

// The glove builder is called as  lea rcx, [pawn + X]; call Builder  - find X.
static void FindGloveHolder(const Text& t) {
    if (!g_gloveCode) return;
    for (size_t i = 7; i + 5 <= t.size; ++i) {
        uint8_t* c = t.start + i;
        if (c[0] != 0xE8) continue;
        uint8_t* target = c + 5 + *(int32_t*)(c + 1);
        if (target > g_gloveCode || g_gloveCode - target > 0x600) continue;
        uint8_t* lea = c - 7;
        if (lea[0] == 0x48 && lea[1] == 0x8D && (lea[2] & 0xF8) == 0x88 && (lea[2] & 7) != 4) {
            g_gloveHolderOff = *(int32_t*)(lea + 3);
            Log("glove builder object on the player at +0x%x", g_gloveHolderOff);
            return;
        }
    }
    Log("glove builder object not found");
}

bool CanSeeGloveEntity() { return g_gloveHolderOff >= 0; }

void* GloveEntity(void* pawn) {
    if (g_gloveHolderOff < 0) return nullptr;
    return FromHandle(At<uint32_t>(pawn, g_gloveHolderOff));
}

static void FindGloveGates(const Text& t) {
    uint8_t* str = FindString(t, "gloves/paints/");
    if (!str) return;
    ForEachLea(t, str, [&](uint8_t* ins) {
        if (g_gPaint || ins[0] != 0x4C) return;   // lea r8, "gloves/paints/"
        void* found[3] = {};
        int n = 0;
        for (uint8_t* p = ins - 5; p > ins - 0x60 && n < 3; --p) {
            if (p[0] == 0xE8 && p[-3] == 0x48 && p[-2] == 0x8B && (p[-1] & 0xF8) == 0xC8) {   // mov rcx, reg; call
                found[n++] = p + 5 + *(int32_t*)(p + 1);
            }
        }
        if (n == 3) { g_gPaint = (GlovePaintFn)found[0]; g_gModel = (GloveModelFn)found[1]; g_gSkip = (GloveSkipFn)found[2]; g_gloveCode = ins; }
    });
    Log(g_gPaint ? "glove checks found" : "glove checks not found");
}

bool GloveGates(void* item, bool& skip, const char*& model, void*& paint) {
    if (!g_gSkip) return false;
    skip = g_gSkip(item);
    model = g_gModel(item);
    paint = g_gPaint(item);
    return true;
}

// The animation code picks "networked" (demo-table) or local animation data from two settings,
// ag2_use_networked_serialization_context_demo / _game:
//   test al,al; mov rax,[demo]; jne +7; mov rax,[game]; movzx ecx, byte [rax+N]; test cl,cl; je local_path
// We find both settings there and switch their value byte to 0, which is what the console command would do.
static uint8_t** g_ctxDemo = nullptr;
static uint8_t** g_ctxGame = nullptr;
static int g_ctxValueOff = 0;

static void FindAnimContextSettings(const Text& t) {
    uint8_t* p = FindPattern(t, "84 C0 48 8B 05 ?? ?? ?? ?? 75 ?? 48 8B 05 ?? ?? ?? ?? 0F B6 48 ?? 84 C9 74");
    if (!p) { Log("animation-data switch not found: knives/gloves outside the demo stay off"); return; }
    g_ctxDemo = (uint8_t**)(p + 9 + *(int32_t*)(p + 5));
    g_ctxGame = (uint8_t**)(p + 18 + *(int32_t*)(p + 14));
    g_ctxValueOff = p[21];
    Log("animation-data switch found (value at +0x%x)", g_ctxValueOff);
}

static uint8_t g_ctxOrig[2] = {};
static bool g_ctxSaved = false, g_ctxSwitched = false;

bool UseLocalAnimData() {
    if (!g_ctxDemo || !g_ctxGame) return false;
    bool ok = true;
    int i = 0;
    for (uint8_t** g : {g_ctxDemo, g_ctxGame}) {
        uint8_t* cv = *g;
        if (!Readable(cv + g_ctxValueOff, 1)) { ok = false; ++i; continue; }
        if (!g_ctxSaved) g_ctxOrig[i] = cv[g_ctxValueOff];
        if (cv[g_ctxValueOff] != 0) {
            Log("animation data: switching setting at %p from %d to 0 (local)", (void*)cv, cv[g_ctxValueOff]);
            cv[g_ctxValueOff] = 0;
        }
        ++i;
    }
    if (ok) { g_ctxSaved = true; g_ctxSwitched = true; }
    return ok;
}

bool AnimDataSwitched() { return g_ctxSwitched; }

void RestoreAnimData() {
    if (!g_ctxSwitched || !g_ctxSaved) return;
    int i = 0;
    for (uint8_t** g : {g_ctxDemo, g_ctxGame}) {
        uint8_t* cv = *g;
        if (Readable(cv + g_ctxValueOff, 1) && cv[g_ctxValueOff] != g_ctxOrig[i]) cv[g_ctxValueOff] = g_ctxOrig[i];
        ++i;
    }
    g_ctxSwitched = false;
    Log("animation data: setting put back to the demo's own data");
}

// ---- demo version
// CS2's TrueView check:  demo = engine->GetDemoFile(); demo->IsOpen(); ...
//   hdr = demo->[a](); ver = hdr->[b]();  client = engine->[c]();  "Demo is version %d, client is version %d."
// The three function numbers are read from that code.
static int g_dOpen = -1, g_dHdr = -1, g_dVer = -1, g_cVer = -1;
static bool g_demoFileFromCode = false;

static void FindDemoVersion(const Text& t) {
    uint8_t* str = FindString(t, "Demo is version %d, client is version %d.  TrueView is %s as per %s=%d\n");
    uint8_t* lea = nullptr;
    if (str) ForEachLea(t, str, [&](uint8_t* ins) { if (!lea) lea = ins; });
    if (lea) {
        uint8_t* hdrCall = nullptr;
        for (uint8_t* q = lea - 0x200; q < lea && !hdrCall; ++q) {
            // call [rax+a]; test rax,rax; ... mov rdx,[rax]; mov rcx,rax; call [rdx+b]
            if (q[0] != 0xFF || q[1] != 0x50 || q[3] != 0x48 || q[4] != 0x85 || q[5] != 0xC0) continue;
            for (uint8_t* w = q + 6; w < q + 0x1E; ++w)
                if (w[0] == 0xFF && w[1] == 0x52) { g_dHdr = q[2] / 8; g_dVer = w[2] / 8; hdrCall = q; break; }
        }
        if (hdrCall) {
            uint8_t* openCall = nullptr;
            for (uint8_t* q = hdrCall; q > hdrCall - 0x100; --q)
                if (q[0] == 0xFF && q[1] == 0x52 && q[3] == 0x84 && q[4] == 0xC0) { g_dOpen = q[2] / 8; openCall = q; break; }   // call [rdx+x]; test al,al
            // Just before: demo = engine->GetDemoFile() - "call [rax+disp32]" (or disp8). Its number comes from here
            // rather than a fixed one, so a CS2 update that moves it doesn't send us into another function.
            if (openCall) {
                for (uint8_t* q = openCall - 6; q > openCall - 0x40; --q) {
                    int idx = -1;
                    if (q[0] == 0xFF && q[1] == 0x90) idx = *(int32_t*)(q + 2) / 8;
                    else if (q[0] == 0xFF && q[1] == 0x50) idx = q[2] / 8;
                    if (idx < 20 || idx > 200) continue;
                    if (idx != I.engineGetDemoFile) Log("engine GetDemoFile is function %d in this CS2 build (built-in %d) - using %d", idx, I.engineGetDemoFile, idx);
                    I.engineGetDemoFile = idx;
                    g_demoFileFromCode = true;
                    break;
                }
            }
            for (uint8_t* q = hdrCall; q < hdrCall + 0x80; ++q)
                if (q[0] == 0xFF && q[1] == 0x92) { g_cVer = *(int32_t*)(q + 2) / 8; break; }                         // call [rdx+disp32]
        }
    }
    if (g_dOpen > 0 && g_dHdr > 0 && g_dVer > 0 && g_cVer > 0)
        Log("demo version check found (%d/%d/%d/%d)", g_dOpen, g_dHdr, g_dVer, g_cVer);
    else {
        Log("demo version check NOT found: knives/gloves nobody in the demo has are only allowed if you tick 'Any model in old demos'");
        g_dOpen = g_dHdr = g_dVer = g_cVer = -1;
    }
}

bool DemoFileVerified() { return g_demoFileFromCode; }
bool CanCheckDemoVersion() { return g_cVer > 0; }
int InterfaceFallbacks() { return g_interfaceFallbacks; }
int SchemaMisses() { return g_schemaMisses; }
int ClientVersion() {
    if (g_cVer <= 0 || !engine) return 0;
    using IntFn = int (*)(void*);
    return ((IntFn)VFunc(engine, g_cVer))(engine);
}

int DemoIsCurrent(int* demoVersion, int* gameVersion) {
    if (g_cVer < 0 || !engine) return -1;
    using GetDemoFileFn = void* (*)(void*);
    using BoolFn = bool (*)(void*);
    using PtrFn = void* (*)(void*);
    using IntFn = int (*)(void*);
    void* demo = ((GetDemoFileFn)VFunc(engine, I.engineGetDemoFile))(engine);
    if (!Readable(demo) || !((BoolFn)VFunc(demo, g_dOpen))(demo)) return -1;
    void* hdr = ((PtrFn)VFunc(demo, g_dHdr))(demo);
    if (!Readable(hdr)) return -1;
    int dv = ((IntFn)VFunc(hdr, g_dVer))(hdr);
    int gv = ((IntFn)VFunc(engine, g_cVer))(engine);
    if (demoVersion) *demoVersion = dv;
    if (gameVersion) *gameVersion = gv;
    if (dv <= 0 || gv <= 0) return -1;
    return dv == gv ? 1 : 0;
}

// Loading a model the demo never loaded: the game does it for its map preview gloves with
//   CResourceName name; name.Set(path); if (name.type == 'vmdl') resourceSystem->Precache(&name, "MapPlayerPreview gloves");
// We find those pieces next to that text and do the same with our own reason string.
using NameSetFn = void (*)(void*, const char*);
using PrecacheFn = void (*)(void*, void*, const char*);
using PurgeFn = void (*)(void*, int);
static NameSetFn g_nameSet = nullptr;
static void** g_resourceSystem = nullptr;
static int g_precacheOff = 0;
static PurgeFn g_bufPurge = nullptr;

static void FindPrecache(const Text& t) {
    uint8_t* str = FindString(t, "MapPlayerPreview gloves");
    if (!str) return;
    ForEachLea(t, str, [&](uint8_t* ins) {
        if (g_nameSet || ins[0] != 0x4C) return;   // lea r8, reason
        uint8_t *setCall = nullptr, *rsLoad = nullptr;
        for (uint8_t* p = ins - 0x60; p < ins; ++p) {
            if (!setCall && p[0] == 0xC7 && p[1] == 0x45 && *(uint32_t*)(p + 3) == 0xC00000C8) {
                for (uint8_t* q = p + 7; q < ins; ++q) if (q[0] == 0xE8) { setCall = q; break; }
            }
            if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x1D) rsLoad = p;   // mov rbx, [rip+x]
        }
        uint8_t* vcall = nullptr;
        for (uint8_t* p = ins + 7; p < ins + 0x18; ++p) if (p[0] == 0xFF && p[1] == 0x90) { vcall = p; break; }
        if (!setCall || !rsLoad || !vcall) return;
        g_nameSet = (NameSetFn)(setCall + 5 + *(int32_t*)(setCall + 1));
        g_resourceSystem = (void**)(rsLoad + 7 + *(int32_t*)(rsLoad + 3));
        g_precacheOff = *(int32_t*)(vcall + 2);
    });
    if (HMODULE tier0 = GetModuleHandleA("tier0.dll")) g_bufPurge = (PurgeFn)GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z");
    Log(g_nameSet && g_bufPurge ? "model loading found" : "model loading not found");
}

bool PrecacheModel(const char* path) { return PrecacheResource(path, 0x6C646D76); }   // 'vmdl'

bool PrecacheResource(const char* path, uint32_t type) {
    if (!g_nameSet || !g_bufPurge || !g_resourceSystem || !*g_resourceSystem || !path || !*path) return false;
    alignas(16) uint8_t name[0x100] = {};
    *(uint32_t*)(name + 4) = 0xC00000C8;   // inline storage of 200 characters
    g_nameSet(name, path);
    bool ok = *(uint64_t*)(name + 0xD8) == type;   // resource type, e.g. 'vmdl', 'vpcf'
    if (ok) {
        void* rs = *g_resourceSystem;
        ((PrecacheFn)(*(void***)rs)[g_precacheOff / 8])(rs, name, "DemoTools2");
    }
    g_bufPurge(name, 0);
    return ok;
}

// Legacy-model finishes need the weapon's legacy mesh. The mesh mask is a networked CModelState field; its change
// handler is registered next to the text "skeletonMeshGroupMaskChanged" (lea rax, handler ... lea rdx, name) and
// takes (CModelState*, entity, const uint64_t* newMask).
using MaskChangedFn = void (*)(void*, void*, const uint64_t*);
static MaskChangedFn g_maskChanged = nullptr;

static void FindMeshMaskHandler(const Text& t) {
    uint8_t* name = FindString(t, "skeletonMeshGroupMaskChanged");
    if (name) {
        ForEachLea(t, name, [&](uint8_t* ins) {
            if (g_maskChanged || ins[2] != 0x15) return;   // lea rdx, name
            for (uint8_t* p = ins - 7; p > ins - 0x50; --p)
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { g_maskChanged = (MaskChangedFn)LeaTarget(p); break; }
        });
    }
    Log(g_maskChanged ? "legacy-model switch found" : "legacy-model switch NOT found: legacy finishes may look scrambled");
}

static uint8_t* ModelStateOf(void* entity) {
    uint8_t* node = Get<uint8_t*>(entity, O.ent_pGameSceneNode);
    return !LooksLikePtr(node) ? nullptr : node + O.skeleton_modelState;
}

uint64_t MeshGroupMask(void* entity) {
    uint8_t* ms = ModelStateOf(entity);
    return ms ? Get<uint64_t>(ms, O.modelState_MeshGroupMask) : 0;
}

int SetNetworkedBodyGroup(void* entity, const char* group, int choice) {
    uint8_t* ms = ModelStateOf(entity);
    if (!ms || choice < 0) return -1;
    int g = BodyGroup(entity, group);
    if (g < 0 || g > 64) return -1;
    int count = Get<int32_t>(ms, O.modelState_BodyGroupChoices);
    int32_t* choices = Get<int32_t*>(ms, O.modelState_BodyGroupChoices + 8);
    if (count <= g || count > 256 || !Readable(choices + g, 4)) return -1;
    int prev = choices[g];
    if (prev != choice) choices[g] = choice;
    return prev;
}

bool SetMeshGroupMask(void* entity, uint64_t mask) {
    uint8_t* ms = ModelStateOf(entity);
    if (!ms || !g_maskChanged) return false;
    g_maskChanged(ms, entity, &mask);
    return true;
}

// FrameStageNotify(stage) dispatches with  add ebx,-N; cmp ebx,M; ja; lea rdx,[image]; movsxd rax,ebx;
// mov ecx,[rdx+rax*4+TABLE]. The stage whose case logs "FramePostDataUpdate" is the one where CS2 runs every
// changed entity's PostDataUpdate (builds skins, gloves...) - right after the network data was unpacked.
// Writing our values just before that stage is exactly like the demo itself containing them.
static int g_pduStage = -1;
static void FindPostDataUpdateStage(const Text& t) {
    uint8_t* str = FindString(t, "FramePostDataUpdate(%.3f %d)");
    if (!str) { Log("post-data-update stage: text not found"); return; }
    uint8_t* block = nullptr;
    ForEachLea(t, str, [&](uint8_t* ins) { if (!block) block = ins - 7; });   // the case starts with a 7-byte mov before it
    if (!block) return;
    for (uint8_t* p = block; p > block - 0x800; --p) {
        if (!(p[0] == 0x8B && p[1] == 0x8C && p[2] == 0x82)) continue;   // mov ecx, [rdx+rax*4+table]
        uint32_t table = *(uint32_t*)(p + 3);
        int base = 0;
        for (uint8_t* q = p; q > p - 0x40; --q)
            if (q[0] == 0x83 && (q[1] & 0xF8) == 0xC0 && (int8_t)q[2] < 0) { base = -(int8_t)q[2]; break; }   // add r32, -N
        if (!base) continue;
        for (int i = 0; i < 24; ++i) {
            uint32_t entry = *(uint32_t*)(t.base + table + 4 * i);
            uint8_t* target = t.base + entry;
            if (target <= block && block - target < 0x20) { g_pduStage = i + base; break; }
        }
        if (g_pduStage >= 0) break;
    }
    Log(g_pduStage >= 0 ? "post-data-update stage is %d" : "post-data-update stage not found (%d)", g_pduStage);
}
int PostDataUpdateStage() { return g_pduStage; }

// A direct call (E8 rel32) to a function start in .text. Function starts are 16-byte aligned in client.dll, and the
// byte before must not make the E8 part of another instruction (mov ebp,eax = 8B E8, shr rax,9 = 48 C1 E8 09, ...).
static uint8_t* CallTarget(const Text& t, uint8_t* q) {
    if (q[0] != 0xE8) return nullptr;
    switch (q[-1]) {
        case 0x8B: case 0x89: case 0xC1: case 0xD1: case 0xD3: case 0x83: case 0x81: case 0x2B: case 0x03: case 0x33:
        case 0x85: case 0x3B: case 0x0F: case 0xF7: return nullptr;
    }
    uint8_t* target = q + 5 + *(int32_t*)(q + 1);
    if (target < t.start || target >= t.start + t.size || ((uintptr_t)target & 15)) return nullptr;
    return target;
}
// The first n direct calls in [fn, fn+len).
static int CallsIn(const Text& t, uint8_t* fn, int len, uint8_t** out, int n) {
    int k = 0;
    for (uint8_t* q = fn; q < fn + len && k < n; ++q)
        if (uint8_t* c = CallTarget(t, q)) out[k++] = c;
    return k;
}

// ---- first-person weapon models
// The weapon's data-changed handler logs "Re-creating weapon hudmodel ..." and then does
//   mov ecx, [weapon + HUD]   (the hud model's handle)  ...  call UTIL_Remove(hudModel)
using RemoveFn = void (*)(void*);
static RemoveFn g_remove = nullptr;
static int32_t g_hudOff = 0;

static void FindHudModel(const Text& t) {
    uint8_t* str = FindString(t, "Re-creating weapon hudmodel due to vdata subclass model change.\n");
    if (str) {
        ForEachLea(t, str, [&](uint8_t* ins) {
            if (g_remove) return;
            uint8_t* p = ins + 7;
            int32_t off = 0;
            while (p < ins + 0x20) {
                if (p[0] == 0xFF && p[1] == 0x15) { p += 6; continue; }   // call [rip+x] (the log function)
                if (p[0] == 0x8B && (p[1] & 0xC0) == 0x80 && ((p[1] >> 3) & 7) == 1 && (p[1] & 7) != 4) { off = *(int32_t*)(p + 2); p += 6; break; }
                ++p;
            }
            if (off < 0x100 || off > 0x10000) return;
            for (uint8_t* q = p; q < p + 0x80; ++q) {
                uint8_t* fn = CallTarget(t, q);
                if (!fn) continue;
                if (fn[0] == 0x48 && fn[1] == 0x83 && fn[2] == 0xEC && fn[4] == 0x48 && fn[5] == 0x85 && fn[6] == 0xC9) {   // sub rsp,x; test rcx,rcx
                    g_remove = (RemoveFn)fn;
                    g_hudOff = off;
                }
                break;
            }
        });
    }
    if (g_remove) Log("first-person models found (handle on the weapon at +0x%x)", g_hudOff);
    else Log("first-person models NOT found: in first person, skins may only show after the weapon is drawn again");
}

bool CanFixHudModels() { return g_remove && g_hudOff; }
void RemoveEntity(void* entity) { if (g_remove && entity) g_remove(entity); }

void* HudModel(void* weapon) {
    if (!g_hudOff) return nullptr;
    void* e = FromHandleExact(At<uint32_t>(weapon, g_hudOff));
    if (!e || !ModelName(e)) return nullptr;
    static bool logged = false;
    if (!logged) { logged = true; const char* dn = DesignerName(e); Log("first-person model entity: '%s'", dn ? dn : "?"); }
    return e;
}

// ---- body groups
// The glove builder asks  ShowsDefaultGloves(pawn)  before the re-apply flag check:
//   call G; test al,al; je; cmp byte [pawn + NeedToReApplyGloves], ...
// G calls GroupOfEntity(pawn, token) and ChoiceIs(pawn, token, &name) twice ("thirdperson_default",
// "firstperson_default"). ChoiceIs calls GetModel(ent) twice, GroupIndex(model, token), ChoiceIndex(model, group, &name).
// SetBodygroup(ent, group, choice) is the function that logs "CBaseModelEntity::SetBodygroup(%d,%d) failed ...".
struct NameTok { uint32_t token; uint32_t pad; const char* name; };
using GroupOfEntityFn = int (*)(void*, uint32_t);
using ChoiceIsFn = bool (*)(void*, uint32_t, const NameTok*);
using GetModelFn = void* (*)(void*);
using ChoiceIndexFn = int (*)(void*, int, const NameTok*);
using SetBodygroupFn = void (*)(void*, int, int);
static GroupOfEntityFn g_bgGroup = nullptr;
static ChoiceIsFn g_bgIs = nullptr;
static GetModelFn g_bgModel = nullptr;
static ChoiceIndexFn g_bgChoice = nullptr;
static SetBodygroupFn g_bgSet = nullptr;

static void FindBodyGroups(const Text& t) {
    uint8_t* gate = nullptr;
    if (g_gloveCode && O.pawn_NeedToReApplyGloves) {
        for (uint8_t* p = g_gloveCode - 0x400; p < g_gloveCode && !gate; ++p) {
            if (p[0] != 0xE8 || p[5] != 0x84 || p[6] != 0xC0 || p[7] != 0x74) continue;
            for (int k = 9; k <= 12; ++k)
                if (*(int32_t*)(p + k) == (int32_t)O.pawn_NeedToReApplyGloves) { gate = CallTarget(t, p); break; }
        }
    }
    if (gate) {
        uint8_t* c[3] = {};
        if (CallsIn(t, gate, 0xC0, c, 3) == 3 && c[1] == c[2]) {
            uint8_t* d[4] = {};
            if (CallsIn(t, c[1], 0x90, d, 4) == 4 && d[0] == d[1]) {
                g_bgGroup = (GroupOfEntityFn)c[0];
                g_bgIs = (ChoiceIsFn)c[1];
                g_bgModel = (GetModelFn)d[0];
                g_bgChoice = (ChoiceIndexFn)d[3];
            }
        }
    }
    if (uint8_t* str = FindString(t, "CBaseModelEntity::SetBodygroup(%d,%d) failed: CBaseModelEntity has no model!\n")) {
        ForEachLea(t, str, [&](uint8_t* ins) {
            if (g_bgSet) return;
            for (uint8_t* p = ins; p > ins - 0x60; --p) {
                if (!(p[0] == 0x85 && p[1] == 0xD2 && p[2] == 0x0F && p[3] == 0x88)) continue;   // test edx,edx; js
                uint8_t* first = nullptr;
                CallsIn(t, p, 0x40, &first, 1);
                if (first && (void*)first == (void*)g_bgModel) g_bgSet = (SetBodygroupFn)p;
                break;
            }
        });
    }
    Log("body groups: %s%s", CanSetBodyGroups() ? "found" : "NOT found",
        CanSetBodyGroups() ? "" : " - gloves can't be shown on players who had default gloves; legacy finishes use the mesh mask only");
}

bool CanSetBodyGroups() { return g_bgGroup && g_bgIs && g_bgModel && g_bgChoice && g_bgSet; }

int BodyGroup(void* entity, const char* group) {
    if (!g_bgGroup || !entity) return -1;
    return g_bgGroup(entity, StringToken(group));
}

bool BodyGroupIs(void* entity, const char* group, const char* choice) {
    if (!g_bgIs || !entity) return false;
    NameTok n{StringToken(choice), 0, choice};
    return g_bgIs(entity, StringToken(group), &n);
}

bool SetBodyGroup(void* entity, const char* group, const char* choice) {
    if (!CanSetBodyGroups() || !entity) return false;
    int g = BodyGroup(entity, group);
    if (g < 0) return false;
    void* model = g_bgModel(entity);
    if (!model) return false;
    NameTok n{StringToken(choice), 0, choice};
    int c = g_bgChoice(model, g, &n);
    if (c < 0) return false;
    g_bgSet(entity, g, c);
    return true;
}

bool SetBodyGroupIndex(void* entity, const char* group, int choice) {
    if (!CanSetBodyGroups() || !entity || choice < 0) return false;
    int g = BodyGroup(entity, group);
    if (g < 0) return false;
    g_bgSet(entity, g, choice);
    return true;
}

bool CanSwapModels() { return g_setModel && g_subclassChanged; }
bool CanRegenerate() { return g_regenerate != nullptr; }
void SetModel(void* entity, const char* path) { if (g_setModel) g_setModel(entity, path); }
bool CanSetModel() { return g_setModel != nullptr; }
void ChangeSubclass(void* entity, uint32_t token) {
    if (!g_subclassChanged) return;
    At<uint32_t>(entity, O.ent_nSubclassID) = token;
    g_subclassChanged(entity);
}
void RegenerateWeaponSkins() { if (g_regenerate) g_regenerate(nullptr, nullptr); }

// Called for every weapon every frame: plain null checks only (these are the game's own live pointers).
// Whether the entity's model is really loaded: CModelState::m_hModel is a resource handle whose first pointer is the
// loaded model (null while it isn't, e.g. a model the resource system couldn't find).
bool ModelLoaded(void* entity) {
    static ptrdiff_t oModel = -1;
    if (oModel < 0 && !SchemaField("CModelState", "m_hModel", oModel)) oModel = 0;
    if (oModel <= 0 || !entity) return false;
    uint8_t* node = Get<uint8_t*>(entity, O.ent_pGameSceneNode);
    if (!LooksLikePtr(node)) return false;
    void* h = Get<void*>(node, O.skeleton_modelState + oModel);
    return h && Readable(h, 8) && *(void**)h != nullptr;
}

void* ModelData(void* entity) {
    static ptrdiff_t oModel = -1;
    if (oModel < 0 && !SchemaField("CModelState", "m_hModel", oModel)) oModel = 0;
    if (oModel <= 0 || !entity) return nullptr;
    uint8_t* node = Get<uint8_t*>(entity, O.ent_pGameSceneNode);
    if (!LooksLikePtr(node)) return nullptr;
    void* h = Get<void*>(node, O.skeleton_modelState + oModel);
    if (!h || !Readable(h, 8)) return nullptr;
    void* d = *(void**)h;
    return d && Readable(d, 0x358) ? d : nullptr;
}

const char* ModelName(void* entity) {
    if (!LooksLikePtr(entity)) return nullptr;
    uint8_t* node = Get<uint8_t*>(entity, O.ent_pGameSceneNode);
    if (!LooksLikePtr(node)) return nullptr;
    const char* name = Get<const char*>(node, O.skeleton_modelState + O.modelState_ModelName);
    return !LooksLikePtr(name) ? nullptr : name;
}

uint32_t StringToken(const char* s) {
    const uint32_t m = 0x5bd1e995;
    uint32_t len = (uint32_t)std::strlen(s);
    uint32_t h = 0x31415926 ^ len;
    auto lower = [](uint8_t c) -> uint32_t { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
    const uint8_t* d = (const uint8_t*)s;
    while (len >= 4) {
        uint32_t k = lower(d[0]) | lower(d[1]) << 8 | lower(d[2]) << 16 | lower(d[3]) << 24;
        k *= m; k ^= k >> 24; k *= m;
        h *= m; h ^= k;
        d += 4; len -= 4;
    }
    switch (len) {
        case 3: h ^= lower(d[2]) << 16; [[fallthrough]];
        case 2: h ^= lower(d[1]) << 8; [[fallthrough]];
        case 1: h ^= lower(d[0]); h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    return h;
}

bool Init() {
    client = Interface("client.dll", "Source2Client002");
    engine = Interface("engine2.dll", "Source2EngineToClient001");
    resourceService = Interface("engine2.dll", "GameResourceServiceClientV001");
    if (!client || !engine || !resourceService) return false;

    if (ReadSchema()) {
        Field(O.identity_designerName, "CEntityIdentity", "m_designerName");
        Size(O.identitySize, "CEntityIdentity");
        Field(O.ent_pEntity, "CEntityInstance", "m_pEntity");
        Field(O.ent_iTeamNum, "C_BaseEntity", "m_iTeamNum");
        Field(O.ent_hOwnerEntity, "C_BaseEntity", "m_hOwnerEntity");
        Field(O.ctrl_steamID, "CBasePlayerController", "m_steamID");
        Field(O.ctrl_iszPlayerName, "CBasePlayerController", "m_iszPlayerName");
        Field(O.pawn_hController, "C_BasePlayerPawn", "m_hController");
        Field(O.econ_AttributeManager, "C_EconEntity", "m_AttributeManager");
        Field(O.econ_FallbackPaintKit, "C_EconEntity", "m_nFallbackPaintKit");
        Field(O.econ_FallbackSeed, "C_EconEntity", "m_nFallbackSeed");
        Field(O.econ_FallbackWear, "C_EconEntity", "m_flFallbackWear");
        Field(O.econ_FallbackStatTrak, "C_EconEntity", "m_nFallbackStatTrak");
        Field(O.econ_OrigXuidLow, "C_EconEntity", "m_OriginalOwnerXuidLow");
        Field(O.econ_OrigXuidHigh, "C_EconEntity", "m_OriginalOwnerXuidHigh");
        Field(O.container_Item, "C_AttributeContainer", "m_Item");
        Field(O.item_DefIndex, "C_EconItemView", "m_iItemDefinitionIndex");
        Field(O.item_EntityQuality, "C_EconItemView", "m_iEntityQuality");
        Field(O.item_ItemIDHigh, "C_EconItemView", "m_iItemIDHigh");
        Field(O.item_ItemIDLow, "C_EconItemView", "m_iItemIDLow");
        Field(O.item_AccountID, "C_EconItemView", "m_iAccountID");
        Field(O.item_CustomName, "C_EconItemView", "m_szCustomName");
        Field(O.item_AttributeList, "C_EconItemView", "m_AttributeList");
        Field(O.item_NetworkedDynamicAttributes, "C_EconItemView", "m_NetworkedDynamicAttributes");
        Field(O.attrlist_Attributes, "CAttributeList", "m_Attributes");
        Field(O.attr_DefIndex, "CEconItemAttribute", "m_iAttributeDefinitionIndex");
        Field(O.attr_Value, "CEconItemAttribute", "m_flValue");
        Field(O.attr_InitialValue, "CEconItemAttribute", "m_flInitialValue");
        Size(O.attrSize, "CEconItemAttribute");
        Field(O.weapon_CustomEconReloadEventId, "C_CSWeaponBase", "m_nCustomEconReloadEventId");
        Field(O.item_Initialized, "C_EconItemView", "m_bInitialized");
        Field(O.ctrl_hPlayerPawn, "CCSPlayerController", "m_hPlayerPawn");
        Field(O.pawn_EconGloves, "C_CSPlayerPawn", "m_EconGloves");
        Field(O.pawn_NeedToReApplyGloves, "C_CSPlayerPawn", "m_bNeedToReApplyGloves");
        Field(O.pawn_EconGlovesChanged, "C_CSPlayerPawn", "m_nEconGlovesChanged");
        Field(O.ent_nSubclassID, "C_BaseEntity", "m_nSubclassID");
        Field(O.ent_pGameSceneNode, "C_BaseEntity", "m_pGameSceneNode");
        Field(O.skeleton_modelState, "CSkeletonInstance", "m_modelState");
        Field(O.modelState_ModelName, "CModelState", "m_ModelName");
        Field(O.modelState_MeshGroupMask, "CModelState", "m_MeshGroupMask");
        Field(O.item_ItemID, "C_EconItemView", "m_iItemID");
        Field(O.modelState_BodyGroupChoices, "CModelState", "m_nBodyGroupChoices");
        Log("schema: %d field(s) missing", g_schemaMisses);
        // kept: the camera reads more fields after this
    } else {
        Log("WARNING: could not read the game's schema, using built-in offsets (may be wrong after a CS2 update).");
    }
    Log("CEntityIdentity size 0x%x, CEconItemAttribute size 0x%x", (unsigned)O.identitySize, (unsigned)O.attrSize);

    attributeVtable = FindVtable("client.dll", ".?AVCEconItemAttribute@@");
    Log(attributeVtable ? "CEconItemAttribute table found" : "WARNING: CEconItemAttribute table not found - StatTrak/gloves can only edit existing values");
    if (HMODULE tier0 = GetModuleHandleA("tier0.dll")) {
        void** pp = (void**)GetProcAddress(tier0, "g_pMemAlloc");
        if (pp && Readable(pp)) g_memAlloc = *pp;
    }
    Log(g_memAlloc ? "engine allocator found" : "WARNING: g_pMemAlloc not exported - lists can only be extended within their spare room");
    FindFunctions();
    FindGlobals();
    return true;
}


// ---------------------------------------------------------------- camera support
void* SceneNode(void* entity) {
    if (!entity) return nullptr;
    uint8_t* node = Get<uint8_t*>(entity, O.ent_pGameSceneNode);
    return Readable(node, 0x100) ? node : nullptr;
}

// Source2Client::SetGlobals (vtable 11) stores the CGlobalVarsBase pointer: "mov [rip+x], rdx". HLAE reads the same
// fields: +0x04 frame count, +0x08 frame time, +0x0c absolute frame time, +0x30 current time.
static uint8_t** g_globalsSlot = nullptr;
static void FindGlobals() {
    uint8_t* fn = (uint8_t*)(*(void***)client)[11];
    if (!Readable(fn, 0x40)) return;
    for (int i = 0; i < 0x38; ++i)
        if (fn[i] == 0x48 && fn[i + 1] == 0x89 && fn[i + 2] == 0x15) { g_globalsSlot = (uint8_t**)(fn + i + 7 + *(int32_t*)(fn + i + 3)); break; }
    Log(g_globalsSlot ? "game time found" : "game time NOT found - camera smoothing uses real time");
}
static uint8_t* GlobalsPtr() {
    if (!g_globalsSlot || !Readable(g_globalsSlot)) return nullptr;
    uint8_t* g = *g_globalsSlot;
    return Readable(g, 0x40) ? g : nullptr;
}
float CurTime() { uint8_t* g = GlobalsPtr(); return g ? Get<float>(g, 0x30) : 0.f; }
float FrameTime() { uint8_t* g = GlobalsPtr(); return g ? Get<float>(g, 0x08) : 0.f; }

// LookupAttachment(entity, uint8& index, name) and GetAttachment(entity, index, out[8]: x y z - qx qy qz qw), found
// the way HLAE finds them (a call pair in the breakable-model code: "E8 .. 0F B6 95 .. 84 D2 0F 84 .. 4C 8D 45 B0
// 48 8B CE E8 ..").
using LookupAttachmentFn = void (*)(void*, uint8_t*, const char*);
using GetAttachmentFn = bool (*)(void*, uint8_t, float*);
static LookupAttachmentFn g_lookupAtt = nullptr;
static GetAttachmentFn g_getAtt = nullptr;
static void FindAttachments(const Text& t) {
    if (uint8_t* p = FindPattern(t, "E8 ?? ?? ?? ?? 0F B6 95 ?? ?? ?? ?? 84 D2 0F 84 ?? ?? ?? ?? 4C 8D 45 B0 48 8B CE E8 ?? ?? ?? ??")) {
        g_lookupAtt = (LookupAttachmentFn)(p + 5 + *(int32_t*)(p + 1));
        g_getAtt = (GetAttachmentFn)(p + 32 + *(int32_t*)(p + 28));
    }
    Log(g_getAtt ? "camera: attachments found" : "camera: attachments NOT found - 'attachment' calcs won't have a value");
}

bool Attachment(void* entity, const char* name, float pos[3], float quat[4]) {
    if (!g_lookupAtt || !g_getAtt || !entity || !name) return false;
    uint8_t idx = 0;
    g_lookupAtt(entity, &idx, name);
    if (!idx) return false;
    alignas(16) float out[8] = {};
    if (!g_getAtt(entity, idx, out)) return false;
    pos[0] = out[0]; pos[1] = out[1]; pos[2] = out[2];
    quat[0] = out[4]; quat[1] = out[5]; quat[2] = out[6]; quat[3] = out[7];
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

// Bones: CSkeletonInstance::m_modelState + 0x80 points to the bone transforms in world space, 0x20 bytes each
// (x y z scale, qx qy qz qw) - the layout every CS2 tool that draws skeletons uses.
bool Bone(void* entity, int index, float pos[3], float quat[4]) {
    if (index < 0 || index > 255) return false;
    uint8_t* node = (uint8_t*)SceneNode(entity);
    if (!node) return false;
    uint8_t* bones = Get<uint8_t*>(node, O.skeleton_modelState + 0x80);
    if (!Readable(bones + (size_t)index * 0x20, 0x20)) return false;
    float* b = (float*)(bones + (size_t)index * 0x20);
    for (int i = 0; i < 3; ++i) pos[i] = b[i];
    for (int i = 0; i < 4; ++i) quat[i] = b[4 + i];
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) && (pos[0] != 0 || pos[1] != 0 || pos[2] != 0);
}

void* CvarSystem() {
    static void* cvar = Interface("tier0.dll", "VEngineCvar007");
    return cvar;
}

ptrdiff_t viewOriginOff = 0, viewAnglesOff = 0, viewFovOff = 0;
using GetFloatFn = float (*)(void*);
using CalcDemoViewFn = void (*)(float*, float*);
static GetFloatFn g_getFloat = nullptr;
static void* g_overrideCvarRef = nullptr;
static CalcDemoViewFn g_calcDemoView = nullptr;

float OriginalDemoViewOverrideValue() { return g_getFloat && g_overrideCvarRef ? g_getFloat(g_overrideCvarRef) : 0.f; }
void OriginalCalcDemoViewOverride(void* setup) {
    if (g_calcDemoView && setup) g_calcDemoView((float*)((uint8_t*)setup + viewOriginOff), (float*)((uint8_t*)setup + viewAnglesOff));
}

static bool Patch(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// ---- the camera position (campath map, quick moves)
static ViewValueFn g_userValue = nullptr;
static ViewApplyFn g_userApply = nullptr;
static void* volatile g_viewSetup = nullptr;
static float g_viewPos[3] = {0, 0, 0}, g_viewAng[3] = {0, 0, 0}, g_viewFov = 0;
static volatile ULONGLONG g_viewPosAt = 0;
static void CopyViewPos(void* setup) {
    uint8_t* s = (uint8_t*)setup;
    if (!s || viewOriginOff <= 0 || !Readable(s + viewOriginOff, 12)) return;
    const float* o = (const float*)(s + viewOriginOff);
    if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) return;
    g_viewPos[0] = o[0]; g_viewPos[1] = o[1]; g_viewPos[2] = o[2];
    if (viewAnglesOff > 0 && Readable(s + viewAnglesOff, 12)) {
        const float* a = (const float*)(s + viewAnglesOff);
        if (std::isfinite(a[0]) && std::isfinite(a[1])) { g_viewAng[0] = a[0]; g_viewAng[1] = a[1]; g_viewAng[2] = a[2]; }
    }
    if (viewFovOff > 0 && Readable(s + viewFovOff, 4)) {
        const float f = *(const float*)(s + viewFovOff);
        if (std::isfinite(f) && f > 1.f && f < 179.f) g_viewFov = f;
    }
    g_viewPosAt = GetTickCount64();
}
static float ValueHook() {
    CopyViewPos(g_viewSetup);
    return g_userValue ? g_userValue() : 0.f;
}
static void ApplyHook(void* setup) {
    if (g_userApply) g_userApply(setup);
    CopyViewPos(setup);   // where the DemoTools2 camera put it
}
const LocalSlots& Locals() {
    static LocalSlots L;
    static bool done = false;
    if (done) return L;
    done = true;
    const Text t = ClientText();
    if (!t.start) return L;
    auto rip = [](uint8_t* at, int disp) -> uint8_t* { return at + disp + 4 + *(int32_t*)(at + disp); };
    // GetLocalPlayerController(slot) / GetLocalPlayerPawn(slot): slot -1 = the active one, then array[slot]
    const char* getter = "83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1";
    // where the game makes a controller the local one: array[slot] = controller; controller->m_bIsLocalPlayerController = 1
    if (uint8_t* p = FindPattern(t, "48 8D 0D ?? ?? ?? ?? ?? ?? 4C 89 ?? ?? 41 C6 86 ?? ?? ?? ?? 01")) {
        L.controllers = (void**)rip(p, 3);
        L.isLocal = *(int32_t*)(p + 16);
    }
    // the other getter (not the controllers) is the pawns
    std::vector<int> bytes;
    for (const char* q = getter; *q;) {
        if (*q == ' ') { ++q; continue; }
        if (*q == '?') { bytes.push_back(-1); q += 2; continue; }
        bytes.push_back((int)std::strtol(q, (char**)&q, 16));
    }
    for (size_t i = 0; i + bytes.size() <= t.size; ++i) {
        size_t k = 0;
        while (k < bytes.size() && (bytes[k] < 0 || t.start[i + k] == bytes[k])) ++k;
        if (k != bytes.size()) continue;
        void** arr = (void**)rip(t.start + i, 34);
        if (arr != L.controllers && !L.pawns) L.pawns = arr;
    }
    // IsHLTV(controller): controller->m_bIsLocalPlayerController ? g_bIsHLTV : controller->m_bIsHLTV
    if (uint8_t* p = FindPattern(t, "80 B8 ?? ?? ?? ?? 00 74 09 0F B6 05 ?? ?? ?? ?? EB 07 0F B6 80")) {
        L.hltv = rip(p, 12);
        L.isHltv = *(int32_t*)(p + 21);
        if (L.isLocal && *(int32_t*)(p + 2) != L.isLocal) Log("POV: two different 'local player' fields (0x%x / 0x%x)", (unsigned)L.isLocal, (unsigned)*(int32_t*)(p + 2));
    }
    Log("POV: local controllers %s, local pawns %s, GOTV flag %s, fields 0x%x / 0x%x", L.controllers ? "found" : "MISSING",
        L.pawns ? "found" : "MISSING", L.hltv ? "found" : "MISSING", (unsigned)L.isLocal, (unsigned)L.isHltv);
    return L;
}

bool HasModuleString(const char* module, const char* text) {
    HMODULE m = GetModuleHandleA(module);
    if (!m || !text || !*text) return false;
    const uint8_t* base = (const uint8_t*)m;
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const size_t n = std::strlen(text) + 1;   // with its 0, so "mirv_pov" doesn't match "mirv_pov_radio_mode"
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_READ) || (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = base + sec[i].VirtualAddress;
        const size_t size = sec[i].Misc.VirtualSize;
        if (size < n || !Readable(p, size)) continue;
        for (size_t k = 1; k + n <= size; ++k)
            if (p[k] == (uint8_t)text[0] && p[k - 1] == 0 && std::memcmp(p + k, text, n) == 0) return true;
    }
    return false;
}

float ViewFov() { return g_viewPosAt && GetTickCount64() - g_viewPosAt <= 1000 ? g_viewFov : 0.f; }
bool ViewPosition(float out[3], float angles[3]) {
    if (!g_viewPosAt || GetTickCount64() - g_viewPosAt > 1000) return false;
    out[0] = g_viewPos[0]; out[1] = g_viewPos[1]; out[2] = g_viewPos[2];
    if (angles) { angles[0] = g_viewAng[0]; angles[1] = g_viewAng[1]; angles[2] = g_viewAng[2]; }
    return true;
}

bool InstallViewHook(ViewValueFn value, ViewApplyFn apply) {
    static bool done = false;
    if (done) return true;
    Text t = ClientText();
    // xorps xmm7,xmm7; test al,al; je; lea rcx,[cl_demoviewoverride]; call GetFloat; comiss xmm0,xmm7; jbe;
    // lea rdx,[rbx+angles]; lea rcx,[rbx+origin]; call CalcDemoViewOverride; jmp
    // (the 16 bytes before it are the IsPlayingDemo call that HLAE rewrites, so they are not part of the pattern)
    uint8_t* p = FindPattern(t, "0F 57 FF 84 C0 74 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 2F C7 76 ?? 48 8D 93 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? EB");
    if (!p) {
        // Without its first 7 bytes (xorps / test / je): if an HLAE build patches a little more than the IsPlayingDemo
        // call in front of it, the rest is still found. Only bytes from the lea on are used and changed below.
        if (uint8_t* q = FindPattern(t, "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 2F C7 76 ?? 48 8D 93 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? EB")) {
            p = q - 7;
            Log("camera: view hook found without its first bytes (changed by HLAE or another tool) - using it");
        }
    }
    if (!p) { Log("camera: view hook NOT found - the DemoTools2 camera can't take over the view in this CS2 version"); return false; }
    uint8_t* lea = p + 7;
    uint8_t* call = p + 14;
    g_overrideCvarRef = lea + 7 + *(int32_t*)(lea + 3);
    g_getFloat = (GetFloatFn)(call + 5 + *(int32_t*)(call + 1));
    uint8_t* leaAng = p + 24;
    uint8_t* leaOrg = p + 31;
    uint8_t* call2 = p + 38;
    viewAnglesOff = *(int32_t*)(leaAng + 3);
    viewOriginOff = *(int32_t*)(leaOrg + 3);
    viewFovOff = viewOriginOff - 8;   // as HLAE: fov at +0x498 right before the origin at +0x4a0
    g_calcDemoView = (CalcDemoViewFn)(call2 + 5 + *(int32_t*)(call2 + 1));
    if (viewOriginOff < 0x100 || viewOriginOff > 0x2000 || viewAnglesOff != viewOriginOff + 0x18) {
        Log("camera: view layout unexpected (origin +0x%x, angles +0x%x) - not hooking", (unsigned)viewOriginOff, (unsigned)viewAnglesOff);
        return false;
    }
    g_userValue = value;
    g_userApply = apply;
    // A small piece of code that remembers the view setup (rbx) before asking value(), so the camera position is
    // known every frame (the Cinematics map and quick moves use it), also when the DemoTools2 camera is off.
    uint8_t* thunk = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunk) { Log("camera: couldn't allocate the view hook"); return false; }
    {
        void* slot = (void*)&g_viewSetup;
        void* fn = (void*)&ValueHook;
        uint8_t code[] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,    // mov rax, &g_viewSetup
                          0x48, 0x89, 0x18,                      // mov [rax], rbx
                          0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,    // mov rax, ValueHook
                          0xFF, 0xE0};                           // jmp rax
        std::memcpy(code + 2, &slot, 8);
        std::memcpy(code + 15, &fn, 8);
        std::memcpy(thunk, code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), thunk, sizeof(code));
    }
    // 1) lea rcx,[cvar]; call GetFloat   ->  mov rax, thunk; call rax            (12 bytes)
    uint8_t a[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0};
    std::memcpy(a + 2, &thunk, 8);
    // 2) lea rdx; lea rcx; call Calc...  ->  mov rcx, rbx; mov rax, apply; call rax; nop x4   (19 bytes)
    uint8_t b[19] = {0x48, 0x89, 0xD9, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0, 0x90, 0x90, 0x90, 0x90};
    void* applyFn = (void*)&ApplyHook;
    std::memcpy(b + 5, &applyFn, 8);
    if (!Patch(lea, a, sizeof(a)) || !Patch(leaAng, b, sizeof(b))) { Log("camera: couldn't write the view hook"); return false; }
    done = true;
    Log("camera: view hook installed (view origin +0x%x, angles +0x%x, fov +0x%x)", (unsigned)viewOriginOff, (unsigned)viewAnglesOff, (unsigned)viewFovOff);
    return true;
}

// "Is a demo playing" decides whether DemoTools2 does anything at all, so it is checked two ways: the engine's
// IsPlayingDemo, and - when its function numbers were read from the game's own code - that the engine's demo file is
// open. If the IsPlayingDemo call crashed last session (a CS2 update moved it), only the second check is used.
bool IsPlayingDemo() {
    using Fn = bool (*)(void*);
    using GetDemoFileFn = void* (*)(void*);
    if (!engine) return false;
    static const bool idxOff = CrashedLastTime("demo-check");
    if (!idxOff && !((Fn)VFunc(engine, I.engineIsPlayingDemo))(engine)) return false;
    if (!g_demoFileFromCode || g_dOpen <= 0) return !idxOff;
    void* demo = ((GetDemoFileFn)VFunc(engine, I.engineGetDemoFile))(engine);
    static void* checked = nullptr;
    if (demo != checked) {
        if (!Readable(demo, 0x10) || !Readable(*(void**)demo, (g_dOpen + 1) * sizeof(void*))) return false;
        checked = demo;
    }
    return ((Fn)VFunc(demo, g_dOpen))(demo);
}

int DemoTick() {
    using GetDemoFileFn = void* (*)(void*);
    using TickFn = int (*)(void*);
    if (!engine) return -1;
    void* demo = ((GetDemoFileFn)VFunc(engine, I.engineGetDemoFile))(engine);
    static void* checked = nullptr;   // the memory check is a system call: once per demo object, not every call
    if (demo != checked) {
        if (!Readable(demo)) return -1;
        checked = demo;
    }
    return ((TickFn)VFunc(demo, I.demoGetDemoTick))(demo);
}

void ExecuteCommand(const char* cmd) {
    using Fn = void (*)(void*, int, const char*, bool, double, uint64_t);
    if (!engine) return;
    Log("console: %s", cmd);
    ((Fn)VFunc(engine, I.engineExecuteClientCmd))(engine, 0, cmd, false, NAN, 0);   // queued: runs between frames, not inside ours
}

// ---------------------------------------------------------------- entities
// CGameEntitySystem +0x10: 64 pointers to chunks of 512 CEntityIdentity. Identity +0x00 = entity, +0x10 = handle.
static void* g_entitySystemVtable = nullptr;   // CGameEntitySystem, from RTTI
static bool g_entitySystemChecked = false;

static bool IsEntitySystem(void* p) {
    return Readable(p, 0x10 + 64 * 8) && (!g_entitySystemVtable || Get<void*>(p, 0) == g_entitySystemVtable);
}

void* EntitySystem() {
    if (!g_entitySystemChecked) {
        g_entitySystemChecked = true;
        g_entitySystemVtable = FindVtable("client.dll", ".?AVCGameEntitySystem@@");
        Log(g_entitySystemVtable ? "CGameEntitySystem table found" : "WARNING: CGameEntitySystem table not found, entity list unchecked");
    }
    void* es = Readable((uint8_t*)resourceService + I.entitySystemOffset) ? Get<void*>(resourceService, I.entitySystemOffset) : nullptr;
    static void* g_checkedEs = nullptr;   // checking costs system calls: do it once per pointer
    if (es && es == g_checkedEs) return es;
    if (es && IsEntitySystem(es)) { g_checkedEs = es; return es; }
    // The offset moved in a CS2 update: look for the pointer near it.
    static ULONGLONG nextSearch = 0;
    if (GetTickCount64() < nextSearch || !g_entitySystemVtable) return nullptr;
    nextSearch = GetTickCount64() + 2000;
    for (int off = 0; off < 0x200; off += 8) {
        if (!Readable((uint8_t*)resourceService + off)) break;
        void* cand = Get<void*>(resourceService, off);
        if (cand && IsEntitySystem(cand)) {
            Log("entity list found at offset 0x%x (config had 0x%x)", off, I.entitySystemOffset);
            I.entitySystemOffset = off;
            return cand;
        }
    }
    return nullptr;
}

int HighestChunk() { return 32; }   // networked entities live below index 16384

// Chunk pointers are checked once each, so a bad pointer is never followed.
static uint8_t* g_goodChunks[64] = {};
uint8_t* Chunk(void* es, int c) {
    uint8_t* chunk = Get<uint8_t*>(es, 0x10 + 8 * c);
    if (!chunk) return nullptr;
    if (g_goodChunks[c] == chunk) return chunk;
    if (!Readable(chunk, (size_t)O.identitySize * 512)) return nullptr;
    g_goodChunks[c] = chunk;
    return chunk;
}


void* IdentityAt(int index) {
    if (index < 0 || index >= 0x8000) return nullptr;
    void* es = EntitySystem();
    if (!es) return nullptr;
    uint8_t* chunk = Chunk(es, index >> 9);
    if (!chunk) return nullptr;
    return chunk + O.identitySize * (index & 0x1ff);
}

void* EntityAt(int index) {
    void* id = IdentityAt(index);
    void* e = id ? Get<void*>(id, 0) : nullptr;
    return LooksLikePtr(e) ? e : nullptr;
}

void* FromHandleExact(uint32_t handle) {
    if (handle == 0xffffffffu || handle == 0xfffffffeu) return nullptr;
    void* id = IdentityAt(handle & 0x7fff);
    if (!id || Get<uint32_t>(id, 0x10) != handle) return nullptr;
    void* e = Get<void*>(id, 0);
    return LooksLikePtr(e) ? e : nullptr;
}

void* FromHandle(uint32_t handle) {
    if (handle == 0xffffffffu) return nullptr;
    void* id = IdentityAt(handle & 0x7fff);
    if (!id) return nullptr;
    if ((Get<uint32_t>(id, 0x10) & 0x7fff) != (handle & 0x7fff)) return nullptr;
    void* e = Get<void*>(id, 0);
    return LooksLikePtr(e) ? e : nullptr;
}

const char* DesignerName(void* entity) {
    if (!LooksLikePtr(entity)) return nullptr;
    void* id = Get<void*>(entity, O.ent_pEntity);
    if (!LooksLikePtr(id)) return nullptr;
    const char* n = Get<const char*>(id, O.identity_designerName);
    return LooksLikePtr(n) ? n : nullptr;
}

// The game folder (...\game\csgo\), from where client.dll is.
std::string GameFolder() {
    char path[MAX_PATH] = {};
    HMODULE m = GetModuleHandleA("client.dll");
    if (!m || !GetModuleFileNameA(m, path, MAX_PATH)) return {};
    std::string s = path;
    for (int i = 0; i < 3; ++i) { size_t k = s.find_last_of("\\/"); if (k == std::string::npos) return {}; s.resize(k); }
    return s + "\\";
}

uint32_t HandleOf(void* entity) {
    void* id = Get<void*>(entity, O.ent_pEntity);
    return id ? Get<uint32_t>(id, 0x10) : 0xffffffffu;
}
// ---- sequences (old-style animations)
// The script function CBaseAnimGraph_API::PlaySequence: its entry in the script binding table holds the name and,
// 9 pointers on, a small function that calls C_BaseAnimGraph::PlaySequence(entity, "name") as its second call.
using PlaySequenceFn = bool (*)(void*, const char*);
static PlaySequenceFn g_playSequence = nullptr;
static ptrdiff_t g_animModeFlag = 0;

static void FindPlaySequence(const Text& t) {
    uint8_t* name = FindString(t, "CBaseAnimGraph_API::PlaySequence");
    auto nt = (IMAGE_NT_HEADERS64*)(t.base + ((IMAGE_DOS_HEADER*)t.base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; name && !g_playSequence && i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".data", 6)) continue;
        uint8_t* d = t.base + sec[i].VirtualAddress;
        const size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 10 * 8 <= n; k += 8) {
            if (*(uint8_t**)(d + k) != name) continue;
            uint8_t* thunk = *(uint8_t**)(d + k + 9 * 8);
            if (thunk < t.start || thunk >= t.start + t.size) continue;
            int calls = 0;
            for (uint8_t* q = thunk; q < thunk + 0x50; ++q)
                if (uint8_t* c = CallTarget(t, q)) {
                    if (++calls == 2) { g_playSequence = (PlaySequenceFn)c; break; }
                }
            break;
        }
    }
    Log(g_playSequence ? "sequences: PlaySequence found" : "sequences: PlaySequence NOT found");
}
static void FindAnimModeFlag(const Text& t);
// Found the first time old animations are switched on (nothing of this runs for anyone who doesn't use them).
static void FindSequenceFunctions() {
    static bool done = false;
    if (done) return;
    done = true;
    Step step("old-anims: finding");
    const Text t = ClientText();
    if (!t.start) return;
    FindPlaySequence(t);
    FindAnimModeFlag(t);
}
bool CanPlaySequence() { FindSequenceFunctions(); return g_playSequence != nullptr && g_animModeFlag != 0; }

// Which animation system an entity uses: CBaseAnimGraphController::m_nAnimationAlgorithm, in the entity's body
// component (C_BaseEntity::m_CBodyComponent -> CBodyComponentBaseAnimGraph::m_animationController). When the network
// changes it, its callback "AG2_OnAnimGraphDefinitionOrModeChanged" only sets a flag on the entity
// (mov byte [rcx+X], 1; ret) and the game rebuilds the animation set-up later in the frame.
static void FindAnimModeFlag(const Text& t) {
    if (uint8_t* str = FindString(t, "AG2_OnAnimGraphDefinitionOrModeChanged"))
        ForEachLea(t, str, [&](uint8_t* ins) {
            if (g_animModeFlag || ins[0] != 0x48 || ins[2] != 0x15) return;
            for (uint8_t* q = ins - 7; q > ins - 0x60; --q)
                if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x05) {
                    uint8_t* h = LeaTarget(q);
                    if (h >= t.start && h < t.start + t.size && h[0] == 0xC6 && h[1] == 0x81 && h[6] == 0x01 && h[7] == 0xC3)
                        g_animModeFlag = *(int32_t*)(h + 2);
                    break;
                }
        });
    Log(g_animModeFlag ? "animation mode switch found (flag at +0x%x)" : "animation mode switch NOT found", (unsigned)g_animModeFlag);
}
static uint8_t* AnimAlgorithmField(void* entity) {
    static ptrdiff_t oBody = -1, oCtrl = -1, oAlg = -1;
    if (oBody < 0) {
        if (!SchemaField("C_BaseEntity", "m_CBodyComponent", oBody)) oBody = 0;
        if (!SchemaField("CBodyComponentBaseAnimGraph", "m_animationController", oCtrl)) oCtrl = 0;
        if (!SchemaField("CBaseAnimGraphController", "m_nAnimationAlgorithm", oAlg)) oAlg = 0;
        Log("animation mode: body +0x%x, controller +0x%x, algorithm +0x%x", (unsigned)oBody, (unsigned)oCtrl, (unsigned)oAlg);
    }
    if (oBody <= 0 || oCtrl <= 0 || oAlg <= 0 || !entity) return nullptr;
    uint8_t* body = Get<uint8_t*>(entity, oBody);
    if (!body || !Readable(body + oCtrl + oAlg, 4)) return nullptr;
    return body + oCtrl + oAlg;
}
int AnimAlgorithm(void* entity) {
    uint8_t* f = AnimAlgorithmField(entity);
    return f ? *(int32_t*)f : -1;
}
bool SetAnimAlgorithm(void* entity, int value) {
    uint8_t* f = AnimAlgorithmField(entity);
    if (!f || !g_animModeFlag || !Readable((uint8_t*)entity + g_animModeFlag, 1)) return false;
    *f = (uint8_t)value;                                 // the low byte only: safe whether the field is 1 or 4 bytes
    *((uint8_t*)entity + g_animModeFlag) = 1;            // what the network's change callback does
    return true;
}
bool PlaySequence(void* entity, const char* name) { return g_playSequence && entity && name ? g_playSequence(entity, name) : false; }

// ---- DemoTools2's own files (addons)
// CS2's file system (VFileSystem017 in filesystem_stdio.dll) can be told to also look in another folder:
//   AddSearchPath(path, pathID, addType, priority, unknown)   - found by its shape: the only function in the table that
//   reads both stack arguments as  mov edi,[rsp+90h] / mov eax,[rsp+98h].
// DemoTools2 adds its folders at the END of the "GAME" path: CS2 only takes files from there that it doesn't have
// itself, so nothing of the game's is ever replaced, and nothing is written anywhere - it lasts until CS2 closes.
using AddSearchPathFn = void (*)(void*, const char*, const char*, int, int, int);
// FileExists(name, pathID): the vtable function that uses the text "CBaseFileSystem::FileExists" (its profiling name).
using FileExistsFn = bool (*)(void*, const char*, const char*);
static void* g_fs = nullptr;
static FileExistsFn g_fileExists = nullptr;
static void FindFileExists() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!g_fs) g_fs = Interface("filesystem_stdio.dll", "VFileSystem017");
    HMODULE m = GetModuleHandleA("filesystem_stdio.dll");
    if (!g_fs || !m) return;
    auto base = (uint8_t*)m;
    auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* str = nullptr;
    static const char kName[] = "CBaseFileSystem::FileExists";
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !str; ++i) {
        if (std::memcmp(sec[i].Name, ".rdata", 6)) continue;
        uint8_t* d = base + sec[i].VirtualAddress;
        for (size_t k = 1; k + sizeof(kName) <= sec[i].Misc.VirtualSize; ++k)
            if (d[k] == 'C' && d[k - 1] == 0 && !std::memcmp(d + k, kName, sizeof(kName))) { str = d + k; break; }
    }
    if (!str) return;
    void** vt = *(void***)g_fs;
    for (int i = 10; i < 60 && !g_fileExists; ++i) {
        uint8_t* f = (uint8_t*)vt[i];
        if (!Readable(f, 0x300)) break;
        for (int k = 0; k + 7 <= 0x300; ++k)
            if ((f[k] == 0x48 || f[k] == 0x4C) && f[k + 1] == 0x8D && (f[k + 2] & 0xC7) == 0x05 && f + k + 7 + *(int32_t*)(f + k + 3) == str) {
                g_fileExists = (FileExistsFn)f;
                Log("file system: FileExists found (function %d)", i);
                break;
            }
    }
}
int FileExists(const char* name, const char* pathId) {
    FindFileExists();
    if (!g_fileExists) return -1;
    return g_fileExists(g_fs, name, pathId) ? 1 : 0;
}

bool AddSearchPath(const char* dir, const char* pathId, bool atHead) {
    static void* fs = nullptr;
    static AddSearchPathFn add = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fs = Interface("filesystem_stdio.dll", "VFileSystem017");
        g_fs = fs;
        if (fs) {
            void** vt = *(void***)fs;
            for (int i = 10; i < 120 && !add; ++i) {
                uint8_t* f = (uint8_t*)vt[i];
                if (!Readable(f, 0x40)) break;
                static const uint8_t a[] = {0x8B, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00}, b[] = {0x8B, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00};
                bool ha = false, hb = false;
                for (int k = 0; k + 7 <= 0x40; ++k) {
                    if (!std::memcmp(f + k, a, 7)) ha = true;
                    if (!std::memcmp(f + k, b, 7)) hb = true;
                }
                if (ha && hb) { add = (AddSearchPathFn)f; Log("file system: AddSearchPath found (function %d)", i); }
            }
        }
        if (!add) Log("file system: AddSearchPath NOT found - DemoTools2's own files can't be used");
    }
    if (!add || !dir || !*dir) return false;
    add(fs, dir, pathId, atHead ? 0 /* PATH_ADD_TO_HEAD */ : 1 /* PATH_ADD_TO_TAIL */, 0, 0);
    Log("file system: CS2 also looks in %s for %s files (%s)", dir, pathId, atHead ? "first" : "only ones it doesn't have itself");
    return true;
}

}  // namespace Game
