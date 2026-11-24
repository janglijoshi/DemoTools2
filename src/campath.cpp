// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// Campaths (see campath.h): keyframes, smooth interpolation, timing tools, HLAE campath files, and mirv_pov.
#include "campath.h"
#include "console.h"
#include "game.h"
#include "util.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>

namespace Campath {

std::mutex lock;
std::vector<Key> keys;
Settings set;
Pov pov;
int selected = -1;
Sequence seq;

static std::deque<std::vector<Key>> g_undo;   // newest at the back
static Live g_live;
static bool g_previewing = false;
static double g_previewStart = 0, g_previewFrom = 0;
static bool g_frozen = false, g_frozenDone = false;   // Freeze path record (see campath.h)
static double g_frozenFrom = 0, g_frozenStep = 0, g_frozenWall = 0;
static long long g_frozenFrames = 0;
static std::vector<std::string> g_commands;   // run on the game thread (AfterFrame)

static const double kMinGap = 0.02;   // two keys never closer in time than this

// ------------------------------------------------------------------ helpers (lock held)
static float Wrap180(float a) { a = std::fmod(a + 180.f, 360.f); if (a < 0) a += 360.f; return a - 180.f; }
static void Sort() { std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; }); }
static void PushUndo() {
    g_undo.push_back(keys);
    while (g_undo.size() > 64) g_undo.pop_front();
}
// No two keys at the same time (the spline needs t to grow)
static void Spread() {
    Sort();
    for (size_t i = 1; i < keys.size(); ++i)
        if (keys[i].t < keys[i - 1].t + kMinGap) keys[i].t = keys[i - 1].t + kMinGap;
}

// ------------------------------------------------------------------ interpolation
// Each channel is a cubic Hermite curve over time. Inner tangents: Fritsch-Butland (see Tangent: smooth, follows
// uneven timing, no overshoot). End tangents: zero with "ease ends" (camera starts and stops gently),
// otherwise the slope of the first / last segment. Angles are unwrapped first so yaw never spins the long way round.
// A key marked "cut" holds its view until the next key's time, then the camera jumps there: each run of keys between
// cuts is its own shot, with its own start and end (eased if set).
struct Channels {
    std::vector<double> t;
    std::vector<std::array<double, 7>> v;   // x y z pitch yaw roll fov
    std::vector<char> cut;                  // cut[i]: hold key i, then jump to key i+1
};
static Channels Build(const std::vector<Key>& ks = keys) {
    Channels c;
    c.t.reserve(ks.size());
    c.v.reserve(ks.size());
    std::array<double, 7> prev{};
    for (size_t i = 0; i < ks.size(); ++i) {
        const Key& k = ks[i];
        std::array<double, 7> v{k.pos[0], k.pos[1], k.pos[2], k.ang[0], k.ang[1], k.ang[2], k.fov};
        if (i) for (int a = 3; a < 6; ++a) v[a] = prev[a] + Wrap180((float)(v[a] - prev[a]));
        c.t.push_back(k.t);
        c.v.push_back(v);
        c.cut.push_back(k.cut ? 1 : 0);
        prev = v;
    }
    return c;
}
static double Tangent(const Channels& c, size_t i, int ch, bool ease) {
    const size_t n = c.t.size();
    const bool first = i == 0 || c.cut[i - 1], last = i == n - 1 || c.cut[i];   // a shot's first / last key
    if (first && last) return 0.0;
    if (first) return ease ? 0.0 : (c.v[i + 1][ch] - c.v[i][ch]) / (c.t[i + 1] - c.t[i]);
    if (last) return ease ? 0.0 : (c.v[i][ch] - c.v[i - 1][ch]) / (c.t[i] - c.t[i - 1]);
    const double h0 = c.t[i] - c.t[i - 1], h1 = c.t[i + 1] - c.t[i];
    const double d0 = (c.v[i][ch] - c.v[i - 1][ch]) / h0, d1 = (c.v[i + 1][ch] - c.v[i][ch]) / h1;
    // Fritsch-Butland: a weighted harmonic mean of the two slopes, 0 where the channel turns around. Smooth, and it
    // never swings past a key (an even average does with uneven timing: a quick key then a slow one throws the
    // camera far out of the way).
    if (d0 * d1 <= 0) return 0.0;
    return 3.0 * (h0 + h1) / ((2.0 * h1 + h0) / d0 + (h1 + 2.0 * h0) / d1);
}
static double Channel(const Channels& c, size_t i, double u, int ch, int interp) {
    const double a = c.v[i][ch], b = c.v[i + 1][ch];
    if (c.cut[i]) return a;   // holding until the cut
    if (interp == Linear) return a + (b - a) * u;
    const double h = c.t[i + 1] - c.t[i];
    const double m0 = Tangent(c, i, ch, set.easeEnds) * h, m1 = Tangent(c, i + 1, ch, set.easeEnds) * h;
    const double u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b + (u3 - u2) * m1;
}
// t inside [first, last] (clamped)
static void EvalAt(const Channels& c, double t, float pos[3], float ang[3], float& fov) {
    const size_t n = c.t.size();
    if (t <= c.t[0]) t = c.t[0];
    if (t >= c.t[n - 1]) t = c.t[n - 1];
    size_t i = std::upper_bound(c.t.begin(), c.t.end(), t) - c.t.begin();
    i = i == 0 ? 0 : std::min(i - 1, n - 2);
    const double u = (t - c.t[i]) / (c.t[i + 1] - c.t[i]);
    for (int k = 0; k < 3; ++k) pos[k] = (float)Channel(c, i, u, k, set.posInterp);
    ang[0] = (float)std::clamp(Channel(c, i, u, 3, set.angInterp), -89.9, 89.9);
    ang[1] = Wrap180((float)Channel(c, i, u, 4, set.angInterp));
    ang[2] = Wrap180((float)Channel(c, i, u, 5, set.angInterp));
    fov = (float)std::clamp(Channel(c, i, u, 6, set.fovInterp), 1.0, 170.0);
}
static void LookAt(const float from[3], const float to[3], float ang[3]) {
    const double dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const double xy = std::sqrt(dx * dx + dy * dy);
    if (xy < 0.01 && std::fabs(dz) < 0.01) return;
    ang[0] = (float)(std::atan2(-dz, xy) * 180.0 / 3.14159265358979);
    ang[1] = (float)(std::atan2(dy, dx) * 180.0 / 3.14159265358979);
}

double Duration() {
    std::lock_guard<std::mutex> g(lock);
    return keys.size() >= 2 ? keys.back().t - keys.front().t : 0.0;
}

// ------------------------------------------------------------------ game thread
static bool g_lastOn = false;   // last frame's result, reused while the window holds the lock (no one-frame jumps)
static float g_lastPos[3], g_lastAng[3], g_lastFov = 90.f;
static bool Remember(bool on, const float pos[3], const float ang[3], float fov) {
    g_lastOn = on;
    if (on) { std::memcpy(g_lastPos, pos, 12); std::memcpy(g_lastAng, ang, 12); g_lastFov = fov; }
    return on;
}
bool Evaluate(double now, const float* aimAt, float pos[3], float ang[3], float& fov) {
    std::unique_lock<std::mutex> g(lock, std::try_to_lock);
    if (!g.owns_lock()) {
        if (!g_lastOn) return false;
        std::memcpy(pos, g_lastPos, 12); std::memcpy(ang, g_lastAng, 12); fov = g_lastFov;
        return true;
    }
    g_lastOn = false;
    g_live.now = now;
    g_live.playing = false;
    g_live.previewing = g_previewing;
    g_live.camKnown = Game::ViewPosition(g_live.camPos);
    if (keys.size() < 2) {
        g_previewing = false;
        g_live.note = keys.empty() ? "No keys yet: move the camera, then Add key." : "One key: add at least one more.";
        return false;
    }
    double t;
    if (g_frozen) {   // the demo stands still; the path runs on its own clock
        t = g_frozenFrom + (g_frozenStep > 0 ? g_frozenFrames * g_frozenStep : (NowMs() / 1000.0) - g_frozenWall);
        if (t >= keys.back().t) { t = keys.back().t; g_frozenDone = true; }
    } else if (g_previewing) {
        t = g_previewFrom + ((NowMs() / 1000.0) - g_previewStart);
        if (t > keys.back().t) { g_previewing = false; g_live.previewing = false; g_live.note = "Preview finished."; return false; }
    } else {
        if (!set.enabled) { g_live.note = "Path off."; return false; }
        t = now;
        if (t < keys.front().t || t > keys.back().t) {
            if (!set.hold) {
                g_live.note = t < keys.front().t ? "Waiting: the demo hasn't reached the path yet." : "The demo is past the end of the path.";
                g_live.pathTime = t - keys.front().t;
                return false;
            }
            t = std::clamp(t, keys.front().t, keys.back().t);
        }
    }
    const Channels c = Build();
    EvalAt(c, t, pos, ang, fov);
    if (set.aim == 1 && aimAt) LookAt(pos, aimAt, ang);
    g_live.playing = true;
    g_live.pathTime = t - keys.front().t;
    g_live.note = g_frozen ? "Freeze path record." : g_previewing ? "Preview (real time)." : "Playing.";
    return Remember(true, pos, ang, fov);
}

void AfterFrame(bool playing) {
    std::vector<std::string> cmds;
    bool hideHud, hideVm;
    {
        std::lock_guard<std::mutex> g(lock);
        cmds.swap(g_commands);
        if (g_frozen && g_frozenStep > 0) ++g_frozenFrames;   // one frame drawn: the frozen path moves one step
        hideHud = set.hideHud;
        hideVm = set.hideViewmodel;
    }
    for (const std::string& c : cmds) Game::ExecuteCommand(c.c_str());
    // HUD and first-person gun: only what we changed is put back.
    static bool hudHidden = false, vmHidden = false;
    const bool wantHud = playing && hideHud, wantVm = playing && hideVm;
    if (wantHud != hudHidden) { Game::ExecuteCommand(wantHud ? "cl_drawhud 0" : "cl_drawhud 1"); hudHidden = wantHud; }
    if (wantVm != vmHidden) { Game::ExecuteCommand(wantVm ? "r_drawviewmodel 0" : "r_drawviewmodel 1"); vmHidden = wantVm; }
}

Live Status() {
    std::lock_guard<std::mutex> g(lock);
    return g_live;
}

// ------------------------------------------------------------------ editing
static bool CurrentView(Key& k, std::string* why) {
    if (!Game::ViewPosition(k.pos, k.ang)) { if (why) *why = "The camera isn't known yet - play the demo for a moment."; return false; }
    const float f = Game::ViewFov();
    k.fov = f > 1.f ? f : 90.f;
    k.ang[2] = Wrap180(k.ang[2]);
    return true;
}

bool AddKeyHere(std::string* why) {
    Key k;
    if (!CurrentView(k, why)) return false;
    k.t = Game::CurTime();
    k.tick = Game::DemoTick();
    if (k.t <= 0) { if (why) *why = "The demo time isn't known."; return false; }
    std::lock_guard<std::mutex> g(lock);
    PushUndo();
    // Same moment as an existing key (demo paused): replace it rather than stacking two keys on one time.
    for (size_t i = 0; i < keys.size(); ++i)
        if (std::fabs(keys[i].t - k.t) < 0.001) { keys[i] = k; selected = (int)i; return true; }
    keys.push_back(k);
    Sort();
    for (size_t i = 0; i < keys.size(); ++i) if (keys[i].t == k.t) selected = (int)i;
    return true;
}

bool SetKeyToView(int index) {
    Key v;
    if (!CurrentView(v, nullptr)) return false;
    std::lock_guard<std::mutex> g(lock);
    if (index < 0 || index >= (int)keys.size()) return false;
    PushUndo();
    std::memcpy(keys[index].pos, v.pos, sizeof v.pos);
    std::memcpy(keys[index].ang, v.ang, sizeof v.ang);
    keys[index].fov = v.fov;
    return true;
}

void RemoveKey(int index) {
    std::lock_guard<std::mutex> g(lock);
    if (index < 0 || index >= (int)keys.size()) return;
    PushUndo();
    keys.erase(keys.begin() + index);
    if (selected >= (int)keys.size()) selected = (int)keys.size() - 1;
}

void Clear() {
    std::lock_guard<std::mutex> g(lock);
    if (keys.empty()) return;
    PushUndo();
    keys.clear();
    selected = -1;
    g_previewing = false;
}

bool Undo() {
    std::lock_guard<std::mutex> g(lock);
    if (g_undo.empty()) return false;
    keys = g_undo.back();
    g_undo.pop_back();
    if (selected >= (int)keys.size()) selected = (int)keys.size() - 1;
    return true;
}

void SaveUndoStep() {
    std::lock_guard<std::mutex> g(lock);
    PushUndo();
}

void UpdateKey(int index, const Key& k) {
    std::lock_guard<std::mutex> g(lock);
    if (index < 0 || index >= (int)keys.size()) return;
    keys[index] = k;
    const double t = k.t;
    Spread();
    for (size_t i = 0; i < keys.size(); ++i) if (std::fabs(keys[i].t - t) < 1e-9) selected = (int)i;
}

void EvenSpeed() {
    std::lock_guard<std::mutex> g(lock);
    if (keys.size() < 3) return;
    PushUndo();
    // The curve depends on the timing, so measure, re-time, and measure again. Holds before a cut keep their time;
    // the moving segments share the rest by length.
    for (int pass = 0; pass < 4; ++pass) {
        const Channels c = Build();
        std::vector<double> len(keys.size() - 1, 0.0);
        double sum = 0;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            float prev[3], p[3], a[3], f;
            EvalAt(c, c.t[i], prev, a, f);
            for (int s = 1; s <= 48; ++s) {
                EvalAt(c, c.t[i] + (c.t[i + 1] - c.t[i]) * s / 48.0, p, a, f);
                len[i] += std::sqrt((p[0] - prev[0]) * (p[0] - prev[0]) + (p[1] - prev[1]) * (p[1] - prev[1]) + (p[2] - prev[2]) * (p[2] - prev[2]));
                std::memcpy(prev, p, sizeof p);
            }
            sum += len[i];
        }
        double moving = 0;
        sum = 0;
        for (size_t i = 0; i + 1 < keys.size(); ++i)
            if (!keys[i].cut) { moving += keys[i + 1].t - keys[i].t; sum += len[i]; }
        if (sum < 1.0) return;   // the camera barely moves: nothing to even out
        std::vector<double> dt(keys.size() - 1);
        for (size_t i = 0; i + 1 < keys.size(); ++i) dt[i] = keys[i].cut ? keys[i + 1].t - keys[i].t : moving * len[i] / sum;
        for (size_t i = 1; i < keys.size(); ++i) keys[i].t = keys[i - 1].t + std::max(kMinGap, dt[i - 1]);
        Spread();
    }
}

void SetDuration(double seconds) {
    std::lock_guard<std::mutex> g(lock);
    if (keys.size() < 2 || seconds < kMinGap) return;
    const double t0 = keys.front().t, old = keys.back().t - t0;
    if (old <= 0) return;
    PushUndo();
    const int firstTick = keys.front().tick;   // the first key stays where it was
    for (Key& k : keys) {
        k.t = t0 + (k.t - t0) * seconds / old;
        k.tick = -1;   // no longer the tick it was added on
    }
    keys.front().tick = firstTick;
    Spread();
}

void StartNow() {
    const double now = Game::CurTime();
    const int tick = Game::DemoTick();
    std::lock_guard<std::mutex> g(lock);
    if (keys.empty() || now <= 0) return;
    PushUndo();
    const double d = now - keys.front().t;
    for (Key& k : keys) { k.t += d; k.tick = -1; }
    keys.front().tick = tick;
}

void Orbit(const float center[3], float radius, float height, float degrees, float seconds, int count) {
    Key v;
    const bool known = CurrentView(v, nullptr);
    const double now = Game::CurTime();
    const int tick = Game::DemoTick();
    if (now <= 0 || count < 2 || seconds <= 0) return;
    // Start on the side the camera is on now, so the move begins near the current view.
    double start = 0;
    if (known) start = std::atan2(v.pos[1] - center[1], v.pos[0] - center[0]);
    std::lock_guard<std::mutex> g(lock);
    PushUndo();
    keys.clear();
    for (int i = 0; i < count; ++i) {
        const double f = (double)i / (count - 1);
        const double a = start + f * degrees * 3.14159265358979 / 180.0;
        Key k;
        k.t = now + f * seconds;
        k.tick = i == 0 ? tick : -1;
        k.pos[0] = center[0] + (float)(radius * std::cos(a));
        k.pos[1] = center[1] + (float)(radius * std::sin(a));
        k.pos[2] = center[2] + height;
        LookAt(k.pos, center, k.ang);
        k.ang[2] = 0;
        k.fov = known ? v.fov : 90.f;
        keys.push_back(k);
    }
    selected = 0;
}

void Dolly(float distance, float seconds) {
    Key v;
    if (!CurrentView(v, nullptr)) return;
    const double now = Game::CurTime();
    if (now <= 0 || seconds <= 0) return;
    const double p = v.ang[0] * 3.14159265358979 / 180.0, y = v.ang[1] * 3.14159265358979 / 180.0;
    const float dir[3] = {(float)(std::cos(p) * std::cos(y)), (float)(std::cos(p) * std::sin(y)), (float)-std::sin(p)};
    std::lock_guard<std::mutex> g(lock);
    PushUndo();
    keys.clear();
    Key a = v, b = v;
    a.t = now;
    a.tick = Game::DemoTick();
    b.t = now + seconds;
    b.tick = -1;
    for (int k = 0; k < 3; ++k) b.pos[k] = v.pos[k] + dir[k] * distance;
    keys.push_back(a);
    keys.push_back(b);
    selected = 0;
}

void Preview() {
    std::lock_guard<std::mutex> g(lock);
    if (keys.size() < 2) return;
    g_previewing = true;
    g_previewStart = (NowMs() / 1000.0);
    g_previewFrom = keys.front().t;
}
void StartFrozen(double step) {
    std::lock_guard<std::mutex> g(lock);
    if (keys.size() < 2) return;
    g_previewing = false;
    g_frozen = true;
    g_frozenDone = false;
    g_frozenFrom = keys.front().t;
    g_frozenStep = step;
    g_frozenFrames = 0;
    g_frozenWall = NowMs() / 1000.0;
}
void StopFrozen() {
    std::lock_guard<std::mutex> g(lock);
    g_frozen = false;
}
bool FrozenDone() {
    std::lock_guard<std::mutex> g(lock);
    return g_frozen && g_frozenDone;
}
void StopPreview() {
    std::lock_guard<std::mutex> g(lock);
    g_previewing = false;
}

static void SampleKeys(const std::vector<Key>& ks, int count, std::vector<float>& xyz) {
    xyz.clear();
    if (ks.size() < 2 || count < 2) return;
    const Channels c = Build(ks);
    const double t0 = ks.front().t, t1 = ks.back().t;
    for (int i = 0; i < count; ++i) {
        float p[3], a[3], f;
        EvalAt(c, t0 + (t1 - t0) * i / (count - 1), p, a, f);
        xyz.insert(xyz.end(), p, p + 3);
    }
}
void Sample(int count, std::vector<float>& xyz) {
    std::lock_guard<std::mutex> g(lock);
    SampleKeys(keys, count, xyz);
}

// ------------------------------------------------------------------ files (HLAE campath XML)
std::string Folder() { return DllFolder() + "DemoTools2-campaths\\"; }
static bool SafeName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ')) return false;
    return true;
}
std::vector<std::string> SavedNames() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((Folder() + "*.xml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        out.push_back(n.substr(0, n.size() - 4));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// HLAE: quaternion = yaw(Z) * pitch(Y) * roll(X), angles in Source convention.
struct Q { double w, x, y, z; };
static Q Mul(Q a, Q b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
static Q FromAngles(const float ang[3]) {
    const double d = 3.14159265358979 / 360.0;   // half angle in radians
    const Q qp{std::cos(ang[0] * d), 0, std::sin(ang[0] * d), 0}, qy{std::cos(ang[1] * d), 0, 0, std::sin(ang[1] * d)},
        qr{std::cos(ang[2] * d), std::sin(ang[2] * d), 0, 0};
    return Mul(Mul(qy, qp), qr);
}
static void ToAngles(Q q, float ang[3]) {
    const double sqw = q.w * q.w, sqx = q.x * q.x, sqy = q.y * q.y, sqz = q.z * q.z;
    const double ssq = sqw + sqx + sqy + sqz, inv = ssq ? 1 / ssq : 0;
    const double m00 = (sqx - sqy - sqz + sqw) * inv, m22 = (-sqx - sqy + sqz + sqw) * inv;
    const double m10 = 2.0 * (q.x * q.y + q.z * q.w) * inv, m20 = 2.0 * (q.x * q.z - q.y * q.w) * inv;
    const double m21 = 2.0 * (q.y * q.z + q.x * q.w) * inv;
    const double r = 180.0 / 3.14159265358979;
    ang[0] = (float)(std::asin(std::clamp(-m20, -1.0, 1.0)) * r);
    ang[1] = (float)(std::atan2(m10, m00) * r);
    ang[2] = (float)(std::atan2(m21, m22) * r);
}

bool Save(const std::string& name, std::string* why) {
    if (!SafeName(name)) { if (why) *why = "Use letters, digits, spaces, - and _."; return false; }
    CreateDirectoryA(Folder().c_str(), nullptr);
    std::lock_guard<std::mutex> g(lock);
    if (keys.empty()) { if (why) *why = "The path has no keys."; return false; }
    std::ofstream f(Folder() + name + ".xml");
    if (!f) { if (why) *why = "Couldn't write " + Folder() + name + ".xml"; return false; }
    char b[512];
    f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<campath";
    if (set.posInterp == Linear) f << " positionInterp=\"linear\"";
    if (set.angInterp == Linear) f << " rotationInterp=\"sLinear\"";
    if (set.fovInterp == Linear) f << " fovInterp=\"linear\"";
    if (set.hold) f << " hold=\"\"";
    f << ">\n\t<points>\n";
    f << "\t\t<!-- Made with DemoTools2. HLAE campath format (load with: mirv_campath load <file>). rx roll, ry pitch,"
         " rz yaw; qw qx qy qz the same rotation. t is CS2 game time; tick is DemoTools2's own (the demo tick). -->\n";
    for (const Key& k : keys) {
        const Q q = FromAngles(k.ang);
        std::snprintf(b, sizeof b,
                      "\t\t<p t=\"%.6f\" x=\"%.4f\" y=\"%.4f\" z=\"%.4f\" fov=\"%.4f\" rx=\"%.4f\" ry=\"%.4f\" rz=\"%.4f\" qw=\"%.9f\" qx=\"%.9f\""
                      " qy=\"%.9f\" qz=\"%.9f\" tick=\"%d\"%s/>\n",
                      k.t, k.pos[0], k.pos[1], k.pos[2], k.fov, k.ang[2], k.ang[0], k.ang[1], q.w, q.x, q.y, q.z, k.tick, k.cut ? " cut=\"1\"" : "");
        f << b;
    }
    f << "\t</points>\n</campath>\n";
    return (bool)f;
}

static bool Attr(const std::string& tag, const char* name, double& out) {
    const std::string key = std::string(" ") + name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) {   // also tabs / new lines before the name
        for (const char* sep : {"\t", "\n", "\r"}) if ((p = tag.find(sep + std::string(name) + "=\"")) != std::string::npos) break;
        if (p == std::string::npos) return false;
    }
    p = tag.find('"', p) + 1;
    out = std::atof(tag.c_str() + p);
    return true;
}
static bool HasAttr(const std::string& tag, const char* name) {
    double d;
    return Attr(tag, name, d) || tag.find(std::string(" ") + name + "=\"\"") != std::string::npos || tag.find(std::string(" ") + name + "\"") != std::string::npos;
}
static std::string AttrText(const std::string& tag, const char* name) {
    const std::string key = std::string(name) + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    return tag.substr(p, tag.find('"', p) - p);
}

bool Load(const std::string& name, std::string* why) {
    std::string path = name;
    if (path.find('\\') == std::string::npos && path.find('/') == std::string::npos) {
        if (!SafeName(name)) { if (why) *why = "No campath named '" + name + "'."; return false; }
        path = Folder() + name + ".xml";
    }
    std::ifstream f(path);
    if (!f) { if (why) *why = "Couldn't open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const size_t cp = text.find("<campath");
    if (cp == std::string::npos) { if (why) *why = "Not a campath file."; return false; }
    const std::string head = text.substr(cp, text.find('>', cp) - cp);
    std::vector<Key> out;
    for (size_t p = text.find("<p", cp); p != std::string::npos; p = text.find("<p", p + 2)) {
        const char next = p + 2 < text.size() ? text[p + 2] : 0;
        if (!(next == ' ' || next == '\t' || next == '\n' || next == '\r')) continue;   // <points>
        const size_t e = text.find('>', p);
        if (e == std::string::npos) break;
        const std::string tag = text.substr(p, e - p);
        Key k;
        double t, v;
        if (!Attr(tag, "t", t)) continue;
        k.t = t;
        if (Attr(tag, "x", v)) k.pos[0] = (float)v;
        if (Attr(tag, "y", v)) k.pos[1] = (float)v;
        if (Attr(tag, "z", v)) k.pos[2] = (float)v;
        k.fov = Attr(tag, "fov", v) ? (float)v : 90.f;
        double qw, qx, qy, qz;
        if (Attr(tag, "qw", qw) && Attr(tag, "qx", qx) && Attr(tag, "qy", qy) && Attr(tag, "qz", qz)) ToAngles({qw, qx, qy, qz}, k.ang);
        else {
            if (Attr(tag, "ry", v)) k.ang[0] = (float)v;
            if (Attr(tag, "rz", v)) k.ang[1] = (float)v;
            if (Attr(tag, "rx", v)) k.ang[2] = (float)v;
        }
        k.tick = Attr(tag, "tick", v) ? (int)v : -1;
        k.cut = Attr(tag, "cut", v) && v != 0;
        out.push_back(k);
    }
    if (out.empty()) { if (why) *why = "The file has no points."; return false; }
    std::lock_guard<std::mutex> g(lock);
    PushUndo();
    keys = std::move(out);
    Spread();
    set.posInterp = AttrText(head, "positionInterp") == "linear" ? Linear : Smooth;
    set.angInterp = AttrText(head, "rotationInterp") == "sLinear" ? Linear : Smooth;
    set.fovInterp = AttrText(head, "fovInterp") == "linear" ? Linear : Smooth;
    set.hold = HasAttr(head, "hold");
    double off;
    if (Attr(head, "offset", off) && off != 0) for (Key& k : keys) k.t += off;
    selected = 0;
    return true;
}


// ------------------------------------------------------------------ sequence
static std::vector<Key> SeqKeys();   // below
static int TickForLocked(double t);

bool SeqAdd(std::string* why) {
    Key v;
    if (!CurrentView(v, why)) return false;
    const double now = Game::CurTime();
    const int tick = Game::DemoTick();
    std::lock_guard<std::mutex> g(lock);
    if (seq.points.empty() || seq.startTick < 0) { seq.startTick = tick; seq.startTime = now; }
    Point p;
    std::memcpy(p.pos, v.pos, sizeof p.pos);
    std::memcpy(p.ang, v.ang, sizeof p.ang);
    p.fov = v.fov;
    // after the selected point (or at the end)
    const int at = seq.selected >= 0 && seq.selected < (int)seq.points.size() ? seq.selected + 1 : (int)seq.points.size();
    seq.points.insert(seq.points.begin() + at, p);
    seq.selected = at;
    return true;
}
bool SeqSetToView(int i) {
    Key v;
    if (!CurrentView(v, nullptr)) return false;
    std::lock_guard<std::mutex> g(lock);
    if (i < 0 || i >= (int)seq.points.size()) return false;
    std::memcpy(seq.points[i].pos, v.pos, sizeof v.pos);
    std::memcpy(seq.points[i].ang, v.ang, sizeof v.ang);
    seq.points[i].fov = v.fov;
    return true;
}
void SeqRemove(int i) {
    std::lock_guard<std::mutex> g(lock);
    if (i < 0 || i >= (int)seq.points.size()) return;
    seq.points.erase(seq.points.begin() + i);
    if (seq.selected >= (int)seq.points.size()) seq.selected = (int)seq.points.size() - 1;
    if (seq.points.empty()) seq.startTick = -1;
}
void SeqMove(int i, int dir) {
    std::lock_guard<std::mutex> g(lock);
    const int j = i + dir;
    if (i < 0 || j < 0 || i >= (int)seq.points.size() || j >= (int)seq.points.size()) return;
    std::swap(seq.points[i], seq.points[j]);
    seq.selected = j;
}
void SeqClear() {
    std::lock_guard<std::mutex> g(lock);
    seq.points.clear();
    seq.startTick = -1;
    seq.selected = -1;
}
bool SeqUseCurrentStart(std::string* why) {
    const double now = Game::CurTime();
    const int tick = Game::DemoTick();
    if (now <= 0 || tick < 0) { if (why) *why = "The demo moment isn't known - is a demo playing (paused is fine)?"; return false; }
    std::lock_guard<std::mutex> g(lock);
    seq.startTick = tick;
    seq.startTime = now;
    return true;
}

// Auto timing: long enough for the move at `speed` and for the turn at `turn`, a bit longer when the camera has to
// start or stop (ease), never under minTicks. A cut point holds for holdTicks.
int SeqTicks(int i) {
    if (i < 0 || i + 1 >= (int)seq.points.size()) return 0;
    const Point& a = seq.points[i];
    const Point& b = seq.points[i + 1];
    if (a.ticks > 0) return a.ticks;
    if (a.cut) return std::max(1, seq.holdTicks);
    const double dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    double turn = 0;
    for (int k = 0; k < 3; ++k) turn = std::max(turn, (double)std::fabs(Wrap180(b.ang[k] - a.ang[k])));
    turn = std::max(turn, (double)std::fabs(b.fov - a.fov));
    double sec = std::max(dist / std::max(1.f, seq.speed), turn / std::max(1.f, seq.turn));
    const bool shotStart = i == 0 || seq.points[i - 1].cut, shotEnd = i + 2 == (int)seq.points.size() || b.cut;
    if (set.easeEnds && set.posInterp == Smooth) sec *= 1.0 + 0.25 * (shotStart ? 1 : 0) + 0.25 * (shotEnd ? 1 : 0);
    return std::max(seq.minTicks, (int)std::lround(sec * 64.0));
}
int SeqTotalTicks() {
    int n = 0;
    for (int i = 0; i + 1 < (int)seq.points.size(); ++i) n += SeqTicks(i);
    return n;
}
static std::vector<Key> SeqKeys() {
    std::vector<Key> out;
    double t = seq.startTime;
    int tick = seq.startTick;
    for (int i = 0; i < (int)seq.points.size(); ++i) {
        const Point& p = seq.points[i];
        Key k;
        k.t = t;
        k.tick = tick;
        std::memcpy(k.pos, p.pos, sizeof k.pos);
        std::memcpy(k.ang, p.ang, sizeof k.ang);
        k.fov = p.fov;
        k.cut = p.cut && i + 1 < (int)seq.points.size();
        out.push_back(k);
        const int n = SeqTicks(i);
        t += n / 64.0;   // CS2 demos: 64 ticks a second
        tick += n;
    }
    return out;
}
bool SeqGenerate(std::string* why) {
    std::lock_guard<std::mutex> g(lock);
    if (seq.points.size() < 2) { if (why) *why = "Add at least two points."; return false; }
    if (seq.startTick < 0 || seq.startTime <= 0) { if (why) *why = "The start moment isn't known - press Start here with the demo on the right tick."; return false; }
    PushUndo();
    keys = SeqKeys();
    selected = 0;
    set.enabled = true;
    return true;
}
void SeqFromPath() {
    std::lock_guard<std::mutex> g(lock);
    if (keys.empty()) return;
    seq.points.clear();
    for (size_t i = 0; i < keys.size(); ++i) {
        Point p;
        std::memcpy(p.pos, keys[i].pos, sizeof p.pos);
        std::memcpy(p.ang, keys[i].ang, sizeof p.ang);
        p.fov = keys[i].fov;
        p.cut = keys[i].cut;
        p.ticks = i + 1 < keys.size() ? std::max(1, (int)std::lround((keys[i + 1].t - keys[i].t) * 64.0)) : 0;
        seq.points.push_back(p);
    }
    seq.startTime = keys.front().t;
    seq.startTick = keys.front().tick >= 0 ? keys.front().tick : TickForLocked(keys.front().t);
    seq.selected = 0;
}
void SampleSequence(int count, std::vector<float>& xyz) {
    std::lock_guard<std::mutex> g(lock);
    std::vector<Key> ks = SeqKeys();
    if (seq.startTime <= 0) {   // no start yet: time from 0 just for the drawing
        double t = 1;
        for (size_t i = 0; i < ks.size(); ++i) { ks[i].t = t; t += std::max(1, SeqTicks((int)i)) / 64.0; }
    }
    SampleKeys(ks, count, xyz);
}

void QueueCommand(const std::string& cmd) {
    std::lock_guard<std::mutex> g(lock);
    g_commands.push_back(cmd);
}
static int TickForLocked(double t);
int TickFor(double t) {
    std::lock_guard<std::mutex> g(lock);
    return TickForLocked(t);
}
static int TickForLocked(double t) {
    const Key* best = nullptr;
    for (const Key& k : keys)
        if (k.tick >= 0 && (!best || std::fabs(k.t - t) < std::fabs(best->t - t))) best = &k;
    if (!best) return -1;
    return best->tick + (int)std::lround((t - best->t) * 64.0);   // CS2 demos: 64 ticks a second
}

// ------------------------------------------------------------------ mirv_pov
bool PovAvailable() {
    static int known = -1;
    static ULONGLONG checkedAt = 0;
    if (known == 1) return true;
    if (GetTickCount64() - checkedAt < 5000 && known == 0) return false;
    checkedAt = GetTickCount64();
    known = Game::HasModuleString("AfxHookSource2.dll", "mirv_pov") ? 1 : 0;
    return known == 1;
}
void ApplyPov() {
    std::lock_guard<std::mutex> g(lock);
    g_commands.push_back(pov.on ? "mirv_pov 1" : "mirv_pov 0");
    if (pov.on) {
        g_commands.push_back(pov.scoreboard ? "mirv_pov_scoreboard 1" : "mirv_pov_scoreboard 0");
        g_commands.push_back(pov.buymenu ? "mirv_pov_buymenu 1" : "mirv_pov_buymenu 0");
    }
}

// ------------------------------------------------------------------ console
static void Cmd(const Console::Args& a) {
    const std::string sub = a.size() > 1 ? a[1] : "";
    std::string arg = a.size() > 2 ? a[2] : "";
    for (size_t i = 3; i < a.size(); ++i) arg += " " + a[i];
    std::string why;
    if (sub == "add") {
        if (AddKeyHere(&why)) Console::Print("campath: key added (%u keys)", (unsigned)keys.size());
        else Console::Print("campath: %s", why.c_str());
    } else if (sub == "enabled" || sub == "enable") {
        { std::lock_guard<std::mutex> g(lock); set.enabled = std::atoi(arg.c_str()) != 0; }
        Console::Print("campath %s", set.enabled ? "on" : "off");
    } else if (sub == "clear") { Clear(); Console::Print("campath cleared");
    } else if (sub == "undo") { Console::Print(Undo() ? "campath: undone" : "campath: nothing to undo");
    } else if (sub == "remove") { RemoveKey(std::atoi(arg.c_str()) - 1); Console::Print("campath: %u keys", (unsigned)keys.size());
    } else if (sub == "evenspeed") { EvenSpeed(); Console::Print("campath: re-timed for an even speed");
    } else if (sub == "duration") { SetDuration(std::atof(arg.c_str())); Console::Print("campath: %.2f s long", Duration());
    } else if (sub == "startnow") { StartNow(); Console::Print("campath: starts now");
    } else if (sub == "preview") { Preview(); Console::Print("campath: preview");
    } else if (sub == "hold") { { std::lock_guard<std::mutex> g(lock); set.hold = std::atoi(arg.c_str()) != 0; } Console::Print("campath hold %d", set.hold ? 1 : 0);
    } else if (sub == "save") {
        if (Save(arg, &why)) Console::Print("campath saved as '%s'", arg.c_str());
        else Console::Print("campath: %s", why.c_str());
    } else if (sub == "load") { if (Load(arg, &why)) Console::Print("campath '%s' loaded (%u keys)", arg.c_str(), (unsigned)keys.size()); else Console::Print("campath: %s", why.c_str());
    } else if (sub == "seq") {
        const std::string what = a.size() > 2 ? a[2] : "";
        if (what == "add") {
            if (SeqAdd(&why)) Console::Print("sequence: point added (%u points)", (unsigned)seq.points.size());
            else Console::Print("sequence: %s", why.c_str());
        } else if (what == "generate") {
            if (SeqGenerate(&why)) Console::Print("sequence: path generated, %u keys, %.2f s", (unsigned)keys.size(), Duration());
            else Console::Print("sequence: %s", why.c_str());
        } else if (what == "start") {
            if (SeqUseCurrentStart(&why)) Console::Print("sequence: starts at tick %d", seq.startTick);
            else Console::Print("sequence: %s", why.c_str());
        } else if (what == "clear") { SeqClear(); Console::Print("sequence cleared");
        } else if (what == "undo") { SeqRemove((int)seq.points.size() - 1); Console::Print("sequence: last point removed");
        } else Console::Print("dt2_campath seq add | generate | start | undo | clear");
    } else if (sub == "list") {
        auto n = SavedNames();
        Console::Print("campaths (%u) in %s:", (unsigned)n.size(), Folder().c_str());
        for (auto& s : n) Console::Print("  %s", s.c_str());
    } else if (sub == "print") {
        std::lock_guard<std::mutex> g(lock);
        Console::Print("campath: %u keys, %s, %s", (unsigned)keys.size(), set.enabled ? "on" : "off", set.hold ? "hold" : "no hold");
        for (size_t i = 0; i < keys.size(); ++i) {
            const Key& k = keys[i];
            Console::Print("  %2u  t %.3f (+%.2f s)  pos %.1f %.1f %.1f  ang %.1f %.1f %.1f  fov %.1f", (unsigned)i + 1, k.t, k.t - keys.front().t, k.pos[0],
                           k.pos[1], k.pos[2], k.ang[0], k.ang[1], k.ang[2], k.fov);
        }
    } else {
        Console::Print("dt2_campath seq add|generate|start|undo|clear | add | enabled 0|1 | hold 0|1 | preview | evenspeed | duration <s> | startnow | remove <n> | undo |"
                       " clear | print | save <name> | load <name> | list");
        Console::Print("  Files: %s (HLAE campath XML - mirv_campath can load them too)", Folder().c_str());
    }
}

void Init() { Console::Add("dt2_campath", "DemoTools2 campaths - type dt2_campath for help", &Cmd); }

}  // namespace Campath
