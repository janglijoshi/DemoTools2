// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// Kill recorder - see record.h. Two recorders, the one chosen in the Record tab is used:
//   HLAE: drives mirv_streams through console commands: ffmpeg setting "dt2mp4" (H.264), optionally wrapped in
//     sampler "dt2blur" (motion blur: the game runs at fps * samples frames a second and HLAE blends them into fps),
//     game audio as WAV; one take per clip. Takes stay in their folders. If it records nothing (an HLAE build not
//     updated for this CS2), the recording stops and says to use DTRecord.
//   DTRecord (capture.h): the same frames read from DirectX 11 and blended here, then a second pass of the clip in
//     real time that records only the sound - or, by default, picture and sound together in one real-time pass.
//     Doesn't use HLAE's recording at all.
//   Afterwards (background thread) each clip's video and audio are joined into <out>\<clip>.mp4, and all clips into
//   one file.
#include "record.h"
#include "campath.h"
#include "capture.h"
#include "console.h"
#include "game.h"
#include "pov.h"
#include "util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <climits>
#include <deque>
#include <map>
#include <set>
#include <thread>

namespace Record {

std::mutex lock;
Settings set;
static Scan g_scan;

// game-thread snapshot of the demo's players (for the window)
static std::mutex g_playersLock;
static std::vector<Pov::PlayerSlot> g_players;

// progress (guarded by lock)
static Progress g_progress;

// ------------------------------------------------------------------ helpers
static bool FileExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string BaseName(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".dem") == 0 || n.compare(n.size() - 4, 4, ".DEM") == 0)) n.resize(n.size() - 4);
    return n;
}
static std::string Safe(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return o.empty() ? "demo" : o;
}
static void MakeDirs(const std::string& dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
}

// CS2's own window in this process: the biggest visible top-level window that isn't DemoTools2's
struct WinFind { HWND best = nullptr; long area = 0; };
static BOOL CALLBACK EnumWin(HWND w, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    // InternalGetWindowText: GetWindowText would send a message to the game's window and wait for the game thread
    // (which can be waiting for us - that froze both CS2 and the menu)
    WCHAR title[64] = {};
    InternalGetWindowText(w, title, 64);
    if (wcsncmp(title, L"DemoTools2", 10) == 0) return TRUE;
    RECT r{};
    if (!GetClientRect(w, &r)) return TRUE;
    const long a = (long)(r.right - r.left) * (r.bottom - r.top);
    auto* f = (WinFind*)lp;
    if (a > f->area) { f->area = a; f->best = w; }
    return TRUE;
}
bool GameSize(int& w, int& h) {
    static std::mutex m;
    static int cw = 0, ch = 0;
    static ULONGLONG at = 0;
    std::lock_guard<std::mutex> g(m);
    if (GetTickCount64() - at > 1000) {   // looked up at most once a second
        at = GetTickCount64();
        WinFind f;
        EnumWindows(EnumWin, (LPARAM)&f);
        RECT r{};
        if (f.best && GetClientRect(f.best, &r)) { cw = r.right - r.left; ch = r.bottom - r.top; }
    }
    w = cw;
    h = ch;
    return w > 0 && h > 0;
}
bool StretchSize(int w, int h, int& ow, int& oh) {
    if (w <= 0 || h <= 0) return false;
    const double a = (double)w / h;
    if (a > 1.40) return false;   // already wide (16:10 is 1.6, 16:9 1.78): leave it
    oh = h & ~1;
    ow = ((int)std::lround(h * 16.0 / 9.0) + 1) & ~1;
    return true;
}

bool HlaeLoaded() { return GetModuleHandleA("AfxHookSource2.dll") != nullptr; }

// HLAE took the commands but recorded nothing this session (not updated for this CS2)
static std::atomic<bool> g_hlaeBroken{false};
// "HLAE recording" is a crash-guarded step (util.h): if CS2 crashed while HLAE was starting to record (an HLAE build
// not updated for this CS2 can do that instead of recording nothing), the next session knows it: the recorder starts
// on DTRecord and the window says why.
static const char* kHlaeStep = "HLAE recording";
static bool HlaeHasStreams() {
    static int has = -1;   // the loaded HLAE has mirv_streams (checked once)
    if (has < 0) has = Game::HasModuleString("AfxHookSource2.dll", "mirv_streams") ? 1 : 0;
    return has == 1;
}
std::string RecorderFor(int recorder, std::string* why) {
    if (recorder == Rec_Hlae) {
        if (!HlaeLoaded()) { if (why) *why = "HLAE isn't loaded - choose DTRecord"; return ""; }
        if (!HlaeHasStreams()) { if (why) *why = "the loaded HLAE has no mirv_streams - choose DTRecord"; return ""; }
        return "HLAE";
    }
    std::string w;
    if (Capture::Available(&w)) return "DTRecord";
    if (why) *why = w;
    return "";
}
std::string HlaeWarning() {
    if (g_hlaeBroken) return "HLAE recorded nothing this session - it may not be updated for this CS2 yet. Use DTRecord.";
    if (HlaeLoaded() && CrashedLastTime(kHlaeStep)) return "CS2 crashed last time while HLAE started recording - HLAE may not be updated for this CS2 yet. DTRecord is safer.";
    return "";
}
static std::string SettingsFile() { return DllFolder() + "DemoTools2-record.cfg"; }
void SaveSettings() {
    int r;
    bool same, sound;
    {
        std::lock_guard<std::mutex> g(lock);
        r = set.recorder;
        same = set.samePass;
        sound = set.sound;
    }
    if (FILE* f = std::fopen(SettingsFile().c_str(), "wb")) {
        std::fprintf(f, "recorder=%s\nsound=%s\n", r == Rec_DTRecord ? "dtrecord" : "hlae", !sound ? "off" : same ? "same" : "second");
        std::fclose(f);
    }
}
static void LoadSettings() {
    // default: HLAE when it is loaded and didn't crash CS2 last time, DTRecord otherwise; a saved choice wins
    int r = HlaeLoaded() && !CrashedLastTime(kHlaeStep) ? Rec_Hlae : Rec_DTRecord;
    bool same = true, sound = true;
    if (FILE* f = std::fopen(SettingsFile().c_str(), "rb")) {
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "recorder=dtrecord", 17) == 0) r = Rec_DTRecord;
            else if (std::strncmp(line, "recorder=hlae", 13) == 0 && !CrashedLastTime(kHlaeStep)) r = Rec_Hlae;
            else if (std::strncmp(line, "sound=off", 9) == 0) sound = false;
            else if (std::strncmp(line, "sound=second", 12) == 0) same = false;
        }
        std::fclose(f);
    }
    std::lock_guard<std::mutex> g(lock);
    set.recorder = r;
    set.samePass = same;
    set.sound = sound;
}

// ffmpeg.exe somewhere under dir (at most `depth` folders down)
static std::string FindExe(const std::string& dir, int depth) {
    if (FileExists(dir + "\\ffmpeg.exe")) return dir + "\\ffmpeg.exe";
    if (depth <= 0) return "";
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found;
    do {
        const std::string n = f.cFileName;
        if (n == "." || n == ".." || !(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        found = FindExe(dir + "\\" + n, depth - 1);
    } while (found.empty() && FindNextFileA(h, &f));
    FindClose(h);
    return found;
}
static std::string g_searched;
std::string FfmpegSearched() {
    std::lock_guard<std::mutex> g(lock);
    return g_searched;
}
std::string FfmpegPath() {
    static std::string cached;
    static ULONGLONG at = 0;
    {
        std::lock_guard<std::mutex> g(lock);
        if (!set.ffmpeg.empty() && FileExists(set.ffmpeg)) return set.ffmpeg;
    }
    if (!cached.empty() && FileExists(cached)) return cached;
    if (GetTickCount64() - at < 3000) return "";
    at = GetTickCount64();
    // HLAE's folder: where its CS2 DLL was loaded from (and the folders above it, as installs differ)
    std::string looked;
    HMODULE m = GetModuleHandleA("AfxHookSource2.dll");
    char buf[MAX_PATH];
    if (m && GetModuleFileNameA(m, buf, MAX_PATH)) {
        std::string dir = buf;
        for (int up = 0; up < 3 && cached.empty(); ++up) {
            const size_t s = dir.find_last_of('\\');
            if (s == std::string::npos) break;
            dir.resize(s);
            looked += (looked.empty() ? "" : "; ") + dir;
            if (FileExists(dir + "\\ffmpeg.exe")) cached = dir + "\\ffmpeg.exe";
            else cached = FindExe(dir + "\\ffmpeg", 3);
        }
    } else looked = "HLAE's DLL isn't loaded";
    // DemoTools2's own folder and the ones above it (DemoTools2 often sits inside the HLAE folder)
    std::string dir = DllFolder();
    if (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    for (int up = 0; up < 3 && cached.empty(); ++up) {
        looked += (looked.empty() ? "" : "; ") + dir;
        if (FileExists(dir + "\\ffmpeg.exe")) cached = dir + "\\ffmpeg.exe";
        else cached = FindExe(dir + "\\ffmpeg", 3);
        const size_t s = dir.find_last_of('\\');
        if (s == std::string::npos) break;
        dir.resize(s);
    }
    // ffmpeg on PATH
    if (cached.empty()) {
        char p[MAX_PATH];
        if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, p, nullptr)) cached = p;
    }
    {
        std::lock_guard<std::mutex> g(lock);
        g_searched = looked;
    }
    if (!cached.empty()) Log("record: ffmpeg is %s", cached.c_str());
    else Log("record: ffmpeg not found (looked in %s and its ffmpeg folder, and on PATH)", looked.c_str());
    return cached;
}

std::string OutDir() {
    std::lock_guard<std::mutex> g(lock);
    if (!set.outDir.empty()) return set.outDir;
    return DllFolder() + "recordings\\" + Safe(BaseName(set.demoPath.empty() ? "demo" : set.demoPath));
}

// ------------------------------------------------------------------ the demo CS2 is playing
// CS2 doesn't say which file it plays, so: every .dem path it has open or remembers (its open files, then the text
// in its memory - engine first, then everything), each checked to be a CS2 demo; if there are several (the Watch
// tab lists many), the one whose players are the players in the game now wins.
static bool IsDemoFile(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    char h[8] = {};
    const bool ok = std::fread(h, 1, 8, f) == 8 && std::memcmp(h, "PBDEMS2", 8) == 0;
    std::fclose(f);
    return ok;
}
static void AddCandidate(std::vector<std::string>& out, std::string p) {
    while (!p.empty() && (p[0] == '"' || p[0] == ' ' || p[0] == '\'')) p.erase(0, 1);
    if (p.rfind("\\\\?\\UNC\\", 0) == 0) p = "\\\\" + p.substr(8);
    else if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.size() < 5 || p.size() > 400) return;
    for (char& c : p) if (c == '/') c = '\\';
    const bool absolute = (p.size() > 2 && p[1] == ':') || p.rfind("\\\\", 0) == 0;
    if (!absolute) p = Game::GameFolder() + p;   // "replays\x.dem" is relative to game\csgo
    {   // one spelling per file ("csgo\\replays\\x.dem" and "csgo\replays\x.dem" are the same demo)
        char full[1024];
        const DWORD n = GetFullPathNameA(p.c_str(), sizeof full, full, nullptr);
        if (n && n < sizeof full) p = full;
        std::string q;
        for (size_t i = 0; i < p.size(); ++i)
            if (!(p[i] == '\\' && i > 1 && p[i - 1] == '\\')) q += p[i];
        p = q;
    }
    for (const std::string& o : out) if (_stricmp(o.c_str(), p.c_str()) == 0) return;
    if (FileExists(p) && IsDemoFile(p)) out.push_back(p);
}
// Also notes the maps CS2 has open (game\csgo\maps\<name>.vpk): the demo playing is on one of them.
static void FromHandles(std::vector<std::string>& out, std::set<std::string>* maps = nullptr) {
    for (uintptr_t v = 4; v < 0x40000; v += 4) {
        HANDLE h = (HANDLE)v;
        if (GetFileType(h) != FILE_TYPE_DISK) continue;
        char path[1024];
        const DWORD n = GetFinalPathNameByHandleA(h, path, sizeof path, FILE_NAME_NORMALIZED);
        if (!n || n >= sizeof path || n < 5) continue;
        if (_stricmp(path + n - 4, ".dem") == 0) AddCandidate(out, path);
        else if (maps && _stricmp(path + n - 4, ".vpk") == 0) {
            std::string sp(path, n);
            for (char& c : sp) c = (char)std::tolower((unsigned char)c);
            const size_t m = sp.rfind("\\maps\\");
            if (m == std::string::npos || sp.find('\\', m + 6) != std::string::npos) continue;   // directly in maps\ only
            std::string name = sp.substr(m + 6, sp.size() - m - 6 - 4);
            if (name.size() > 4 && name.compare(name.size() - 4, 4, "_dir") == 0) name.resize(name.size() - 4);
            if (!name.empty()) maps->insert(name);
        }
    }
}
// ".dem" text in [base, base+size): read with ReadProcessMemory so memory freed meanwhile can't crash us
static void FromMemory(std::vector<std::string>& out, uintptr_t base, size_t size, size_t& budget) {
    static std::vector<uint8_t> buf(1 << 20);
    const size_t step = buf.size() - 1024;
    for (size_t off = 0; off < size && budget; off += step) {
        const size_t want = std::min(buf.size(), size - off);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(base + off), buf.data(), want, &got) || got < 8) continue;
        budget = budget > got ? budget - got : 0;
        for (size_t i = 4; i + 5 < got; ++i) {
            // 8-bit text: ".dem" then the end of the text
            if (buf[i] == '.' && (buf[i + 1] | 32) == 'd' && (buf[i + 2] | 32) == 'e' && (buf[i + 3] | 32) == 'm' &&
                (buf[i + 4] == 0 || buf[i + 4] == '"')) {
                size_t st = i;
                while (st > 0 && i - st < 300 && buf[st - 1] >= 0x20 && buf[st - 1] != '"') --st;
                AddCandidate(out, std::string((const char*)buf.data() + st, i + 4 - st));
                if (out.size() > 40) return;
            }
            // UTF-16 text: . d e m 0
            if (i + 10 < got && buf[i] == '.' && buf[i + 1] == 0 && (buf[i + 2] | 32) == 'd' && buf[i + 3] == 0 && (buf[i + 4] | 32) == 'e' &&
                buf[i + 5] == 0 && (buf[i + 6] | 32) == 'm' && buf[i + 7] == 0 && buf[i + 8] == 0 && buf[i + 9] == 0 && (i & 1) == 0) {
                size_t st = i;
                while (st >= 2 && i - st < 600 && buf[st - 1] == 0 && buf[st - 2] >= 0x20 && buf[st - 2] < 0x7f) st -= 2;
                std::string a;
                for (size_t k = st; k < i + 8; k += 2) a += (char)buf[k];
                AddCandidate(out, a);
                if (out.size() > 40) return;
            }
        }
    }
}
static void FromModule(std::vector<std::string>& out, const char* name, size_t& budget) {
    HMODULE m = GetModuleHandleA(name);
    if (!m) return;
    const uint8_t* b = (const uint8_t*)m;
    const auto* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) && !(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            FromMemory(out, (uintptr_t)b + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, budget);
}
static void FromProcess(std::vector<std::string>& out, size_t& budget) {
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && budget && out.size() <= 40; a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE)) && !(mbi.Protect & PAGE_GUARD))
            FromMemory(out, (uintptr_t)mbi.BaseAddress, mbi.RegionSize, budget);
    }
}

// the players in the game now (filled by OnFrame)
static std::vector<uint64_t> NowXuids();

// Players of a demo file (full read), kept while the file doesn't change.
struct ScanCache { long long size = -1; FILETIME written{}; std::vector<DemoScan::Result::Player> players; bool ok = false; };
static std::map<std::string, ScanCache> g_scanCache;
static const ScanCache& PlayersOf(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a);
    const long long size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    ScanCache& c = g_scanCache[path];
    if (c.size != size || CompareFileTime(&c.written, &a.ftLastWriteTime) != 0) {
        const DemoScan::Result r = DemoScan::ScanFile(path);
        c.size = size;
        c.written = a.ftLastWriteTime;
        c.ok = r.ok;
        c.players = r.players;
    }
    return c;
}
static std::string LowerName(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static std::string g_autoFoundFind;   // the finder's own last pick (only touched by its thread)
static std::string FindOpenDemo() {
    std::vector<std::string> c;
    std::set<std::string> maps;
    size_t budget = (size_t)6 << 30;   // read at most 6 GB of memory
    FromHandles(c, &maps);
    const size_t fromHandles = c.size();
    if (c.empty()) { FromModule(c, "engine2.dll", budget); FromModule(c, "client.dll", budget); FromModule(c, "filesystem_stdio.dll", budget); }
    if (c.empty()) FromProcess(c, budget);
    if (c.empty()) return "";
    if (c.size() == 1 && fromHandles == 1) return c[0];   // the one file CS2 holds open
    // several, or names from memory (old demos are often not held open, so they come from text in memory - every demo played this session):
    // the demo's map must be loaded and the current tick must fit in it; then the players in the game now, by Steam
    // ID and by name (old demos often carry no Steam IDs); the pick from last time wins a tie, so it doesn't flip.
    const std::vector<uint64_t> nowX = NowXuids();
    std::vector<std::string> nowN;
    {
        std::lock_guard<std::mutex> g(g_playersLock);
        for (const auto& p : g_players) if (!p.name.empty()) nowN.push_back(LowerName(p.name));
    }
    const int tick = Game::DemoTick();
    std::string mapsText;
    for (const auto& m : maps) mapsText += (mapsText.empty() ? "" : ",") + m;
    std::string best, report;
    int bestScore = INT_MIN, scanned = 0;
    for (size_t i = 0; i < c.size() && i < 40; ++i) {
        std::string map;
        int ticks = 0;
        if (!DemoScan::ReadHeader(c[i], map, ticks)) continue;
        int score = 0;
        std::string why;
        const bool mapOk = maps.empty() || maps.count(LowerName(map));
        if (!mapOk) { score -= 1000; why += " other map (" + map + ")"; }
        if (ticks > 0 && tick > ticks + 128) { score -= 1000; why += " too short (" + std::to_string(ticks) + " ticks)"; }
        int xs = 0, ns = 0;
        if (score > -1000 && scanned < 12) {   // full read only for the ones that can be it
            ++scanned;
            const ScanCache& sc = PlayersOf(c[i]);
            for (const auto& p : sc.players) {
                for (uint64_t x : nowX) if (p.xuid && p.xuid == x) ++xs;
                const std::string n = LowerName(p.name);
                for (const auto& m : nowN) if (!n.empty() && n == m) { ++ns; break; }
            }
            score += xs * 10 + ns * 4;
        }
        if (i < fromHandles) score += 30;   // CS2 holds it open
        if (_stricmp(c[i].c_str(), g_autoFoundFind.c_str()) == 0) score += 2;
        char line[1400];
        std::snprintf(line, sizeof line, "record: candidate %s - map %s, %d ticks, %d Steam ID(s) and %d name(s) of the %zu players in the game, score %d%s\n",
                      c[i].c_str(), map.c_str(), ticks, xs, ns, std::max(nowX.size(), nowN.size()), score, why.c_str());
        report += line;
        if (score > bestScore) { bestScore = score; best = c[i]; }
    }
    // the details only when the pick changes (this runs every few seconds)
    static std::string lastPick = "-";
    const std::string pick = bestScore <= -1000 ? "" : best;
    if (pick != lastPick) {
        lastPick = pick;
        size_t a = 0;
        for (size_t b; (b = report.find('\n', a)) != std::string::npos; a = b + 1) Log("%s", report.substr(a, b - a).c_str());
        Log("record: loaded map %s, demo tick %d -> %s", mapsText.empty() ? "?" : mapsText.c_str(), tick,
            pick.empty() ? "none of the demo files fits the game" : pick.c_str());
    }
    if (pick.empty()) return "";
    g_autoFoundFind = best;
    return best;
}
static std::atomic<bool> g_finding{false}, g_findFailed{false};
bool AutoFindFailed() { return g_findFailed.load(); }
static std::string g_autoFound;   // last demo found playing (guarded by lock)
static bool g_chosen = false;     // the user picked the file: the finder leaves it alone until the next demo starts

// ------------------------------------------------------------------ scan
void StartScan(const std::string& path, bool chosen) {
    {
        std::lock_guard<std::mutex> g(lock);
        if (g_scan.busy) return;
        g_chosen = chosen;
        g_scan.busy = true;
        if (_stricmp(path.c_str(), g_scan.path.c_str()) != 0) { set.slot = -1; set.xuid = 0; }   // another demo: pick again
        g_scan.path = path;
        set.demoPath = path;
    }
    std::thread([path] {
        DemoScan::Result r = DemoScan::ScanFile(path);
        Log("record: scanned %s - %s, map %s, %u rounds, %u kills", path.c_str(), r.ok ? "ok" : r.error.c_str(), r.map.c_str(),
            (unsigned)r.roundStarts.size(), (unsigned)r.kills.size());
        std::lock_guard<std::mutex> g(lock);
        g_scan.result = std::move(r);
        g_scan.busy = false;
    }).detach();
}
Scan ScanState() {
    std::lock_guard<std::mutex> g(lock);
    return g_scan;
}

static std::vector<uint64_t> NowXuids() {
    std::lock_guard<std::mutex> g(g_playersLock);
    std::vector<uint64_t> v;
    for (const auto& p : g_players) if (p.xuid) v.push_back(p.xuid);
    return v;
}

std::vector<Who> PlayersWithKills() {
    std::vector<Pov::PlayerSlot> ps;
    {
        std::lock_guard<std::mutex> g(g_playersLock);
        ps = g_players;
    }
    std::lock_guard<std::mutex> g(lock);
    std::vector<Who> out;
    // the demo file's players, then what the game shows now (team, and anyone the file didn't name)
    for (const auto& p : g_scan.result.players) out.push_back({p.slot, p.xuid, p.name, 0, 0});
    for (const auto& p : ps) {
        bool known = false;
        for (Who& w : out)
            if (w.slot == p.slot) { known = true; w.team = p.team; if (!w.xuid) w.xuid = p.xuid; if (w.name.empty()) w.name = p.name; }
        if (!known) out.push_back({p.slot, p.xuid, p.name, p.team, 0});
    }
    // anyone with kills that neither knows by name
    for (const auto& k : g_scan.result.kills) {
        if (k.attacker < 0) continue;
        bool known = false;
        for (const Who& w : out) known |= w.slot == k.attacker;
        if (!known) out.push_back({k.attacker, 0, "player " + std::to_string(k.attacker + 1), 0, 0});
    }
    for (Who& w : out)
        for (const auto& k : g_scan.result.kills) if (k.attacker == w.slot && k.victim != w.slot) ++w.kills;
    std::sort(out.begin(), out.end(), [](const Who& a, const Who& b) { return a.team != b.team ? a.team < b.team : a.kills > b.kills; });
    return out;
}

std::vector<Clip> BuildClips() {
    float before, after;
    bool merge;
    int slot;
    {
        std::lock_guard<std::mutex> g(lock);
        before = set.before;
        after = set.after;
        merge = set.merge;
        slot = set.slot;
    }
    std::vector<Clip> out;
    if (slot < 0) return out;
    std::lock_guard<std::mutex> g(lock);
    const auto& kills = g_scan.result.kills;
    const int pre = (int)std::lround(before * 64), post = (int)std::lround(after * 64);
    for (int i = 0; i < (int)kills.size(); ++i) {
        const auto& k = kills[i];
        if (k.attacker != slot || k.victim == slot) continue;
        if (k.round <= 0 && !g_scan.result.roundStarts.empty()) continue;   // warmup
        const int s = std::max(0, k.tick - pre), e = k.tick + post;
        if (merge && !out.empty() && s <= out.back().end && out.back().round == k.round) {
            out.back().end = e;
            out.back().kills.push_back(i);
        } else {
            Clip c;
            c.start = s;
            c.end = e;
            c.round = k.round;
            c.kills.push_back(i);
            out.push_back(c);
        }
    }
    for (size_t n = 0; n < out.size(); ++n) {
        Clip& c = out[n];
        std::string first = kills[c.kills[0]].weapon;
        char lab[96];
        if (c.round > 0) std::snprintf(lab, sizeof lab, "%02u_round%02d_%uk_%s", (unsigned)n + 1, c.round, (unsigned)c.kills.size(), Safe(first).c_str());
        else std::snprintf(lab, sizeof lab, "%02u_%uk_%s", (unsigned)n + 1, (unsigned)c.kills.size(), Safe(first).c_str());
        c.label = lab;
        std::string sum = std::to_string(c.kills.size()) + (c.kills.size() == 1 ? " kill - " : " kills - ");
        for (size_t j = 0; j < c.kills.size(); ++j) {
            const auto& k = kills[c.kills[j]];
            if (j) sum += ", ";
            sum += k.weapon;
            std::string tags;
            if (k.headshot) tags += "HS ";
            if (k.wallbang) tags += "wall ";
            if (k.noscope) tags += "noscope ";
            if (k.thruSmoke) tags += "smoke ";
            if (!tags.empty()) { tags.pop_back(); sum += " (" + tags + ")"; }
        }
        c.summary = sum;
    }
    return out;
}

// ------------------------------------------------------------------ post-processing (background)
struct Job { int type; std::string label; std::vector<std::string> files; std::string out; ULONGLONG since = 0; };   // 0 mux, 1 combine; since: FILETIME of the take's start
static std::mutex g_jobLock;
static std::condition_variable g_jobCv;
static std::deque<Job> g_jobs;
static bool g_worker = false;

static void SetPost(const std::string& s) {
    std::lock_guard<std::mutex> g(lock);
    g_progress.post = s;
    if (!s.empty()) Log("record: %s", s.c_str());
}

// errLog: ffmpeg's messages go there (kept only when it failed)
static int Run(const std::string& cmdline, const std::string& errLog = "") {
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::string c = cmdline;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE err = errLog.empty() ? INVALID_HANDLE_VALUE
                                : CreateFileA(errLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (err != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = err;
        si.hStdError = err;
    }
    const BOOL started = CreateProcessA(nullptr, c.data(), nullptr, nullptr, err != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                        nullptr, nullptr, &si, &pi);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!started) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 10 * 60 * 1000) == WAIT_TIMEOUT) {   // stuck: don't leave it running
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    } else
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static ULONGLONG FileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
// newest folder in dir whose name starts with prefix, made at or after `since` (a FILETIME; 0 = any). Without the
// time check, a clip whose take failed picked up an older take of the same name (recording the same demo again).
static std::string NewestTake(const std::string& dir, const std::string& prefix, ULONGLONG since) {
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((dir + "\\" + prefix + "*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string best;
    FILETIME bt{};
    const ULONGLONG slack = 5ull * 10000000ull;   // 5 s: file system time granularity
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const ULONGLONG made = ((ULONGLONG)f.ftCreationTime.dwHighDateTime << 32) | f.ftCreationTime.dwLowDateTime;
        if (since && made + slack < since) continue;
        if (best.empty() || CompareFileTime(&f.ftCreationTime, &bt) > 0) { best = f.cFileName; bt = f.ftCreationTime; }
    } while (FindNextFileA(h, &f));
    FindClose(h);
    return best.empty() ? "" : dir + "\\" + best;
}
static std::string FindFile(const std::string& dir, const char* ext, int depth = 0) {
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string found, sub;
    do {
        const std::string n = f.cFileName;
        if (n == "." || n == "..") continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 2 && sub.empty()) sub = dir + "\\" + n; }
        else if (n.size() > std::strlen(ext) && _stricmp(n.c_str() + n.size() - std::strlen(ext), ext) == 0 && found.empty()) found = dir + "\\" + n;
    } while (FindNextFileA(h, &f));
    FindClose(h);
    if (found.empty() && !sub.empty()) found = FindFile(sub, ext, depth + 1);
    return found;
}
// the file is finished: it exists, its size stays the same, and it can be opened exclusively
// A file's real size, also while another program is writing it. (The size a folder listing gives - FindFirstFile,
// GetFileAttributesEx - is only updated now and then for a file that is open, and can say 0 for a video ffmpeg is
// in the middle of writing.) -1: can't be opened.
static long long RealSize(const std::string& p) {
    HANDLE h = CreateFileA(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz{};
    const BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return ok ? sz.QuadPart : -1;
}
static bool WaitFinished(const std::string& p, int seconds) {
    long long last = -1;
    int same = 0;
    for (int i = 0; i < seconds * 4; ++i) {
        {
            const long long sz = RealSize(p);
            same = (sz == last && sz > 0) ? same + 1 : 0;
            last = sz;
            if (same >= 4) {
                HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
            }
        }
        Sleep(250);
    }
    return false;
}
static std::string Q(const std::string& s) { return "\"" + s + "\""; }

static void Worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> g(g_jobLock);
            if (!g_jobCv.wait_for(g, std::chrono::seconds(30), [] { return !g_jobs.empty(); })) { g_worker = false; return; }
            j = g_jobs.front();
            g_jobs.pop_front();
        }
        const std::string ff = FfmpegPath();
        if (j.type == 0) {
            const std::string out = OutDir();
            SetPost("finishing " + j.label + "...");
            const std::string take = NewestTake(out, j.label, j.since);
            const std::string video = take.empty() ? "" : FindFile(take, ".mp4");
            const std::string audio = take.empty() ? "" : take + "\\audio.wav";
            // HLAE's ffmpeg finishes the video after "record end" (long clips: a while)
            if (video.empty() || !WaitFinished(video, 600)) {
                Log("record: HLAE take of %s: %s", j.label.c_str(), take.empty() ? "no take folder" : video.empty() ? "no .mp4 in it" : "the video wasn't finished in 10 minutes");
                SetPost("couldn't find the video of " + j.label + " in " + (take.empty() ? out : take) + " (is HLAE's ffmpeg set up?)");
                continue;
            }
            const std::string dst = out + "\\" + j.label + ".mp4";
            bool ok = false, joined = false;
            std::string note;
            if (!FileExists(audio)) note = " (no game sound from HLAE)";
            else if (ff.empty()) note = " (sound not joined: ffmpeg not found)";
            else if (!WaitFinished(audio, 60)) note = " (sound not joined: HLAE's audio.wav wasn't finished)";
            else {
                const std::string err = dst + ".join.log";
                const int code = Run(Q(ff) + " -y -v error -i " + Q(video) + " -i " + Q(audio) +
                                     " -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 192k -af apad -shortest -movflags +faststart " + Q(dst), err);
                joined = ok = code == 0 && FileExists(dst);
                if (joined) DeleteFileA(err.c_str());
                else note = " (picture only - joining the sound failed, see " + err + "; HLAE's files stay in " + take + ")";
            }
            if (!ok) ok = CopyFileA(video.c_str(), dst.c_str(), FALSE) != 0;
            Log("record: HLAE take of %s: %s%s", j.label.c_str(), ok ? (joined ? "picture and sound joined into " : "saved as ") : "couldn't save ",
                (dst + note).c_str());
            std::lock_guard<std::mutex> g(lock);
            if (ok) g_progress.done.push_back(dst);
            g_progress.post = ok ? "saved " + j.label + ".mp4" + note : "couldn't save " + j.label + ".mp4";
        } else if (j.type == 2) {
            // DemoTools2's recorder: files[0] the video (finished by its own ffmpeg), files[1] the sound ("" = none)
            const std::string& video = j.files[0];
            const std::string wav = j.files.size() > 1 ? j.files[1] : "";
            SetPost("finishing " + j.label + "...");
            if (!WaitFinished(video, 600)) { SetPost("the video of " + j.label + " wasn't finished (see " + video + ".ffmpeg.log)"); continue; }
            // picture and sound into one MP4 (the picture isn't encoded again)
            bool joined = false, ok = false;
            std::string note;
            if (!wav.empty() && !FileExists(wav)) note = " (no game sound: its file is missing)";
            else if (!wav.empty() && ff.empty()) note = " (no game sound: ffmpeg not found to join it)";
            else if (!wav.empty()) {
                const std::string err = j.out + ".join.log";
                const int code = Run(Q(ff) + " -y -v error -i " + Q(video) + " -i " + Q(wav) +
                                     " -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 192k -af apad -shortest -movflags +faststart " + Q(j.out), err);
                joined = code == 0 && FileExists(j.out);
                if (joined) DeleteFileA(err.c_str());
                else {
                    Log("record: joining the picture and sound of %s failed (ffmpeg exit %d, see %s)", j.label.c_str(), code, err.c_str());
                    note = " (picture only - joining the sound failed, see " + err + "; the sound is kept as " + wav + ")";
                }
            }
            if (joined) {
                ok = true;
                DeleteFileA(video.c_str());
                DeleteFileA(wav.c_str());
            } else ok = MoveFileExA(video.c_str(), j.out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
            if (wav.empty()) note = " (no game sound)";
            if (joined) Log("record: %s: picture and sound joined", j.out.c_str());
            else Log("record: %s %s%s", ok ? "saved" : "couldn't save", j.out.c_str(), note.c_str());
            std::lock_guard<std::mutex> g(lock);
            if (ok) g_progress.done.push_back(j.out);
            g_progress.post = ok ? "saved " + j.label + ".mp4" + note : "couldn't save " + j.label + ".mp4";
        } else {
            if (ff.empty()) { SetPost("no ffmpeg found - the clips are saved one by one, not joined"); continue; }
            std::vector<std::string> have;
            for (const std::string& f : j.files) if (FileExists(f)) have.push_back(f);
            if (have.empty()) { SetPost("nothing to join"); continue; }
            SetPost("joining " + std::to_string(have.size()) + " clips...");
            const std::string list = OutDir() + "\\dt2_join.txt";
            FILE* f = std::fopen(list.c_str(), "wb");
            if (!f) { SetPost("couldn't write " + list); continue; }
            for (std::string p : have) {
                std::replace(p.begin(), p.end(), '\\', '/');
                std::string esc;
                for (char c : p) { if (c == '\'') esc += "'\\''"; else esc += c; }
                std::fprintf(f, "file '%s'\n", esc.c_str());
            }
            std::fclose(f);
            const std::string err = j.out + ".join.log";
            const bool ok = Run(Q(ff) + " -y -v error -f concat -safe 0 -i " + Q(list) + " -c copy " + Q(j.out), err) == 0;
            DeleteFileA(list.c_str());
            if (ok) DeleteFileA(err.c_str());
            Log("record: joining %zu clips into %s: %s", have.size(), j.out.c_str(), ok ? "ok" : ("failed, see " + err).c_str());
            bool keepClips;
            {
                std::lock_guard<std::mutex> g(lock);
                keepClips = set.clips;
            }
            if (ok && !keepClips) for (const std::string& p : have) DeleteFileA(p.c_str());
            std::lock_guard<std::mutex> g(lock);
            if (ok) g_progress.done.push_back(j.out);
            g_progress.post = ok ? "all clips joined: " + j.out : "joining the clips failed";
        }
    }
}
static void Queue(Job j) {
    std::lock_guard<std::mutex> g(g_jobLock);
    g_jobs.push_back(std::move(j));
    g_jobCv.notify_one();
    if (!g_worker) {
        g_worker = true;
        std::thread(Worker).detach();
    }
}

// ------------------------------------------------------------------ recording (game thread)
enum class St { Idle, Setup, Seek, WaitSeek, Preroll, Rec, HlaeVerify, AudSeek, AudWait, AudPre, AudRec, Finish };
static St g_st = St::Idle;
static std::vector<Clip> g_queue;
static size_t g_cur = 0;
static ULONGLONG g_at = 0;
static int g_tries = 0, g_prevPov = 0;
static Settings g_run;              // settings copied at start
static std::string g_out;
static std::vector<std::string> g_made;
static std::atomic<bool> g_stopAsked{false};
static ULONGLONG g_takeSince = 0;   // FILETIME when the current take started
static bool g_dt2 = false;          // this recording uses DemoTools2's own recorder (else HLAE)
static bool g_manual = false;       // StartManual: from now until Stop()
static bool g_endNow = false;
static bool g_freeze = false;       // StartFreeze: demo paused, campath on its own clock       // Stop on a manual take: end it now (the demo may be paused - no later tick comes)
static ULONGLONG g_recAt = 0;       // when the current take started (GetTickCount64)
static int g_hlaeCheck = 0;         // HLAE take checks passed: 1 its folder exists, 2 it has data
static std::string g_video, g_wav;  // DemoTools2's recorder: the current clip's files
static bool g_rt = false;           // this clip: picture and sound together, in real time
static bool g_rtSound = false;      // ... and the sound capture is running
static bool g_rtMarked = false;     // ... its start is set to the first frame

static void Cmd(const std::string& c) { Game::ExecuteCommand(c.c_str()); }
static void State(const std::string& s) {
    std::lock_guard<std::mutex> g(lock);
    if (g_progress.state != s) Log("record: %s", s.c_str());
    g_progress.state = s;
    g_progress.clip = (int)g_cur + 1;
    g_progress.clips = (int)g_queue.size();
}

void Start(const std::vector<Clip>& clips) {
    std::vector<Clip> on;
    for (const Clip& c : clips) if (c.on) on.push_back(c);
    std::lock_guard<std::mutex> g(lock);
    if (g_progress.running || on.empty()) return;
    g_queue = on;
    g_cur = 0;
    g_run = set;
    g_progress = Progress{};
    g_progress.running = true;
    g_progress.clips = (int)on.size();
    g_stopAsked = false;
    g_manual = false;
    g_st = St::Setup;
}
void StartCinematic(int startTick, int endTick, const std::string& name) {
    if (startTick < 0 || endTick <= startTick) return;
    std::lock_guard<std::mutex> g(lock);
    if (g_progress.running) return;
    Clip c;
    c.start = startTick;
    c.end = endTick;
    c.round = 0;
    c.label = Safe(name);
    c.summary = "campath";
    g_queue = {c};
    g_cur = 0;
    g_run = set;
    g_run.view = 2;          // the camera stays as it is: the campath has it
    g_run.combined = false;  // one file
    g_run.clips = true;
    g_progress = Progress{};
    g_progress.running = true;
    g_progress.clips = 1;
    g_stopAsked = false;
    g_manual = false;
    g_st = St::Setup;
}
void StartFreeze(const std::string& name) {
    std::lock_guard<std::mutex> g(lock);
    if (g_progress.running) return;
    Clip c;
    c.start = -1;           // the current tick
    c.end = 0x7fffffff;     // ends when the path does
    c.label = Safe(name);
    c.summary = "freeze path";
    g_queue = {c};
    g_cur = 0;
    g_run = set;
    g_run.view = 2;          // the campath has the camera
    g_run.combined = false;
    g_run.clips = true;
    g_run.sound = false;     // the demo is paused: no sound to record (and DTRecord uses its exact fixed-step blur)
    g_progress = Progress{};
    g_progress.running = true;
    g_progress.clips = 1;
    g_stopAsked = false;
    g_manual = true;         // starts here, no seeking
    g_freeze = true;
    g_st = St::Setup;
}
void StartManual(const std::string& name) {
    std::lock_guard<std::mutex> g(lock);
    if (g_progress.running) return;
    Clip c;
    c.start = -1;           // set to the current tick when it starts
    c.end = 0x7fffffff;     // set when it stops
    c.label = Safe(name);
    c.summary = "manual";
    g_queue = {c};
    g_cur = 0;
    g_run = set;
    g_run.view = 2;          // the view as it is
    g_run.combined = false;
    g_run.clips = true;
    g_progress = Progress{};
    g_progress.running = true;
    g_progress.clips = 1;
    g_stopAsked = false;
    g_manual = true;
    g_st = St::Setup;
}
bool ManualRunning() {
    std::lock_guard<std::mutex> g(lock);
    return g_progress.running && g_manual;
}
void Stop() { g_stopAsked = true; }
Progress Status() {
    std::lock_guard<std::mutex> g(lock);
    return g_progress;
}

static void SetupHlae();
static void SetupView();
static std::string g_setupError;   // why Setup() failed
static bool Setup() {
    if (g_run.slot >= 0)   // the Steam ID from the game when the file didn't have it (or it changed slot)
        for (const auto& p : Pov::Players()) if (p.slot == g_run.slot && (!g_run.xuid || p.xuid != g_run.xuid)) g_run.xuid = p.xuid;
    g_out = OutDir();
    MakeDirs(g_out);
    std::string why;
    const std::string rec = RecorderFor(g_run.recorder, &why);
    g_dt2 = rec == "DTRecord";
    if (rec.empty()) { g_setupError = why; return false; }
    if (g_dt2) {
        if (FfmpegPath().empty()) { g_setupError = "ffmpeg.exe wasn't found - choose it under Output in the Record tab"; return false; }
        MakeDirs(g_out + "\\dt2_takes");
        Log("record: recording with DTRecord");
    } else {
        SetupHlae();
    }
    SetupView();
    g_made.clear();
    return true;
}

static void SetupHlae() {
    char b[512];
    // HLAE: our own output settings (an ffmpeg H.264 one, wrapped in a sampler for motion blur)
    Cmd("mirv_streams record screen settings afxDefault");
    Cmd("mirv_streams settings remove dt2blur");
    Cmd("mirv_streams settings remove dt2mp4");
    // 4:3 (or 5:4) game: stretch to 16:9 while encoding, like a stretched-res player sees it
    std::string vf;
    int gw, gh, sw, sh;
    if (g_run.stretch && GameSize(gw, gh) && StretchSize(gw, gh, sw, sh)) {
        char f[96];
        std::snprintf(f, sizeof f, "-vf scale=%d:%d:flags=lanczos,setsar=1 ", sw, sh);
        vf = f;
        Log("record: game is %dx%d - clips stretched to %dx%d", gw, gh, sw, sh);
    }
    std::snprintf(b, sizeof b, "%s-c:v libx264 -preset medium -crf %d -pix_fmt yuv420p -movflags +faststart {QUOTE}{AFX_OUTPUT_FILE}{QUOTE}", vf.c_str(),
                  g_run.crf);
    Cmd(std::string("mirv_streams settings add ffmpeg dt2mp4 \"") + b + "\"");
    Cmd(std::string("mirv_streams settings edit dt2mp4 options \"") + b + "\"");
    if (g_run.blur) {
        Cmd("mirv_streams settings add sampler dt2blur");
        Cmd("mirv_streams settings edit dt2blur settings dt2mp4");
        std::snprintf(b, sizeof b, "mirv_streams settings edit dt2blur fps %d", g_run.fps);
        Cmd(b);
        std::snprintf(b, sizeof b, "mirv_streams settings edit dt2blur exposure %.2f", std::clamp(g_run.blurAmount, 0.05f, 1.f));
        Cmd(b);
        Cmd("mirv_streams record screen settings dt2blur");
    } else
        Cmd("mirv_streams record screen settings dt2mp4");
    Cmd("mirv_streams record name \"" + g_out + "\"");
    Cmd("mirv_streams record screen enabled 1");
    Cmd("mirv_streams record startMovieWav 1");
}

static void SetupView() {
    g_prevPov = Pov::Mode();
    if (g_run.view == 2) {
        // campath: nothing to change (it hides the HUD / gun itself, as set in the Cinematics tab)
    } else if (g_run.view == 0) {
        Pov::LockTo(g_run.xuid);
        Pov::SetMode(1);
        Cmd("cl_drawhud 1");
    } else {
        Pov::LockTo(0);
        Pov::SetMode(0);
        Cmd("cl_drawhud 0");
        Cmd(g_run.hands ? "r_drawviewmodel 1" : "r_drawviewmodel 0");
    }
}

static void Restore() {
    Cmd("host_framerate 0");
    if (Capture::Active()) Capture::End();
    Sound::Cancel();
    g_rtSound = false;
    if (g_freeze) { Campath::StopFrozen(); g_freeze = false; }
    if (!g_dt2) Cmd("mirv_streams record screen enabled 0");
    if (g_run.view == 2) return;   // the camera / HUD were the campath's
    Cmd("cl_drawhud 1");
    Cmd("r_drawviewmodel 1");
    Pov::LockTo(0);
    Pov::SetMode(g_prevPov);
}

static void Finish(const char* why) {
    if (!g_dt2) FirstRunEnd(kHlaeStep);   // stopped before HLAE's take was checked: not a crash
    Cmd("demo_pause");
    Restore();
    if (g_run.combined && g_made.size() > 1) {   // one clip: nothing to join
        std::string name = "all_kills";
        for (const auto& p : Pov::Players()) if (p.xuid == g_run.xuid) name = Safe(p.name) + "_all_kills";
        Queue({1, "", g_made, g_out + "\\" + name + ".mp4"});
    }
    std::lock_guard<std::mutex> g(lock);
    g_progress.running = false;
    g_progress.state = why;
    Log("record: %s", why);
    g_st = St::Idle;
}

static void NextClip() {
    ++g_cur;
    g_tries = 0;
    g_st = g_cur < g_queue.size() ? St::Seek : St::Finish;
}
// any file with more than `min` bytes in dir (or one folder down)
static bool DirHasData(const std::string& dir, long long min, int depth = 0) {
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::string n = f.cFileName;
        if (n == "." || n == "..") continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 1) found = DirHasData(dir + "\\" + n, min, depth + 1); }
        else found = RealSize(dir + "\\" + n) > min;   // not the listing's size: stale while ffmpeg writes
    } while (!found && FindNextFileA(h, &f));
    FindClose(h);
    return found;
}
// HLAE took the commands but didn't record: stop (the rest would be empty too) and say to use DTRecord.
// Returns true (the caller stops).
static bool HlaeFailed(const Clip& c, const char* what) {
    FirstRunEnd(kHlaeStep);   // it didn't crash, it just didn't record
    Log("record: HLAE didn't record clip %s (%s) - its build may not be updated for this CS2", c.label.c_str(), what);
    if (g_st != St::HlaeVerify) Cmd("mirv_streams record end");
    g_hlaeBroken = true;
    SetFeature("record", "Recording", Feat_Limited, "HLAE took the commands but recorded nothing in this CS2 (not updated?) - use DTRecord.");
    Finish("stopped: HLAE isn't recording in this CS2 (not updated for it yet?) - set Recorder to DTRecord and record again");
    return true;
}

void OnFrame() {
    // players for the window, twice a second
    static ULONGLONG nextPlayers = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextPlayers) {
        nextPlayers = now + 500;
        auto ps = Pov::Players();
        std::lock_guard<std::mutex> g(g_playersLock);
        g_players = std::move(ps);
    }
    // a demo started: find its file and read its kills, and pick the watched player if none is chosen
    static bool wasPlaying = false;
    static ULONGLONG nextFind = 0;
    const bool playing = Game::IsPlayingDemo();
    if (playing && !wasPlaying) {   // give it a moment to open; forget the last demo (it may be another one now)
        nextFind = now + 1000;
        g_findFailed = false;
        std::lock_guard<std::mutex> g(lock);
        if (!g_scan.busy) { g_scan = Scan{}; set.demoPath.clear(); }
        g_autoFound.clear();
        g_chosen = false;
    }
    wasPlaying = playing;
    if (playing && nextFind && now >= nextFind && !g_finding) {
        nextFind = now + (g_findFailed ? 3000 : 10000);   // keep checking (another demo may be loaded)
        g_finding = true;
        std::thread([] {
            const std::string p = FindOpenDemo();
            static bool told = false;
            g_findFailed = p.empty();
            if (p.empty() && !told) { told = true; Log("record: couldn't find the demo's file among CS2's open files"); }
            bool scan = false;
            {
                std::lock_guard<std::mutex> g(lock);
                if (!p.empty() && p != g_autoFound && !g_chosen) {
                    g_autoFound = p;
                    scan = _stricmp(p.c_str(), g_scan.path.c_str()) != 0 || !g_scan.result.ok;
                }
            }
            if (!p.empty() && scan) {
                Log("record: CS2 is playing %s - reading its kills", p.c_str());
                StartScan(p, false);
            }
            g_finding = false;
        }).detach();
    }
    if (playing && g_st == St::Idle) {
        static ULONGLONG nextPick = 0;
        if (now >= nextPick) {
            nextPick = now + 1000;
            bool need;
            {
                std::lock_guard<std::mutex> g(lock);
                need = set.slot < 0 && g_scan.result.ok;
            }
            if (need)
                if (const uint64_t w = Pov::WatchedXuid())
                    for (const auto& p : Pov::Players())
                        if (p.xuid == w) {
                            std::lock_guard<std::mutex> g(lock);
                            set.slot = p.slot;
                            set.xuid = p.xuid;
                        }
        }
    }
    if (g_st == St::Idle) return;
    if (g_stopAsked && g_manual && g_st == St::Rec && g_cur < g_queue.size()) {
        g_stopAsked = false;   // not a cancel: the clip ends at this tick (Rec finishes it normally below)
        g_queue[g_cur].end = std::max(g_queue[g_cur].start + 1, Game::DemoTick());
        g_manual = false;
        g_endNow = true;
    }
    if (g_stopAsked) {
        if (g_cur < g_queue.size()) {   // keep the part that was already recorded
            const std::string dst = g_out + "\\" + g_queue[g_cur].label + ".mp4";
            if ((g_st == St::Rec || g_st == St::HlaeVerify) && !g_dt2) {
                if (g_st == St::Rec) Cmd("mirv_streams record end");
                Queue({0, g_queue[g_cur].label, {}, "", g_takeSince});
                g_made.push_back(dst);
            } else if (g_dt2 && g_st == St::Rec && g_rt) {   // one pass: keep what was recorded, with its sound
                if (g_rtSound) Sound::MarkEnd();
                Capture::End();
                const bool snd = g_rtSound && g_rtMarked && Sound::End(g_wav);
                if (g_rtSound && !snd) Sound::Cancel();
                g_rtSound = false;
                Queue({2, g_queue[g_cur].label, snd ? std::vector<std::string>{g_video, g_wav} : std::vector<std::string>{g_video}, dst});
                g_made.push_back(dst);
            } else if (g_dt2 && (g_st == St::Rec || g_st == St::AudSeek || g_st == St::AudWait || g_st == St::AudPre || g_st == St::AudRec)) {
                if (g_st == St::Rec) Capture::End();
                Sound::Cancel();
                Queue({2, g_queue[g_cur].label, {g_video}, dst});
                g_made.push_back(dst);
            }
        }
        Finish("stopped");
        return;
    }
    if (!Game::IsPlayingDemo()) { Finish("stopped: the demo was closed"); return; }
    const int tick = Game::DemoTick();
    if (g_cur >= g_queue.size()) {
        Finish(("done - " + std::to_string(g_made.size()) + " clip(s) recorded; the files are finished in the background").c_str());
        return;
    }
    const Clip& c = g_queue[g_cur];
    const int target = std::max(0, c.start - 192);   // 3 s before, so the players are there when recording starts
    char b[256];
    switch (g_st) {
    case St::Setup:
        if (!Setup()) { Finish(("can't record: " + g_setupError).c_str()); return; }
        g_tries = 0;
        g_endNow = false;
        if (g_freeze) Cmd("demo_pause");   // the moment stays; only the camera moves
        if (g_manual) {   // no jump: record from here
            g_queue[g_cur].start = std::max(0, tick);
            g_st = St::Preroll;
            break;
        }
        g_st = St::Seek;
        break;
    case St::Seek:
        if (g_dt2 && Capture::Active()) { State("finishing the previous clip's video..."); break; }   // its last frames
        std::snprintf(b, sizeof b, "demo_gototick %d", target);
        Cmd(b);
        Cmd("demo_resume");
        Cmd("host_framerate 0");
        g_at = now;
        g_st = St::WaitSeek;
        State(g_run.view == 2 ? std::string("going to the path's start") : "clip " + std::to_string(g_cur + 1) + ": going to round " + std::to_string(c.round));
        break;
    case St::WaitSeek:
        if (now - g_at < 700) break;
        if (tick >= target - 64 && tick < c.start) { g_st = St::Preroll; break; }
        if (now - g_at > 8000) {
            if (++g_tries <= 2) { g_st = St::Seek; break; }
            State("clip " + std::to_string(g_cur + 1) + ": couldn't get there - skipped");
            ++g_cur;
            g_tries = 0;
            g_st = St::Seek;
        }
        break;
    case St::Preroll:
        if (g_run.view != 2) Pov::SpecXuid(g_run.xuid);
        // same pass: the sound capture starts during the run-up (it takes a moment); the clip's start is set later
        g_rt = g_dt2 && g_run.sound && g_run.samePass && Sound::Available(nullptr);
        if (g_rt && !g_rtSound) {
            Cmd("host_framerate 0");
            Cmd("demo_timescale 1");
            Cmd("snd_mute_losefocus 0");   // the DemoTools2 window may have the focus: keep CS2's sound on
            std::string why;
            g_rtSound = Sound::Begin(&why);
            g_rtMarked = false;
            if (!g_rtSound) State("clip " + std::to_string(g_cur + 1) + ": no game sound (" + why + ")");
        }
        if (tick < c.start) { State("clip " + std::to_string(g_cur + 1) + ": getting ready"); break; }
        if (g_rt) Cmd("host_framerate 0");
        else {
            std::snprintf(b, sizeof b, "host_framerate %d", g_run.blur ? g_run.fps * std::max(1, g_run.samples) : g_run.fps);
            Cmd(b);
        }
        g_recAt = now;
        g_hlaeCheck = 0;
        if (g_dt2) {
            Capture::Params cp;
            cp.ffmpeg = FfmpegPath();
            g_video = g_out + "\\dt2_takes\\" + c.label + ".video.mp4";
            g_wav = g_out + "\\dt2_takes\\" + c.label + ".wav";
            cp.file = g_video;
            cp.fps = g_run.fps;
            cp.samples = g_run.blur ? std::max(1, g_run.samples) : 1;
            cp.exposure = std::clamp(g_run.blurAmount, 0.05f, 1.f);
            cp.crf = g_run.crf;
            cp.stretch = g_run.stretch;
            cp.realtime = g_rt;
            std::string why;
            if (!Capture::Begin(cp, &why)) { Finish(("can't record: " + why).c_str()); return; }
        } else {
            Cmd("mirv_streams record take \"" + c.label + "_\"");
            g_takeSince = FileTimeNow();
            FirstRunBegin(kHlaeStep);   // until HLAE has written a take that has data
            Cmd("mirv_streams record start");
        }
        if (g_freeze)   // each recorded frame (or sample) moves the path on by its share of a second
            Campath::StartFrozen(g_rt ? 0.0 : 1.0 / (g_run.blur ? g_run.fps * std::max(1, g_run.samples) : std::max(1, g_run.fps)));
        g_st = St::Rec;
        State(g_freeze ? std::string("recording the path with the demo paused") : g_manual ? std::string("recording - Stop ends it") : g_run.view == 2 ? std::string("recording the path") : "clip " + std::to_string(g_cur + 1) + ": recording (round " + std::to_string(c.round) + ", " + c.summary + ")");
        break;
    case St::Rec:
        if (g_run.view != 2) Pov::SpecXuid(g_run.xuid);
        if (g_freeze && Campath::FrozenDone()) g_endNow = true;   // the path reached its last key
        if (g_dt2) {
            const std::string err = Capture::Error();
            if (!err.empty()) { Finish(("stopped: " + err).c_str()); return; }
            if (now - g_recAt > 6000 && !Capture::SeenFrames()) {
                Finish("stopped: no picture came from DirectX 11 (is CS2 in Vulkan mode?)");
                return;
            }
            if (g_rt && g_rtSound && !g_rtMarked)
                if (const uint64_t t0 = Capture::StartTime()) { Sound::MarkStartAt(t0); g_rtMarked = true; }   // the video's 0
            if (tick < c.end && !g_endNow) break;
            if (g_rt) {   // one pass: picture and sound end together, then they are joined in the background
                if (g_rtSound) Sound::MarkEnd();
                Capture::End();
                const bool snd = g_rtSound && g_rtMarked && Sound::End(g_wav);
                if (g_rtSound && !snd) Sound::Cancel();
                g_rtSound = false;
                Queue({2, c.label, snd ? std::vector<std::string>{g_video, g_wav} : std::vector<std::string>{g_video}, g_out + "\\" + c.label + ".mp4"});
                g_made.push_back(g_out + "\\" + c.label + ".mp4");
                NextClip();
                break;
            }
            Capture::End();
            Cmd("host_framerate 0");
            if (g_run.sound && Sound::Available(nullptr)) { g_tries = 0; g_st = St::AudSeek; break; }
            Queue({2, c.label, {g_video}, g_out + "\\" + c.label + ".mp4"});
            g_made.push_back(g_out + "\\" + c.label + ".mp4");
            NextClip();
            break;
        }
        // HLAE: is it really recording? (an HLAE build not updated for this CS2 can load and take commands but record
        // nothing) - its take folder must appear, and have data by the end of the clip
        if (g_hlaeCheck == 0 && (now - g_recAt > 4000 || tick >= c.end)) {
            if (!NewestTake(g_out, c.label, g_takeSince).empty()) g_hlaeCheck = 1;
            else if (HlaeFailed(c, "no take folder")) return;
            else g_hlaeCheck = 1;
        }
        if (tick < c.end && !g_endNow) break;
        Cmd("mirv_streams record end");
        Cmd("host_framerate 0");
        if (g_hlaeCheck == 1) {
            const std::string take = NewestTake(g_out, c.label, g_takeSince);
            if (!take.empty() && DirHasData(take, 0)) { g_hlaeCheck = 2; g_hlaeBroken = false; FirstRunEnd(kHlaeStep); }
            else {   // HLAE's ffmpeg may still be catching up: give it time to write before calling it broken
                Cmd("demo_pause");
                g_at = now;
                g_st = St::HlaeVerify;
                State("clip " + std::to_string(g_cur + 1) + ": waiting for HLAE to write the video");
                break;
            }
        }
        Queue({0, c.label, {}, "", g_takeSince});
        g_made.push_back(g_out + "\\" + c.label + ".mp4");
        NextClip();
        break;
    case St::HlaeVerify: {
        const std::string take = NewestTake(g_out, c.label, g_takeSince);
        if (!take.empty() && DirHasData(take, 0)) {
            Log("record: HLAE's video of %s appeared %.1f s after the clip ended", c.label.c_str(), (now - g_at) / 1000.0);
            g_hlaeCheck = 2;
            g_hlaeBroken = false;
            FirstRunEnd(kHlaeStep);
            Queue({0, c.label, {}, "", g_takeSince});
            g_made.push_back(g_out + "\\" + c.label + ".mp4");
            NextClip();
            break;
        }
        if (now - g_at < 30000) break;
        if (HlaeFailed(c, take.empty() ? "no take folder" : "nothing written in the take folder 30 s after the clip")) return;
        break;
    }
    // ---- DemoTools2's recorder, second pass: the same clip in real time, for the sound only
    case St::AudSeek:
        std::snprintf(b, sizeof b, "demo_gototick %d", target);
        Cmd(b);
        Cmd("demo_resume");
        Cmd("host_framerate 0");
        Cmd("demo_timescale 1");
        Cmd("snd_mute_losefocus 0");   // the DemoTools2 window may have the focus: keep CS2's sound on
        {
            std::string why;
            if (!Sound::Begin(&why)) {
                State("clip " + std::to_string(g_cur + 1) + ": no game sound (" + why + ")");
                Queue({2, c.label, {g_video}, g_out + "\\" + c.label + ".mp4"});
                g_made.push_back(g_out + "\\" + c.label + ".mp4");
                NextClip();
                break;
            }
        }
        g_at = now;
        g_st = St::AudWait;
        State(g_run.view == 2 ? std::string("recording the sound (plays the same part again in real time)") : "clip " + std::to_string(g_cur + 1) + ": recording the sound (plays in real time)");
        break;
    case St::AudWait:
        if (now - g_at < 700) break;
        if (tick >= target - 64 && tick < c.start) { g_st = St::AudPre; break; }
        if (now - g_at > 8000) {
            if (++g_tries <= 2) { Sound::Cancel(); g_st = St::AudSeek; break; }
            Sound::Cancel();
            Queue({2, c.label, {g_video}, g_out + "\\" + c.label + ".mp4"});
            g_made.push_back(g_out + "\\" + c.label + ".mp4");
            NextClip();
        }
        break;
    case St::AudPre:
        if (g_run.view != 2) Pov::SpecXuid(g_run.xuid);
        if (tick < c.start) break;
        Sound::MarkStart();
        g_st = St::AudRec;
        break;
    case St::AudRec:
        if (g_run.view != 2) Pov::SpecXuid(g_run.xuid);
        if (tick < c.end) break;
        Sound::MarkEnd();
        {
            const bool snd = Sound::End(g_wav);
            Queue({2, c.label, snd ? std::vector<std::string>{g_video, g_wav} : std::vector<std::string>{g_video}, g_out + "\\" + c.label + ".mp4"});
        }
        g_made.push_back(g_out + "\\" + c.label + ".mp4");
        NextClip();
        break;
    case St::Finish:
        Finish("done");
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------ console
static void CmdRecord(const Console::Args& a) {
    const std::string sub = a.size() > 1 ? a[1] : "";
    if (sub == "scan" && a.size() > 2) {
        std::string p = a[2];
        for (size_t i = 3; i < a.size(); ++i) p += " " + a[i];
        StartScan(p);
        Console::Print("dt2_record: scanning %s", p.c_str());
    } else if (sub == "start") {
        StartManual(a.size() > 2 ? a[2] : "recording");
        Console::Print("dt2_record: recording from here - dt2_record stop ends it");
    } else if (sub == "stop") {
        Stop();
        Console::Print("dt2_record: stopping");
    } else {
        const Progress p = Status();
        Console::Print("dt2_record: %s %s", p.running ? "recording -" : "", p.state.c_str());
        Console::Print("  The Record tab (Insert) does everything. dt2_record start [name] | stop | scan <demo path>");
    }
}
void Init() {
    Console::Add("dt2_record", "DemoTools2 recorder: dt2_record start [name] | stop | scan <demo>", &CmdRecord);
    // HLAE only loads DemoTools2: nothing else needs HLAE's own features to work in this CS2 (recording can use
    // DTRecord instead)
    if (HlaeLoaded() && CrashedLastTime(kHlaeStep))
        SetFeature("hlae", "HLAE", Feat_Limited, "CS2 crashed last time while HLAE started recording (HLAE not updated for this CS2?) - the recorder starts on DTRecord.");
    else if (HlaeLoaded()) SetFeature("hlae", "HLAE", Feat_Ok, "Loaded. Only HLAE's recording depends on HLAE being updated - DTRecord doesn't.");
    else SetFeature("hlae", "HLAE", Feat_Ok, "Not loaded: record with DTRecord.");
    Capture::Install();
    LoadSettings();
    std::string why;
    const bool own = Capture::Available(&why);
    if (own) SetFeature("record", "Recording", Feat_Ok, HlaeLoaded() ? "HLAE or DTRecord - choose under Recorder in the Record tab." : "DTRecord (HLAE isn't loaded).");
    else if (HlaeLoaded()) SetFeature("record", "Recording", Feat_Limited, "HLAE only: DTRecord isn't available (" + why + ").");
    else SetFeature("record", "Recording", Feat_Off, "HLAE isn't loaded and " + why + ".");
}

}  // namespace Record
