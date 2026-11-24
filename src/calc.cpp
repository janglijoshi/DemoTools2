// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// Calcs and the DemoTools2 camera. See calc.h and README ("Camera and calcs").
#include "calc.h"
#include "campath.h"
#include "pov.h"
#include "record.h"
#include "console.h"
#include "game.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <unordered_map>

namespace Calc {

std::mutex lock;
CamState cam;
static bool g_quiet = false;   // the window's live sliders redefine calcs every frame they move: no console output then
void SetQuiet(bool q) { g_quiet = q; }

// ------------------------------------------------------------------ offsets (from the game's schema at start-up)
static struct {
    ptrdiff_t absOrigin = 0xc8, absRot = 0xd4;                 // CGameSceneNode
    ptrdiff_t viewOffset = 0xf60, viewOffsetZ = 0x20;         // C_BaseModelEntity::m_vecViewOffset (.m_vecZ.m_Value)
    ptrdiff_t eyeAngles = 0x3630;                              // C_CSPlayerPawn
    ptrdiff_t weaponServices = 0x12f0, activeWeapon = 0x60;    // C_BasePlayerPawn, CPlayer_WeaponServices
    ptrdiff_t thrower = 0;                                     // C_BaseGrenade::m_hThrower
    ptrdiff_t hudArms = 0x1dd8;                                // C_CSPlayerPawn::m_hHudModelArms
    ptrdiff_t health = 0x34c, lifeState = 0x354;               // C_BaseEntity
    ptrdiff_t playerPawn = 0, steamId = 0, playerName = 0, owner = 0;
} F;

static void ReadOffsets() {
    struct Want { ptrdiff_t* out; const char* cls; const char* field; };
    const Want wants[] = {
        {&F.absOrigin, "CGameSceneNode", "m_vecAbsOrigin"},
        {&F.absRot, "CGameSceneNode", "m_angAbsRotation"},
        {&F.viewOffset, "C_BaseModelEntity", "m_vecViewOffset"},
        {&F.viewOffsetZ, "CNetworkViewOffsetVector", "m_vecZ"},
        {&F.eyeAngles, "C_CSPlayerPawn", "m_angEyeAngles"},
        {&F.weaponServices, "C_BasePlayerPawn", "m_pWeaponServices"},
        {&F.activeWeapon, "CPlayer_WeaponServices", "m_hActiveWeapon"},
        {&F.thrower, "C_BaseGrenade", "m_hThrower"},
        {&F.hudArms, "C_CSPlayerPawn", "m_hHudModelArms"},
        {&F.health, "C_BaseEntity", "m_iHealth"},
        {&F.lifeState, "C_BaseEntity", "m_lifeState"},
    };
    int missing = 0;
    for (const Want& w : wants)
        if (!Game::SchemaField(w.cls, w.field, *w.out)) { ++missing; Log("camera: %s::%s not in schema, using 0x%x", w.cls, w.field, (unsigned)*w.out); }
    F.playerPawn = O.ctrl_hPlayerPawn;
    F.steamId = O.ctrl_steamID;
    F.playerName = O.ctrl_iszPlayerName;
    F.owner = O.ent_hOwnerEntity;
    Log("camera: entity offsets read (%d missing)", missing);
}

// ------------------------------------------------------------------ math (Source conventions)
static const float kPi = 3.14159265358979f;
static float Rad(float d) { return d * kPi / 180.f; }
static float Deg(float r) { return r * 180.f / kPi; }
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float Len(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
static float Wrap180(float a) { a = std::fmod(a + 180.f, 360.f); if (a < 0) a += 360.f; return a - 180.f; }

struct Mat { Vec3 f, l, u; };   // columns: forward, left, up
static Mat FromAngles(Vec3 ang) {
    float sp = std::sin(Rad(ang.x)), cp = std::cos(Rad(ang.x));
    float sy = std::sin(Rad(ang.y)), cy = std::cos(Rad(ang.y));
    float sr = std::sin(Rad(ang.z)), cr = std::cos(Rad(ang.z));
    Mat m;
    m.f = {cp * cy, cp * sy, -sp};
    Vec3 right = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    m.l = right * -1.f;
    m.u = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
    return m;
}
static Vec3 ToAngles(const Mat& m) {
    float xy = std::sqrt(m.f.x * m.f.x + m.f.y * m.f.y);
    if (xy > 0.001f) return {Deg(std::atan2(-m.f.z, xy)), Deg(std::atan2(m.f.y, m.f.x)), Deg(std::atan2(m.l.z, m.u.z))};
    return {Deg(std::atan2(-m.f.z, xy)), Deg(std::atan2(-m.l.x, m.l.y)), 0.f};
}
static Vec3 Mul(const Mat& m, Vec3 v) { return m.f * v.x + m.l * v.y + m.u * v.z; }   // local (fwd, left, up) -> world
static Mat MulM(const Mat& a, const Mat& b) { return {Mul(a, b.f), Mul(a, b.l), Mul(a, b.u)}; }
static Vec3 LookAt(Vec3 from, Vec3 to) {
    Vec3 d = to - from;
    float xy = std::sqrt(d.x * d.x + d.y * d.y);
    if (xy < 0.0001f && std::fabs(d.z) < 0.0001f) return {0, 0, 0};
    return {Deg(std::atan2(-d.z, xy)), Deg(std::atan2(d.y, d.x)), 0.f};
}

static Mat FromQuat(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    Mat m;
    m.f = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
    m.l = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
    m.u = {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
    return m;
}

// The standard CS2 player skeleton (all agents share it). Numbers also work: "bone p 13".
static const struct { const char* name; int index; } kBones[] = {
    {"pelvis", 0},      {"spine_1", 2},      {"spine_2", 4},      {"neck", 5},         {"head", 6},
    {"clavicle_l", 7},  {"arm_upper_l", 8},  {"arm_lower_l", 9},  {"hand_l", 10},
    {"clavicle_r", 12}, {"arm_upper_r", 13}, {"arm_lower_r", 14}, {"hand_r", 15},
    {"leg_upper_l", 22}, {"leg_lower_l", 23}, {"ankle_l", 24},
    {"leg_upper_r", 25}, {"leg_lower_r", 26}, {"ankle_r", 27},
    {"shoulder_l", 8},  {"shoulder_r", 13},  {"elbow_l", 9},      {"elbow_r", 14},     {"knee_l", 23}, {"knee_r", 26},
};
static int BoneIndex(const std::string& s) {
    if (!s.empty() && std::isdigit((unsigned char)s[0])) return std::atoi(s.c_str());
    std::string l;
    for (char c : s) l += (char)std::tolower((unsigned char)c);
    for (auto& b : kBones) if (l == b.name) return b.index;
    return -1;
}
// Attachment names worth trying (weapons, grenades, players); "dt2_calc probe" lists the ones a model has.
static const char* kAttachments[] = {"muzzle_flash", "muzzle_flash2", "shell_eject", "eject_brass", "silencer", "scope", "camera",
                                     "weapon_hand_r", "weapon_hand_l", "stattrak", "nametag", "keychain", "sticker_a", "sticker_b",
                                     "sticker_c", "sticker_d", "sticker_e", "head", "eyes", "facemask", "forward", "c4", "led",
                                     "fuse", "pin", "spoon", "trail"};

// ------------------------------------------------------------------ time
static double RealSeconds() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
// Demo time when known (follows host_framerate while recording, stops while paused), else real time.
static double Now() { float t = Game::CurTime(); return t > 0 ? (double)t : RealSeconds(); }

// ------------------------------------------------------------------ entities
static bool EntOk(void* e) { return e && Readable(e, 0x100); }

static bool EntityView(void* e, View& v) {
    void* node = Game::SceneNode(e);
    if (!node) return false;
    v.pos = Get<Vec3>(node, F.absOrigin);
    v.ang = Get<Vec3>(node, F.absRot);
    return std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
}

static bool IsPawn(void* e) {
    const char* dn = Game::DesignerName(e);
    return dn && std::strcmp(dn, "player") == 0;
}

static bool EyeView(void* e, View& v) {
    if (!EntityView(e, v)) return false;
    if (IsPawn(e)) {
        float z = Get<float>(e, F.viewOffset + F.viewOffsetZ);
        if (!(z > 1.f && z < 128.f)) z = 64.f;
        v.pos.z += z;
        Vec3 a = Get<Vec3>(e, F.eyeAngles);
        if (std::isfinite(a.x) && std::isfinite(a.y)) v.ang = {a.x, a.y, 0.f};
    }
    return true;
}

struct PlayerRef { void* ctrl; void* pawn; uint64_t xuid; std::string name; };
static std::vector<PlayerRef> Players() {
    std::vector<PlayerRef> out;
    for (int i = 1; i <= 64; ++i) {
        void* ctrl = Game::EntityAt(i);
        if (!ctrl) continue;
        const char* dn = Game::DesignerName(ctrl);
        if (!dn || std::strcmp(dn, "cs_player_controller") != 0) continue;
        PlayerRef p;
        p.ctrl = ctrl;
        p.pawn = Game::FromHandle(Get<uint32_t>(ctrl, F.playerPawn));
        p.xuid = Get<uint64_t>(ctrl, F.steamId);
        char name[129] = {};
        std::memcpy(name, (uint8_t*)ctrl + F.playerName, 128);
        p.name = name;
        out.push_back(std::move(p));
    }
    return out;
}

static std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

// ------------------------------------------------------------------ calcs
struct Node {
    std::string name;
    Kind kind = Kind::VecAng;
    std::string type;                 // player, eye, smooth, ...
    std::vector<std::string> args;    // as typed after the type
    // evaluation (once per frame)
    uint64_t frame = 0;
    bool ok = false, busy = false;
    void* ent = nullptr;              // Handle
    View view;                        // VecAng
    float fov = 90.f;                 // Fov
    // state for smooth / orbit / follow
    bool hasState = false;
    double lastTime = 0;
    View state;
    float stateFov = 0;
    Vec3 lastTarget, dir;
    uint32_t lastHandle = 0;
};
static std::map<std::string, std::unique_ptr<Node>> g_nodes;   // by lower-case name
static uint64_t g_frame = 1;
static std::vector<void*> g_grenades;    // projectiles, oldest first (to find "the newest grenade")
static std::unordered_map<void*, double> g_grenadeSeen;

static Node* Find(const std::string& name) {
    auto it = g_nodes.find(Lower(name));
    return it == g_nodes.end() ? nullptr : it->second.get();
}
static float Num(const std::vector<std::string>& a, size_t i, float def) {
    if (i >= a.size()) return def;
    char* end = nullptr;
    float v = std::strtof(a[i].c_str(), &end);
    return end && end != a[i].c_str() ? v : def;
}

static bool EvalHandle(Node* n);
static bool EvalView(Node* n);
static bool EvalFov(Node* n);
static std::map<uint32_t, uint32_t> g_keptGun;   // player pawn handle -> the gun the camera stays on
void ResetKeptGuns() { g_keptGun.clear(); }   // caller holds `lock`

// ------------------------------------------------------------------ mount points (see calc.h)
std::vector<MountKey> mountKeys;
bool mountKeysOn = false;
static bool g_mountOverride = false;
static float g_mountOverrideV[7];
static double g_mountOverrideT = 0;

void MountOverride(const float v[7]) {   // caller holds `lock`
    std::copy(v, v + 7, g_mountOverrideV);
    g_mountOverride = true;
    g_mountOverrideT = Game::CurTime();
}
int MountKeysAdd(const float v[7], std::string* why) {   // caller holds `lock`
    const double t = Game::CurTime();
    if (t <= 0) { if (why) *why = "No demo time - play a demo first."; return -1; }
    MountKey k;
    k.t = t;
    k.tick = Game::DemoTick();
    std::copy(v, v + 7, k.v);
    for (size_t i = 0; i < mountKeys.size(); ++i)
        if (std::fabs(mountKeys[i].t - t) < 0.5 / 64) { mountKeys[i] = k; return (int)i; }   // same tick: replace
    auto it = std::upper_bound(mountKeys.begin(), mountKeys.end(), t, [](double a, const MountKey& b) { return a < b.t; });
    return (int)(mountKeys.insert(it, k) - mountKeys.begin());
}
// Smooth glide (Catmull-Rom through the points, ends held); angles take the short way round.
bool MountKeysAt(double t, float out[7]) {
    const size_t n = mountKeys.size();
    if (!n) return false;
    if (n == 1 || t <= mountKeys[0].t) { std::copy(mountKeys[0].v, mountKeys[0].v + 7, out); return true; }
    if (t >= mountKeys[n - 1].t) { std::copy(mountKeys[n - 1].v, mountKeys[n - 1].v + 7, out); return true; }
    size_t i = 0;
    while (i + 2 < n && t >= mountKeys[i + 1].t) ++i;
    const MountKey& a = mountKeys[i];
    const MountKey& b = mountKeys[i + 1];
    const MountKey& p = mountKeys[i ? i - 1 : i];
    const MountKey& q = mountKeys[i + 2 < n ? i + 2 : i + 1];
    const double u = (t - a.t) / std::max(1e-6, b.t - a.t);
    for (int c = 0; c < 7; ++c) {
        float v1 = a.v[c], v0 = p.v[c], v2 = b.v[c], v3 = q.v[c];
        if (c >= 3 && c <= 5) {   // angles: unwrap around v1
            v0 = v1 + Wrap180(v0 - v1);
            v2 = v1 + Wrap180(v2 - v1);
            v3 = v2 + Wrap180(v3 - v2);
        }
        if (c == 6 && (v0 <= 0 || v1 <= 0 || v2 <= 0 || v3 <= 0)) {   // "game" fov in the mix: step, no glide
            out[c] = u < 0.5 ? v1 : v2;
            continue;
        }
        const double uu = u * u, uuu = uu * u;
        out[c] = (float)(0.5 * (2 * v1 + (-v0 + v2) * u + (2 * v0 - 5 * v1 + 4 * v2 - v3) * uu + (-v0 + 3 * v1 - 3 * v2 + v3) * uuu));
    }
    return true;
}
// The values a mountkeys calc uses now: the sliders while the demo stands still after they moved, else the points.
static bool MountNow(float v[7]) {
    const double t = Game::CurTime();
    if (g_mountOverride && std::fabs(t - g_mountOverrideT) < 0.02) { std::copy(g_mountOverrideV, g_mountOverrideV + 7, v); return true; }
    g_mountOverride = false;
    return mountKeysOn && MountKeysAt(t, v);
}

static Node* Ref(Node* self, size_t i, Kind kind) {
    if (i >= self->args.size()) return nullptr;
    Node* r = Find(self->args[i]);
    return r && r->kind == kind ? r : nullptr;
}
static void* RefEntity(Node* self, size_t i) {
    Node* r = Ref(self, i, Kind::Handle);
    return r && EvalHandle(r) ? r->ent : nullptr;
}
static bool RefView(Node* self, size_t i, View& v) {
    Node* r = Ref(self, i, Kind::VecAng);
    if (!r || !EvalView(r)) return false;
    v = r->view;
    return true;
}

// Called before each evaluation: guards against loops (a calc that uses itself).
struct Busy {
    Node* n;
    bool ok;
    explicit Busy(Node* node) : n(node), ok(!node->busy) { n->busy = true; }
    ~Busy() { if (ok) n->busy = false; }
};

static void TrackGrenades() {
    static uint64_t frame = 0;
    if (frame == g_frame) return;
    frame = g_frame;
    void* es = Game::EntitySystem();
    if (!es) return;
    std::unordered_map<void*, bool> now;
    for (int c = 0; c < Game::HighestChunk(); ++c) {
        uint8_t* chunk = Game::Chunk(es, c);
        if (!chunk) continue;
        for (int i = 0; i < 512; ++i) {
            uint8_t* id = chunk + (size_t)i * O.identitySize;
            void* e = Get<void*>(id, 0);
            if (!e) continue;
            const char* dn = Get<const char*>(id, O.identity_designerName);
            if (!dn || !Readable(dn, 4)) continue;
            size_t len = strnlen(dn, 64);
            if (len < 11 || std::strcmp(dn + len - 11, "_projectile") != 0) continue;
            now[e] = true;
        }
    }
    double t = Now();
    for (auto it = g_grenadeSeen.begin(); it != g_grenadeSeen.end();) it = now.count(it->first) ? std::next(it) : g_grenadeSeen.erase(it);
    for (auto& kv : now) if (!g_grenadeSeen.count(kv.first)) g_grenadeSeen[kv.first] = t;
}

static bool EvalHandle(Node* n) {
    if (n->frame == g_frame) return n->ok;
    Busy busy(n);
    if (!busy.ok) return false;
    n->frame = g_frame;
    n->ok = false;
    n->ent = nullptr;
    const std::string& t = n->type;
    if (t == "index") {
        n->ent = Game::EntityAt((int)Num(n->args, 0, -1));
    } else if (t == "player") {
        std::string want = n->args.empty() ? "" : n->args[0];
        bool isXuid = want.size() >= 15 && std::all_of(want.begin(), want.end(), ::isdigit);
        uint64_t xuid = isXuid ? std::strtoull(want.c_str(), nullptr, 10) : 0;
        std::string lw = Lower(want);
        for (const PlayerRef& p : Players()) {
            if (isXuid ? p.xuid == xuid : Lower(p.name).find(lw) != std::string::npos) { n->ent = p.pawn; break; }
        }
    } else if (t == "observed") {
        // The player whose eyes the demo shows: only that pawn has a first-person arms model.
        for (const PlayerRef& p : Players()) {
            if (!p.pawn) continue;
            uint32_t h = Get<uint32_t>(p.pawn, F.hudArms);
            if (Game::FromHandleExact(h)) { n->ent = p.pawn; break; }
        }
        if (!n->ent && n->lastHandle) n->ent = Game::FromHandleExact(n->lastHandle);   // keep the last one (free camera, chase cam)
        if (n->ent) n->lastHandle = Game::HandleOf(n->ent);
    } else if (t == "weapon") {
        if (void* pawn = RefEntity(n, 0)) {
            uint8_t* ws = Get<uint8_t*>(pawn, F.weaponServices);
            if (Readable(ws + F.activeWeapon, 4)) n->ent = Game::FromHandle(Get<uint32_t>(ws, F.activeWeapon));
        }
    } else if (t == "keptgun") {
        // The gun that was in their hand when the camera was put on it - and it stays that gun when they switch to
        // another weapon (then it is holstered on their body). A new gun only when that one is gone or someone else
        // has it (dropped, picked up, round over).
        if (void* pawn = RefEntity(n, 0)) {
            const uint32_t ph = Game::HandleOf(pawn);
            void* w = nullptr;
            auto it = g_keptGun.find(ph);
            if (it != g_keptGun.end()) {
                w = Game::FromHandleExact(it->second);
                if (w && Game::FromHandle(Get<uint32_t>(w, F.owner)) != pawn) w = nullptr;
            }
            if (!w) {
                uint8_t* ws = Get<uint8_t*>(pawn, F.weaponServices);
                if (Readable(ws + F.activeWeapon, 4)) w = Game::FromHandle(Get<uint32_t>(ws, F.activeWeapon));
                if (w) g_keptGun[ph] = Game::HandleOf(w);
            }
            n->ent = w;
        }
    } else if (t == "viewmodel") {
        // The first-person gun of a player (only the player whose eyes the demo shows has one).
        if (void* pawn = RefEntity(n, 0)) {
            uint8_t* ws = Get<uint8_t*>(pawn, F.weaponServices);
            if (Readable(ws + F.activeWeapon, 4))
                if (void* w = Game::FromHandle(Get<uint32_t>(ws, F.activeWeapon))) n->ent = Game::HudModel(w);
        }
    } else if (t == "owner") {
        if (void* e = RefEntity(n, 0)) n->ent = Game::FromHandle(Get<uint32_t>(e, F.owner));
    } else if (t == "grenade") {
        TrackGrenades();
        void* by = n->args.empty() ? nullptr : RefEntity(n, 0);
        if (!n->args.empty() && !by) { n->ok = false; return false; }
        double best = -1;
        for (auto& kv : g_grenadeSeen) {
            if (by && F.thrower && Game::FromHandle(Get<uint32_t>(kv.first, F.thrower)) != by) continue;
            if (kv.second > best) { best = kv.second; n->ent = kv.first; }
        }
    }
    n->ok = EntOk(n->ent);
    return n->ok;
}

static void SmoothTo(Node* n, const View& target, float posTime, float angTime, float maxSpeed) {
    double t = Now();
    double dt = n->hasState ? t - n->lastTime : -1;
    n->lastTime = t;
    if (!n->hasState || dt < 0 || dt > 1.0 || Len(target.pos - n->state.pos) > 2000.f) {   // first frame or a jump: snap
        n->state = target;
        n->hasState = true;
        return;
    }
    if (dt == 0) return;   // paused
    float kp = posTime > 0 ? 1.f - (float)std::exp(-dt / posTime) : 1.f;
    float ka = angTime > 0 ? 1.f - (float)std::exp(-dt / angTime) : 1.f;
    Vec3 step = (target.pos - n->state.pos) * kp;
    if (maxSpeed > 0) {
        float l = Len(step), cap = maxSpeed * (float)dt;
        if (l > cap) step = step * (cap / l);
    }
    n->state.pos = n->state.pos + step;
    n->state.ang.x = n->state.ang.x + Wrap180(target.ang.x - n->state.ang.x) * ka;
    n->state.ang.y = Wrap180(n->state.ang.y + Wrap180(target.ang.y - n->state.ang.y) * ka);
    n->state.ang.z = n->state.ang.z + Wrap180(target.ang.z - n->state.ang.z) * ka;
}

static bool EvalView(Node* n) {
    if (n->frame == g_frame) return n->ok;
    Busy busy(n);
    if (!busy.ok) return false;
    n->frame = g_frame;
    n->ok = false;
    const std::string& t = n->type;
    View v;
    if (t == "value") {
        v.pos = {Num(n->args, 0, 0), Num(n->args, 1, 0), Num(n->args, 2, 0)};
        v.ang = {Num(n->args, 3, 0), Num(n->args, 4, 0), Num(n->args, 5, 0)};
        n->ok = true;
    } else if (t == "origin" || t == "eye") {
        void* e = RefEntity(n, 0);
        n->ok = e && (t == "eye" ? EyeView(e, v) : EntityView(e, v));
    } else if (t == "attachment") {
        void* e = RefEntity(n, 0);
        float p[3], q[4];
        if (e && n->args.size() > 1 && Game::Attachment(e, n->args[1].c_str(), p, q)) {
            v.pos = {p[0], p[1], p[2]};
            v.ang = ToAngles(FromQuat(q));
            n->ok = true;
        } else if (e && n->args.size() > 1) {
            // a holstered gun may have no working attachment: then its own position and turn (the gun's origin)
            if (EntityView(e, v)) {
                n->ok = true;
                static std::set<std::string> said;
                if (said.insert(n->args[1]).second)
                    Log("camera: attachment '%s' not available on %s (holstered?) - using the gun's own position",
                        n->args[1].c_str(), Game::DesignerName(e) ? Game::DesignerName(e) : "?");
            }
        }
    } else if (t == "bone") {
        void* e = RefEntity(n, 0);
        float p[3], q[4];
        int idx = n->args.size() > 1 ? BoneIndex(n->args[1]) : -1;
        if (e && idx >= 0 && Game::Bone(e, idx, p, q)) {
            v.pos = {p[0], p[1], p[2]};
            v.ang = ToAngles(FromQuat(q));
            n->ok = true;
        }
    } else if (t == "offset") {
        View p;
        if (RefView(n, 0, p)) {
            Mat m = FromAngles(p.ang);
            // forward, right, up -> local (forward, left, up)
            v.pos = p.pos + Mul(m, {Num(n->args, 1, 0), -Num(n->args, 2, 0), Num(n->args, 3, 0)});
            v.ang = ToAngles(MulM(m, FromAngles({Num(n->args, 4, 0), Num(n->args, 5, 0), Num(n->args, 6, 0)})));
            n->ok = true;
        }
    } else if (t == "mountkeys") {
        // like offset, but forward/right/up/pitch/yaw/roll come from the mount points (the numbers typed are used
        // when there are none)
        View p;
        if (RefView(n, 0, p)) {
            float k[7] = {Num(n->args, 1, 0), Num(n->args, 2, 0), Num(n->args, 3, 0), Num(n->args, 4, 0), Num(n->args, 5, 0), Num(n->args, 6, 0), 0};
            MountNow(k);
            Mat m = FromAngles(p.ang);
            v.pos = p.pos + Mul(m, {k[0], -k[1], k[2]});
            v.ang = ToAngles(MulM(m, FromAngles({k[3], k[4], k[5]})));
            n->ok = true;
        }
    } else if (t == "chase") {
        // Behind the target along its view direction (yaw only), looking at it.
        void* e = RefEntity(n, 0);
        View eye;
        if (e && EyeView(e, eye)) {
            float dist = Num(n->args, 1, 120), height = Num(n->args, 2, 20);
            Vec3 back = {std::cos(Rad(eye.ang.y)), std::sin(Rad(eye.ang.y)), 0};
            v.pos = eye.pos - back * dist + Vec3{0, 0, height};
            v.ang = LookAt(v.pos, eye.pos);
            if (n->args.size() > 3) v.ang.x = Num(n->args, 3, 0);
            n->ok = true;
        }
    } else if (t == "follow") {
        // Behind the target along the way it MOVES (good for grenades and running players), looking at it.
        void* e = RefEntity(n, 0);
        View tv;
        if (e && EntityView(e, tv)) {
            float dist = Num(n->args, 1, 150), height = Num(n->args, 2, 30);
            Vec3 d = tv.pos - n->lastTarget;
            if (!n->hasState || Len(d) > 1000.f) { n->dir = {std::cos(Rad(tv.ang.y)), std::sin(Rad(tv.ang.y)), 0}; }
            else if (Len(d) > 0.5f) { Vec3 nd = d * (1.f / Len(d)); n->dir = n->dir * 0.85f + nd * 0.15f; }
            if (Len(n->dir) > 0.001f) n->dir = n->dir * (1.f / Len(n->dir));
            n->lastTarget = tv.pos;
            n->hasState = true;
            v.pos = tv.pos - n->dir * dist + Vec3{0, 0, height};
            v.ang = LookAt(v.pos, tv.pos);
            n->ok = true;
        }
    } else if (t == "orbit") {
        View c;
        if (RefView(n, 0, c)) {
            float r = Num(n->args, 1, 150), h = Num(n->args, 2, 40), speed = Num(n->args, 3, 20), start = Num(n->args, 4, 0);
            float a = Rad(start + (float)std::fmod(Now() * speed, 360.0));
            v.pos = c.pos + Vec3{std::cos(a) * r, std::sin(a) * r, h};
            v.ang = LookAt(v.pos, c.pos);
            n->ok = true;
        }
    } else if (t == "lookat") {
        View a, b;
        if (RefView(n, 0, a) && RefView(n, 1, b)) { v.pos = a.pos; v.ang = LookAt(a.pos, b.pos); n->ok = true; }
    } else if (t == "level") {
        View a;
        if (RefView(n, 0, a)) { v.pos = a.pos; v.ang = {0.f, a.ang.y, 0.f}; n->ok = true; }
    } else if (t == "mix") {
        View a, b;
        if (RefView(n, 0, a) && RefView(n, 1, b)) { v.pos = a.pos; v.ang = b.ang; n->ok = true; }
    } else if (t == "add" || t == "subtract") {
        View a, b;
        if (RefView(n, 0, a) && RefView(n, 1, b)) {
            float s = t == "add" ? 1.f : -1.f;
            v.pos = a.pos + b.pos * s;
            v.ang = {Wrap180(a.ang.x + s * b.ang.x), Wrap180(a.ang.y + s * b.ang.y), Wrap180(a.ang.z + s * b.ang.z)};
            n->ok = true;
        }
    } else if (t == "smooth") {
        View src;
        if (RefView(n, 0, src)) {
            SmoothTo(n, src, Num(n->args, 1, 0.3f), Num(n->args, 2, 0.3f), Num(n->args, 3, 0));
            v = n->state;
            n->ok = true;
        }
    }
    if (n->ok) n->view = v;
    return n->ok;
}

static bool EvalFov(Node* n) {
    if (n->frame == g_frame) return n->ok;
    Busy busy(n);
    if (!busy.ok) return false;
    n->frame = g_frame;
    n->ok = false;
    if (n->type == "value") { n->fov = Num(n->args, 0, 90); n->ok = true; }
    else if (n->type == "mountkeys") {   // the fov of the mount points (0 there = the game's: no value)
        float k[7] = {0, 0, 0, 0, 0, 0, Num(n->args, 0, 0)};
        MountNow(k);
        if (k[6] > 0) { n->fov = k[6]; n->ok = true; }
    }
    else if (n->type == "smooth") {
        Node* src = Ref(n, 0, Kind::Fov);
        if (src && EvalFov(src)) {
            double t = Now(), dt = n->hasState ? t - n->lastTime : -1;
            n->lastTime = t;
            float tau = Num(n->args, 1, 0.3f);
            if (!n->hasState || dt < 0 || dt > 1.0) { n->stateFov = src->fov; n->hasState = true; }
            else if (dt > 0) n->stateFov += (src->fov - n->stateFov) * (tau > 0 ? 1.f - (float)std::exp(-dt / tau) : 1.f);
            n->fov = n->stateFov;
            n->ok = true;
        }
    }
    if (n->ok) n->fov = std::clamp(n->fov, 1.f, 179.f);
    return n->ok;
}

// ------------------------------------------------------------------ definitions
struct TypeInfo { Kind kind; const char* type; const char* usage; int minArgs; };
static const TypeInfo kTypes[] = {
    {Kind::Handle, "index", "index <entity index>", 1},
    {Kind::Handle, "player", "player <xuid or part of the name>", 1},
    {Kind::Handle, "observed", "observed   (the player whose eyes the demo shows)", 0},
    {Kind::Handle, "weapon", "weapon <player handle>   (the gun in hand)", 1},
    {Kind::Handle, "viewmodel", "viewmodel <player handle>   (their first-person gun, if the demo shows their eyes)", 1},
    {Kind::Handle, "keptgun", "keptgun <player handle>   (the gun in hand when first looked at - stays it when holstered)", 1},
    {Kind::Handle, "owner", "owner <handle>", 1},
    {Kind::Handle, "grenade", "grenade [<player handle>]   (the newest grenade in the air)", 0},
    {Kind::VecAng, "value", "value <x> <y> <z> <pitch> <yaw> <roll>", 6},
    {Kind::VecAng, "origin", "origin <handle>   (position and angles of an entity)", 1},
    {Kind::VecAng, "eye", "eye <handle>   (eye position and view angles of a player)", 1},
    {Kind::VecAng, "attachment", "attachment <handle> <name>   (muzzle_flash, shell_eject, ... - see dt2_calc probe)", 2},
    {Kind::VecAng, "bone", "bone <player handle> <name or number>   (head, neck, shoulder_l/r, hand_l/r, pelvis, ...)", 2},
    {Kind::VecAng, "offset", "offset <vecang> <forward> <right> <up> [<pitch> <yaw> <roll>]   (relative to it)", 4},
    {Kind::VecAng, "chase", "chase <player handle> <distance> <height> [<pitch>]   (behind where they look)", 1},
    {Kind::VecAng, "follow", "follow <handle> <distance> <height>   (behind where it moves: grenades, runners)", 1},
    {Kind::VecAng, "orbit", "orbit <vecang> <radius> <height> <degrees per second> [<start yaw>]", 1},
    {Kind::VecAng, "lookat", "lookat <from vecang> <target vecang>   (position of the first, looking at the second)", 2},
    {Kind::VecAng, "mix", "mix <position vecang> <angles vecang>", 2},
    {Kind::VecAng, "level", "level <vecang>   (same, but only its yaw: no tilt or roll)", 1},
    {Kind::VecAng, "add", "add <a> <b>", 2},
    {Kind::VecAng, "subtract", "subtract <a> <b>", 2},
    {Kind::VecAng, "smooth", "smooth <vecang> <position time> <angle time> [<max speed>]   (seconds; units/s)", 1},
    {Kind::VecAng, "mountkeys", "mountkeys <vecang> <forward> <right> <up> <pitch> <yaw> <roll>   (offset that follows the Attach tab's points)", 7},
    {Kind::Fov, "value", "value <degrees>", 1},
    {Kind::Fov, "mountkeys", "mountkeys [<degrees>]   (the fov of the Attach tab's points)", 0},
    {Kind::Fov, "smooth", "smooth <fov calc> <time>", 1},
};
static const char* KindName(Kind k) { return k == Kind::Handle ? "handle" : k == Kind::VecAng ? "vecang" : "fov"; }
static bool ParseKind(const std::string& s, Kind& k) {
    std::string l = Lower(s);
    if (l == "handle") k = Kind::Handle;
    else if (l == "vecang" || l == "view") k = Kind::VecAng;
    else if (l == "fov") k = Kind::Fov;
    else return false;
    return true;
}
static std::string Definition(const Node& n) {
    std::string s = std::string(KindName(n.kind)) + " add " + n.name + " " + n.type;
    for (const std::string& a : n.args) s += " " + (a.find(' ') != std::string::npos ? "\"" + a + "\"" : a);
    return s;
}

std::string Help() {
    std::string s =
        "dt2_calc - named values computed from the demo every frame (like mirv_calc):\n"
        "  dt2_calc <handle|vecang|fov> add <name> <type> <arguments...>\n"
        "  dt2_calc remove <name>  |  dt2_calc list  |  dt2_calc print <name>  |  dt2_calc clear\n"
        "  dt2_calc probe <handle calc>   (lists the attachments and bones that entity has, with positions)\n"
        "types:\n";
    for (const TypeInfo& t : kTypes) s += std::string("  ") + KindName(t.kind) + " " + t.usage + "\n";
    s += "dt2_cam - the DemoTools2 camera (recorded by HLAE mirv_streams):\n"
         "  dt2_cam source <vecang calc>  |  dt2_cam fov <fov calc | degrees | game>  |  dt2_cam enabled 0|1\n"
         "  dt2_cam blend <seconds>  |  dt2_cam thirdperson 0|1  |  dt2_cam hideviewmodel 0|1  |  dt2_cam   (status)\n"
         "dt2_preset save|load|delete <name>  |  dt2_preset list\n"
         "example - smooth third-person chase of a player:\n"
         "  dt2_calc handle add p player s1mple\n"
         "  dt2_calc vecang add view chase p 110 25\n"
         "  dt2_calc vecang add cam smooth view 0.25 0.15\n"
         "  dt2_cam source cam; dt2_cam enabled 1";
    return s;
}

static std::string ValueText(Node& n) {
    char buf[200];
    if (!n.ok) return "-";
    if (n.kind == Kind::Handle) {
        const char* dn = Game::DesignerName(n.ent);
        std::snprintf(buf, sizeof(buf), "%s #%u", dn ? dn : "?", Game::HandleOf(n.ent) & 0x7fff);
    } else if (n.kind == Kind::VecAng) {
        std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f | %.1f %.1f %.1f", n.view.pos.x, n.view.pos.y, n.view.pos.z, n.view.ang.x, n.view.ang.y,
                      n.view.ang.z);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", n.fov);
    }
    return buf;
}

static void Evaluate(Node& n) {
    if (n.kind == Kind::Handle) EvalHandle(&n);
    else if (n.kind == Kind::VecAng) EvalView(&n);
    else EvalFov(&n);
}

// ------------------------------------------------------------------ commands
void CmdCalc(const std::vector<std::string>& a) {
    std::lock_guard<std::mutex> g(lock);
    if (a.size() < 2) { Console::Print("%s", Help().c_str()); return; }
    std::string sub = Lower(a[1]);
    if (sub == "list") {
        if (g_nodes.empty()) { Console::Print("no calcs. %s", "Type dt2_calc for help."); return; }
        for (auto& kv : g_nodes) Console::Print("%-14s %s   = %s", kv.second->name.c_str(), Definition(*kv.second).c_str(), ValueText(*kv.second).c_str());
        return;
    }
    if (sub == "clear") { g_nodes.clear(); Console::Print("all calcs removed"); return; }
    if (sub == "remove" && a.size() >= 3) {
        Console::Print(g_nodes.erase(Lower(a[2])) ? "removed %s" : "no calc named %s", a[2].c_str());
        return;
    }
    if (sub == "probe" && a.size() >= 3) {
        Node* n = Find(a[2]);
        if (!n || n->kind != Kind::Handle) { Console::Print("no handle calc named %s", a[2].c_str()); return; }
        ++g_frame;
        if (!EvalHandle(n)) { Console::Print("%s has no value right now", n->name.c_str()); return; }
        const char* mn = Game::ModelName(n->ent);
        Console::Print("%s = %s, model %s", n->name.c_str(), ValueText(*n).c_str(), mn ? mn : "?");
        int found = 0;
        float p[3], q[4];
        for (const char* at : kAttachments)
            if (Game::Attachment(n->ent, at, p, q)) { ++found; Console::Print("  attachment %-14s %.1f %.1f %.1f", at, p[0], p[1], p[2]); }
        if (!found) Console::Print("  no known attachments (try other names with: dt2_calc vecang add x attachment %s <name>)", n->name.c_str());
        if (IsPawn(n->ent))
            for (auto& b : kBones)
                if (std::strncmp(b.name, "shoulder", 8) && std::strncmp(b.name, "elbow", 5) && std::strncmp(b.name, "knee", 4) &&
                    Game::Bone(n->ent, b.index, p, q))
                    Console::Print("  bone %-14s (%2d) %.1f %.1f %.1f", b.name, b.index, p[0], p[1], p[2]);
        return;
    }
    if (sub == "print" && a.size() >= 3) {
        Node* n = Find(a[2]);
        if (!n) { Console::Print("no calc named %s", a[2].c_str()); return; }
        ++g_frame;
        Evaluate(*n);
        Console::Print("%s = %s", n->name.c_str(), ValueText(*n).c_str());
        return;
    }
    Kind kind;
    if (!ParseKind(a[1], kind)) { Console::Print("unknown: %s. %s", a[1].c_str(), "Type dt2_calc for help."); return; }
    if (a.size() >= 4 && Lower(a[2]) == "remove") { g_nodes.erase(Lower(a[3])); Console::Print("removed %s", a[3].c_str()); return; }
    if (a.size() < 5 || Lower(a[2]) != "add") {
        Console::Print("usage: dt2_calc %s add <name> <type> ...", KindName(kind));
        for (const TypeInfo& t : kTypes) if (t.kind == kind) Console::Print("  %s", t.usage);
        return;
    }
    const std::string name = a[3], type = Lower(a[4]);
    const TypeInfo* info = nullptr;
    for (const TypeInfo& t : kTypes) if (t.kind == kind && type == t.type) info = &t;
    if (!info) {
        Console::Print("unknown %s type '%s'. Types:", KindName(kind), a[4].c_str());
        for (const TypeInfo& t : kTypes) if (t.kind == kind) Console::Print("  %s", t.usage);
        return;
    }
    if ((int)a.size() - 5 < info->minArgs) { Console::Print("usage: dt2_calc %s add <name> %s", KindName(kind), info->usage); return; }
    if (name.empty() || std::isdigit((unsigned char)name[0])) { Console::Print("give the calc a name that starts with a letter"); return; }
    auto n = std::make_unique<Node>();
    n->name = name;
    n->kind = kind;
    n->type = type;
    n->args.assign(a.begin() + 5, a.end());
    bool replaced = g_nodes.count(Lower(name)) > 0;
    g_nodes[Lower(name)] = std::move(n);
    if (!g_quiet) Console::Print("%s %s", replaced ? "replaced" : "added", Definition(*g_nodes[Lower(name)]).c_str());
}

// Third person: CS2's spectator modes are 2 = in eye, 3 = chase (OBS_MODE_ enum; the demo UI's "Chase Cam" button
// sends "spec_mode 3"). In chase mode the watched player's own body and world gun are drawn and there is no
// first-person gun, which is what a camera on a muzzle or a shoulder needs. While the camera is on we check once a
// second: if any player still has a first-person arms model, the demo is back in first person, so switch again.
static bool g_weSetChase = false;
static double g_chaseCheckAt = 0;
static int g_chaseTries = 0;   // in a row; stops after 3 so a demo that has no chase cam isn't sent the command forever
static bool AnyFirstPerson() {
    for (const PlayerRef& p : Players())
        if (p.pawn && Game::FromHandleExact(Get<uint32_t>(p.pawn, F.hudArms))) return true;
    return false;
}
static void KeepThirdPerson(bool want) {
    double now = RealSeconds();
    if (want) {
        if (now < g_chaseCheckAt) return;
        g_chaseCheckAt = now + 1.0;
        if (!AnyFirstPerson()) { g_chaseTries = 0; return; }
        if (g_chaseTries >= 3) return;
        ++g_chaseTries;
        Game::ExecuteCommand("spec_mode 3");
        g_weSetChase = true;
    } else if (g_weSetChase || g_chaseTries) {
        g_chaseTries = 0;
        if (!g_weSetChase) return;
        g_weSetChase = false;
        g_chaseCheckAt = 0;
        Game::ExecuteCommand("spec_mode 2");
    }
}

uint64_t ObservedXuid() {
    std::lock_guard<std::mutex> g(lock);
    for (const PlayerRef& p : Players())
        if (p.pawn && Game::FromHandleExact(Get<uint32_t>(p.pawn, F.hudArms))) return p.xuid;
    // In chase cam nobody has first-person arms: use the last player an "observed" calc saw.
    for (auto& kv : g_nodes)
        if (kv.second->kind == Kind::Handle && kv.second->type == "observed" && kv.second->lastHandle)
            if (void* pawn = Game::FromHandleExact(kv.second->lastHandle))
                if (void* ctrl = Game::FromHandle(Get<uint32_t>(pawn, O.pawn_hController))) return Get<uint64_t>(ctrl, F.steamId);
    return 0;
}

static void SetViewmodel(bool show) {
    static int state = -1;   // what we last set (-1: never)
    if (state == (show ? 1 : 0)) return;
    if (state == -1 && show) return;   // nothing to undo
    state = show ? 1 : 0;
    Game::ExecuteCommand(show ? "r_drawviewmodel 1" : "r_drawviewmodel 0");
}

void CmdCam(const std::vector<std::string>& a) {
    std::lock_guard<std::mutex> g(lock);
    if (a.size() < 2) {
        Console::Print("dt2_cam: %s, source '%s', fov '%s', blend %.2f s, third person %d, hide first-person gun %d\n  %s", cam.enabled ? "ON" : "off",
                       cam.source.c_str(), cam.fov.empty() ? "game" : cam.fov.c_str(), cam.blend, cam.thirdPerson ? 1 : 0, cam.hideViewmodel ? 1 : 0,
                       cam.status.c_str());
        return;
    }
    std::string sub = Lower(a[1]);
    std::string val = a.size() > 2 ? a[2] : "";
    if (sub == "source") {
        if (Lower(val) == "none") val.clear();
        if (!val.empty()) { Node* n = Find(val); if (!n || n->kind != Kind::VecAng) { Console::Print("no vecang calc named '%s'", val.c_str()); return; } val = n->name; }
        cam.source = val;
    } else if (sub == "fov") {
        if (Lower(val) == "game" || Lower(val) == "none") val.clear();
        cam.fov = val;
    } else if (sub == "enabled" || sub == "enable") {
        cam.enabled = std::atoi(val.c_str()) != 0;
    } else if (sub == "blend") {
        cam.blend = std::max(0.f, (float)std::atof(val.c_str()));
    } else if (sub == "hideviewmodel") {
        cam.hideViewmodel = std::atoi(val.c_str()) != 0;
    } else if (sub == "thirdperson") {
        cam.thirdPerson = std::atoi(val.c_str()) != 0;
    } else {
        Console::Print("usage: dt2_cam [source <vecang calc> | fov <fov calc|degrees|game> | enabled 0|1 | blend <seconds> | thirdperson 0|1 | hideviewmodel 0|1]");
        return;
    }
    if (!g_quiet) Console::Print("dt2_cam %s %s", sub.c_str(), a.size() > 2 ? a[2].c_str() : "");
}

// ------------------------------------------------------------------ presets
static std::string PresetFolder() { return DllFolder() + "DemoTools2-presets\\"; }
static bool SafeName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ')) return false;
    return true;
}
std::vector<std::string> PresetNames() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((PresetFolder() + "*.cfg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        out.push_back(n.substr(0, n.size() - 4));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
bool SavePreset(const std::string& name) {
    if (!SafeName(name)) return false;
    CreateDirectoryA(PresetFolder().c_str(), nullptr);
    std::lock_guard<std::mutex> g(lock);
    std::ofstream f(PresetFolder() + name + ".cfg");
    if (!f) return false;
    f << "// DemoTools2 camera preset \"" << name << "\" - load with: dt2_preset load " << name << " (or exec this file)\n";
    f << "dt2_calc clear\n";
    // Handles first, then fov, then vecangs that others use (sorted so references come first where possible).
    std::vector<Node*> order;
    for (Kind k : {Kind::Handle, Kind::Fov, Kind::VecAng})
        for (auto& kv : g_nodes) if (kv.second->kind == k) order.push_back(kv.second.get());
    for (Node* n : order) f << "dt2_calc " << Definition(*n) << "\n";
    f << "dt2_cam source " << (cam.source.empty() ? "none" : cam.source) << "\n";
    f << "dt2_cam fov " << (cam.fov.empty() ? "game" : cam.fov) << "\n";
    f << "dt2_cam blend " << cam.blend << "\n";
    f << "dt2_cam hideviewmodel " << (cam.hideViewmodel ? 1 : 0) << "\n";
    f << "dt2_cam thirdperson " << (cam.thirdPerson ? 1 : 0) << "\n";
    f << "dt2_cam enabled " << (cam.enabled ? 1 : 0) << "\n";
    return true;
}
bool LoadPreset(const std::string& name) {
    if (!SafeName(name)) return false;
    std::ifstream f(PresetFolder() + name + ".cfg");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.rfind("//", 0) == 0 || line[0] == '#') continue;
        // "a; b" on one line, as in .cfg files
        size_t start = 0;
        while (start < line.size()) {
            size_t semi = line.find(';', start);
            std::string part = line.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            if (part.find_first_not_of(' ') != std::string::npos) Console::Run(part);
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
    }
    return true;
}
bool DeletePreset(const std::string& name) { return SafeName(name) && DeleteFileA((PresetFolder() + name + ".cfg").c_str()); }

void CmdPreset(const std::vector<std::string>& a) {
    std::string sub = a.size() > 1 ? Lower(a[1]) : "";
    std::string name = a.size() > 2 ? a[2] : "";
    for (size_t i = 3; i < a.size(); ++i) name += " " + a[i];
    if (sub == "list" || sub.empty()) {
        auto names = PresetNames();
        Console::Print("presets (%u) in %s:", (unsigned)names.size(), PresetFolder().c_str());
        for (auto& n : names) Console::Print("  %s", n.c_str());
        if (sub.empty()) Console::Print("usage: dt2_preset save|load|delete <name>  |  dt2_preset list");
    } else if (sub == "save") {
        Console::Print(SavePreset(name) ? "saved preset '%s'" : "couldn't save '%s' (use letters, digits, - and _)", name.c_str());
    } else if (sub == "load") {
        Console::Print(LoadPreset(name) ? "loaded preset '%s'" : "no preset named '%s'", name.c_str());
    } else if (sub == "delete") {
        Console::Print(DeletePreset(name) ? "deleted preset '%s'" : "no preset named '%s'", name.c_str());
    } else {
        Console::Print("usage: dt2_preset save|load|delete <name>  |  dt2_preset list");
    }
}

// ------------------------------------------------------------------ the view
static std::vector<Preview> g_preview;
static bool g_wantOverride = false;
static View g_out;
static float g_outFov = 0;
static std::string g_lastSource;
static bool g_blending = false;
static View g_blendFrom;
static double g_blendStart = 0;
static bool g_hadOutput = false;
static bool g_campathOn = false;   // the campath drives the view this frame

static float SmoothStep(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3 - 2 * x); }

// Runs once per frame while a demo plays (from the view hook): evaluates every calc, prepares the camera.
static float ViewValueInner();
// The view hook: CS2 has worked out the camera for this frame by now (what our hook reads from the view set-up).
static float ViewValue() {
    Pov::OnViewSetup();
    const float v = ViewValueInner();
    Pov::OnViewDone();
    Record::OnFrame();
    return v;
}
static float ViewValueInner() {
    std::unique_lock<std::mutex> g(lock, std::try_to_lock);
    if (!g.owns_lock()) return g_wantOverride ? 1.f : Game::OriginalDemoViewOverrideValue();   // the window is editing: keep last frame's result
    static const bool off = CrashedLastTime("camera");
    if (off) { cam.status = "off this session (it crashed last time)"; return Game::OriginalDemoViewOverrideValue(); }
    Step step("camera calcs");
    FirstRunBegin("camera");
    ++g_frame;
    g_wantOverride = false;
    std::vector<Preview> pv;
    pv.reserve(g_nodes.size());
    for (auto& kv : g_nodes) {
        Node& n = *kv.second;
        Evaluate(n);
        pv.push_back({n.name, KindName(n.kind), Definition(n), n.ok, ValueText(n)});
    }
    g_preview.swap(pv);

    cam.active = false;
    // A campath that is playing (or previewing) has the camera; the calc camera waits.
    {
        float aim[3];
        bool haveAim = false;
        uint64_t aimXuid = 0;
        int aimPart = 0;
        {
            std::lock_guard<std::mutex> cg(Campath::lock);
            if (Campath::set.aim == 1) { aimXuid = Campath::set.aimXuid; aimPart = Campath::set.aimPart; }
        }
        if (aimXuid)
            for (const PlayerRef& p : Players())
                if (p.xuid == aimXuid && p.pawn) {
                    View v;
                    if (EyeView(p.pawn, v)) {
                        const float drop = aimPart == 1 ? 22.f : aimPart == 2 ? 62.f : 0.f;   // chest / feet below the eyes
                        aim[0] = v.pos.x; aim[1] = v.pos.y; aim[2] = v.pos.z - drop;
                        haveAim = true;
                    }
                    break;
                }
        float cp[3], ca[3], cf = 90.f;
        g_campathOn = Campath::Evaluate(Game::CurTime(), haveAim ? aim : nullptr, cp, ca, cf);
        if (g_campathOn) {
            g_out.pos = {cp[0], cp[1], cp[2]};
            g_out.ang = {ca[0], ca[1], ca[2]};
            g_outFov = cf;
            g_wantOverride = true;
        }
    }
    uint32_t povPawn = 0;
    bool povCam = false;
    if (!g_campathOn && Pov::CameraPawn(povPawn))
        if (void* pawn = Game::FromHandleExact(povPawn)) {
            View v;
            if (EyeView(pawn, v)) {
                g_out = v;
                g_outFov = 0;
                g_wantOverride = true;
                povCam = true;
            }
        }
    if (g_campathOn) {
        cam.status = "the campath has the camera (Cinematics tab)";
    } else if (povCam) {
        cam.status = "POV test 2 has the camera";
    } else if (cam.enabled) {
        Node* src = cam.source.empty() ? nullptr : Find(cam.source);
        if (!src) cam.status = cam.source.empty() ? "no source - choose a vecang calc" : "source calc '" + cam.source + "' doesn't exist";
        else if (src->kind != Kind::VecAng) cam.status = "source must be a vecang calc";
        else if (!EvalView(src)) {
            // Keep the camera where it was instead of handing the view back to the demo's own (chase) camera, which
            // looks like the camera suddenly "follows" the player.
            if (g_hadOutput) {
                g_wantOverride = true;
                cam.active = true;
                cam.status = "HOLDING last position: '" + cam.source + "' has no value right now (part not found, other gun, player dead?)";
            } else {
                cam.status = "waiting: '" + cam.source + "' has no value right now (part not found, player dead or not in the demo?)";
            }
        } else {
            View v = src->view;
            double t = Now();
            if (g_lastSource != cam.source) {
                if (g_hadOutput && cam.blend > 0) { g_blending = true; g_blendFrom = g_out; g_blendStart = t; }
                g_lastSource = cam.source;
            }
            if (g_blending) {
                float k = SmoothStep((float)((t - g_blendStart) / cam.blend));
                if (k >= 1.f || t < g_blendStart) g_blending = false;
                else {
                    v.pos = g_blendFrom.pos + (v.pos - g_blendFrom.pos) * k;
                    v.ang = {g_blendFrom.ang.x + Wrap180(v.ang.x - g_blendFrom.ang.x) * k, Wrap180(g_blendFrom.ang.y + Wrap180(v.ang.y - g_blendFrom.ang.y) * k),
                             g_blendFrom.ang.z + Wrap180(v.ang.z - g_blendFrom.ang.z) * k};
                }
            }
            g_out = v;
            g_outFov = 0;
            if (!cam.fov.empty()) {
                char* end = nullptr;
                float f = std::strtof(cam.fov.c_str(), &end);
                if (end && end != cam.fov.c_str()) g_outFov = std::clamp(f, 1.f, 179.f);
                else if (Node* fn = Find(cam.fov)) { if (fn->kind == Kind::Fov && EvalFov(fn)) g_outFov = fn->fov; }
            }
            g_wantOverride = true;
            g_hadOutput = true;
            cam.active = true;
            cam.out = g_out;
            cam.outFov = g_outFov;
            cam.status = "on";
        }
    } else {
        cam.status = "off";
        g_lastSource.clear();
        g_hadOutput = false;
        g_blending = false;
    }
    if (!g_campathOn) {
        if (cam.hideViewmodel) SetViewmodel(!cam.active);
        else SetViewmodel(true);
    }
    KeepThirdPerson(!g_campathOn && cam.enabled && cam.thirdPerson && cam.active);
    Campath::AfterFrame(g_campathOn);
    FirstRunEnd("camera");
    return g_wantOverride ? 1.f : Game::OriginalDemoViewOverrideValue();
}

static void ViewApply(void* setup) {
    if (!g_wantOverride) { Game::OriginalCalcDemoViewOverride(setup); return; }
    uint8_t* s = (uint8_t*)setup;
    if (!Readable(s + Game::viewOriginOff, 0x18 + 12)) return;
    float* o = (float*)(s + Game::viewOriginOff);
    float* an = (float*)(s + Game::viewAnglesOff);
    o[0] = g_out.pos.x; o[1] = g_out.pos.y; o[2] = g_out.pos.z;
    an[0] = g_out.ang.x; an[1] = g_out.ang.y; an[2] = g_out.ang.z;
    if (g_outFov > 0) *(float*)(s + Game::viewFovOff) = g_outFov;
}

std::vector<Preview> Snapshot() {
    std::lock_guard<std::mutex> g(lock);
    return g_preview;
}

std::vector<std::string> Names(Kind kind) {
    std::lock_guard<std::mutex> g(lock);
    std::vector<std::string> out;
    for (auto& kv : g_nodes) if (kv.second->kind == kind) out.push_back(kv.second->name);
    return out;
}

void Init() {
    ReadOffsets();
    Console::Add("dt2_calc", "DemoTools2 calcs (like mirv_calc) - type dt2_calc for help", &CmdCalc);
    Console::Add("dt2_cam", "DemoTools2 camera - type dt2_cam for status", &CmdCam);
    Console::Add("dt2_preset", "DemoTools2 camera presets: save|load|delete <name>, list", &CmdPreset);
}

// On the game thread (first FrameStageNotify): register the commands and rewrite the view code while the game
// is certainly not inside it.
void OnGameThread() {
    static bool done = false;
    if (done) return;
    done = true;
    if (CrashedLastTime("console-register")) {
        Log("console commands are off this session (registering them crashed last time) - use the command line in the window");
        SetFeature("console", "Console commands", Feat_Off, "Crashed last time - use the command line in the Camera tab.");
    } else if (Console::Install()) SetFeature("console", "Console commands", Feat_Ok);
    else SetFeature("console", "Console commands", Feat_Off, "Not found in this CS2 version - use the command line in the Camera tab.");
    if (CrashedLastTime("camera")) {
        cam.status = "the camera crashed last time - off for this session";
        SetFeature("camera", "Camera", Feat_Off, "Crashed last time - off for this session.");
    } else if (!Game::InstallViewHook(&ViewValue, &ViewApply)) {
        cam.status = "the camera can't take over the view in this CS2 version (see DemoTools2.log)";
        SetFeature("camera", "Camera", Feat_Off, "The view code wasn't found (CS2 or HLAE update). Skins are not affected.");
    } else SetFeature("camera", "Camera", Feat_Ok);
}

}   // namespace Calc
