// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// DTRecord, DemoTools2's own recorder - see capture.h. Independent of HLAE: it reads CS2's frames from its DirectX 11 swap
// chain and its sound from Windows' per-process loopback.
#include "capture.h"
#include "util.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Capture {

// ------------------------------------------------------------------ ffmpeg writer (one per recording)
// Raw BGR frames go into ffmpeg's stdin on its own thread, so the game only waits when ffmpeg falls behind.
static std::atomic<int> g_writers{0};

struct Writer {
    HANDLE proc = nullptr, in = nullptr;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> q;
    bool closed = false;
    std::atomic<bool> failed{false};
    std::atomic<long long> frames{0};
    std::string file, log;
    size_t ahead = 6;   // frames that may wait for ffmpeg before the game waits

    bool Start(const Params& p, int w, int h, std::string* why) {
        file = p.file;
        log = p.file + ".ffmpeg.log";
        if (p.realtime) ahead = 30;   // short encoder hiccups don't hold the game up
        std::string vf;
        if (p.stretch && w > 0 && h > 0 && (double)w / h <= 1.40) {
            char b[96];
            std::snprintf(b, sizeof b, " -vf scale=%d:%d:flags=lanczos,setsar=1", ((int)std::lround(h * 16.0 / 9.0) + 1) & ~1, h & ~1);
            vf = b;
        } else if ((w & 1) || (h & 1)) {
            vf = " -vf crop=trunc(iw/2)*2:trunc(ih/2)*2";   // H.264 4:2:0 needs even sizes
        }
        char args[256];
        std::snprintf(args, sizeof args, " -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgr24 -video_size %dx%d -framerate %d -i -", w, h, p.fps);
        char enc[160];
        // real time: the encoder must keep up with the game, or the game waits for it
        std::snprintf(enc, sizeof enc, " -c:v libx264 -preset %s -crf %d -pix_fmt yuv420p -movflags +faststart ", p.realtime ? "veryfast" : "medium", p.crf);
        std::string cmd = "\"" + p.ffmpeg + "\"" + args + vf + enc + "\"" + p.file + "\"";

        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 8 << 20)) { if (why) *why = "couldn't create a pipe to ffmpeg"; return false; }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        HANDLE errFile = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOA si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = errFile != INVALID_HANDLE_VALUE ? errFile : nullptr;
        si.hStdError = errFile != INVALID_HANDLE_VALUE ? errFile : nullptr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
        CloseHandle(rd);
        if (errFile != INVALID_HANDLE_VALUE) CloseHandle(errFile);
        if (!ok) {
            CloseHandle(wr);
            if (why) *why = "couldn't start ffmpeg (" + p.ffmpeg + ")";
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        in = wr;
        Log("capture: ffmpeg started for %s (%dx%d, %d fps)", file.c_str(), w, h, p.fps);
        ++g_writers;
        std::thread([this] { Run(); }).detach();
        return true;
    }
    // render thread; waits while ffmpeg is 6 frames behind (the game simply runs slower then)
    void Push(std::vector<uint8_t>&& f) {
        std::unique_lock<std::mutex> g(m);
        cv.wait_for(g, std::chrono::seconds(20), [this] { return q.size() < ahead || failed; });
        if (failed) return;
        q.push_back(std::move(f));
        cv.notify_all();
    }
    void Close() {
        std::lock_guard<std::mutex> g(m);
        closed = true;
        cv.notify_all();
    }
    void Run() {
        for (;;) {
            std::vector<uint8_t> f;
            {
                std::unique_lock<std::mutex> g(m);
                cv.wait(g, [this] { return !q.empty() || closed; });
                if (q.empty()) break;
                f = std::move(q.front());
                q.pop_front();
                cv.notify_all();
            }
            if (failed) continue;
            size_t off = 0;
            while (off < f.size()) {
                DWORD n = 0;
                if (!WriteFile(in, f.data() + off, (DWORD)std::min<size_t>(f.size() - off, 1 << 24), &n, nullptr) || !n) {
                    failed = true;
                    Log("capture: ffmpeg stopped taking frames (see %s)", log.c_str());
                    break;
                }
                off += n;
            }
            if (!failed) ++frames;
        }
        CloseHandle(in);
        DWORD code = 1;
        if (WaitForSingleObject(proc, 10 * 60 * 1000) == WAIT_TIMEOUT) TerminateProcess(proc, 1);
        else GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (code == 0 && !failed) DeleteFileA(log.c_str());
        Log("capture: %s %s (%lld frames)", file.c_str(), code == 0 && !failed ? "finished" : "FAILED - see the .ffmpeg.log next to it", frames.load());
        --g_writers;
        delete this;
    }
};
int WritersRunning() { return g_writers.load(); }

// ------------------------------------------------------------------ the recording (render thread)
struct Rec {
    unsigned id = 0;
    Params p;
    Writer* w = nullptr;
    IDXGISwapChain* sc = nullptr;    // the game's (compared only, not kept alive)
    ID3D11Device* dev = nullptr;
    ID3D11Texture2D* staging[3] = {};
    ID3D11Texture2D* resolve = nullptr;   // multisampled back buffer
    UINT width = 0, height = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    long long copied = 0, processed = 0;
    int groupIn = 0;                  // samples blended into acc so far in this group
    std::vector<uint16_t> acc;        // per output frame: sum of the blended samples (BGR)
    std::vector<uint8_t> row;
    bool stopping = false;
    // realtime
    uint64_t times[3] = {};           // when each copy in the ring was shown
    uint64_t t0 = 0;                  // the first one: the video's 0
    long long outIdx = 0;             // output frame being filled
    std::vector<uint8_t> lastOut;     // repeated when the game showed no frame in an output frame's time
    long long repeated = 0, blended = 0;
};
static std::mutex g_lock;                 // Rec between the game thread (Begin/End) and the render thread
static std::atomic<bool> g_on{false};     // a recording exists (cheap check in Present)
static Rec* g_rec = nullptr;
static unsigned g_nextId = 1;
static std::string g_error;
static std::atomic<long long> g_lastFrames{0};
static std::atomic<bool> g_hooked{false}, g_seen{false};
static std::atomic<uint64_t> g_startTime{0};

uint64_t Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((long double)c.QuadPart * 10000000.0L / f.QuadPart);
}
uint64_t StartTime() { return g_startTime.load(); }

static void Release(Rec* r) {
    for (auto*& s : r->staging) if (s) { s->Release(); s = nullptr; }
    if (r->resolve) { r->resolve->Release(); r->resolve = nullptr; }
    if (r->dev) { r->dev->Release(); r->dev = nullptr; }
}
static void Fail(Rec* r, const std::string& why) {
    if (g_error.empty()) { g_error = why; Log("capture: %s", why.c_str()); }
    r->stopping = true;
}

static inline uint8_t Half8(uint16_t h) {   // half float (linear) -> 8-bit sRGB-ish (clamped, gamma 2.2)
    const int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.f : e == 31 ? 1.f : std::ldexp(1.f + m / 1024.f, e - 15);
    if (h & 0x8000) v = 0;
    v = std::pow(std::min(v, 1.f), 1.f / 2.2f);
    return (uint8_t)(v * 255.f + 0.5f);
}
// one mapped row -> BGR8
static bool RowToBgr(DXGI_FORMAT f, const uint8_t* s, uint8_t* d, UINT w) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        for (UINT x = 0; x < w; ++x) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; s += 4; d += 3; }
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (UINT x = 0; x < w; ++x) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; s += 4; d += 3; }
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        for (UINT x = 0; x < w; ++x) {
            uint32_t v;
            std::memcpy(&v, s, 4);
            d[2] = (uint8_t)((v & 1023) >> 2); d[1] = (uint8_t)(((v >> 10) & 1023) >> 2); d[0] = (uint8_t)(((v >> 20) & 1023) >> 2);
            s += 4; d += 3;
        }
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (UINT x = 0; x < w; ++x) {
            uint16_t c[3];
            std::memcpy(c, s, 6);
            d[2] = Half8(c[0]); d[1] = Half8(c[1]); d[0] = Half8(c[2]);
            s += 8; d += 3;
        }
        return true;
    default:
        return false;
    }
}

// The oldest copy in the ring: read it and blend it in.
static void Process(Rec* r, ID3D11DeviceContext* ctx, int slot) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(r->staging[slot], 0, D3D11_MAP_READ, 0, &m))) { Fail(r, "couldn't read a frame back from the GPU"); return; }
    const int N = std::max(1, r->p.samples);
    const int k = std::clamp((int)std::lround(r->p.exposure * N), 1, N);   // blended samples per output frame
    const int sub = (int)(r->processed % N);
    ++r->processed;
    const UINT W = r->width, H = r->height;
    const size_t px = (size_t)W * H * 3;
    if (sub >= N - k) {
        if (r->acc.size() != px) r->acc.assign(px, 0);
        r->row.resize((size_t)W * 3);
        const bool first = r->groupIn == 0;
        for (UINT y = 0; y < H; ++y) {
            if (!RowToBgr(r->fmt, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, r->row.data(), W)) {
                ctx->Unmap(r->staging[slot], 0);
                Fail(r, "the game's picture format isn't supported (" + std::to_string((int)r->fmt) + ")");
                return;
            }
            uint16_t* a = r->acc.data() + (size_t)y * W * 3;
            const uint8_t* b = r->row.data();
            if (first) for (size_t i = 0; i < (size_t)W * 3; ++i) a[i] = b[i];
            else for (size_t i = 0; i < (size_t)W * 3; ++i) a[i] += b[i];
        }
        ++r->groupIn;
    }
    ctx->Unmap(r->staging[slot], 0);
    if (sub == N - 1 && r->groupIn > 0) {
        std::vector<uint8_t> out(px);
        const int n = r->groupIn;
        if (n == 1) for (size_t i = 0; i < px; ++i) out[i] = (uint8_t)r->acc[i];
        else for (size_t i = 0; i < px; ++i) out[i] = (uint8_t)((r->acc[i] + n / 2) / n);
        r->groupIn = 0;
        r->w->Push(std::move(out));
        g_lastFrames = r->w->frames.load();
        if (r->w->failed) Fail(r, "ffmpeg stopped (see the .ffmpeg.log next to the clip)");
    }
}

// ---- real time: output frame k covers [t0 + k/fps, t0 + (k+1)/fps). It gets the game frames shown in the last
// `exposure` part of that time blended (motion blur), or without blur the last frame shown in it; when the game
// showed none (slower than the output frame rate), the previous output frame again.
static void EmitRT(Rec* r) {
    const size_t px = (size_t)r->width * r->height * 3;
    std::vector<uint8_t> out;
    if (r->groupIn > 0) {
        out.resize(px);
        const int n = r->groupIn;
        if (n == 1) for (size_t i = 0; i < px; ++i) out[i] = (uint8_t)r->acc[i];
        else for (size_t i = 0; i < px; ++i) out[i] = (uint8_t)((r->acc[i] + n / 2) / n);
        if (n > 1) ++r->blended;
        r->groupIn = 0;
        r->lastOut = out;
    } else if (!r->lastOut.empty()) {
        out = r->lastOut;
        ++r->repeated;
    } else return;   // nothing shown yet
    r->w->Push(std::move(out));
    g_lastFrames = r->w->frames.load();
    if (r->w->failed) Fail(r, "ffmpeg stopped (see the .ffmpeg.log next to the clip)");
}
// copy `slot` (shown at r->times[slot]); next: when the copy after it was shown (0 = none yet: the last one)
static void ProcessRT(Rec* r, ID3D11DeviceContext* ctx, int slot, uint64_t next) {
    const double period = 10000000.0 / std::max(1, r->p.fps);
    const uint64_t t = r->times[slot];
    if (t < r->t0) return;
    const long long k = (long long)((t - r->t0) / period);
    while (r->outIdx < k && g_error.empty()) { EmitRT(r); ++r->outIdx; }
    const bool lastInFrame = !next || (long long)((next - r->t0) / period) > k;
    const bool blur = r->p.samples > 1;
    const bool inShutter = blur && (double)(t - r->t0) >= (k + 1 - std::clamp((double)r->p.exposure, 0.05, 1.0)) * period;
    if (!inShutter && !(lastInFrame && r->groupIn == 0) && blur) return;
    if (!blur && !lastInFrame) return;   // a later frame of this output frame will be used
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(r->staging[slot], 0, D3D11_MAP_READ, 0, &m))) { Fail(r, "couldn't read a frame back from the GPU"); return; }
    const UINT W = r->width, H = r->height;
    const size_t px = (size_t)W * H * 3;
    if (r->acc.size() != px) r->acc.assign(px, 0);
    r->row.resize((size_t)W * 3);
    const bool first = r->groupIn == 0 || !blur;
    for (UINT y = 0; y < H; ++y) {
        if (!RowToBgr(r->fmt, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, r->row.data(), W)) {
            ctx->Unmap(r->staging[slot], 0);
            Fail(r, "the game's picture format isn't supported (" + std::to_string((int)r->fmt) + ")");
            return;
        }
        uint16_t* a = r->acc.data() + (size_t)y * W * 3;
        const uint8_t* b = r->row.data();
        if (first) for (size_t i = 0; i < (size_t)W * 3; ++i) a[i] = b[i];
        else for (size_t i = 0; i < (size_t)W * 3; ++i) a[i] += b[i];
    }
    ctx->Unmap(r->staging[slot], 0);
    r->groupIn = blur ? r->groupIn + 1 : 1;
    if (r->groupIn >= 255) { EmitRT(r); ++r->outIdx; }   // 16-bit sums: never more than 255 frames in one
}

static bool IsGameWindow(HWND w) {
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return false;
    WCHAR title[32] = {};
    InternalGetWindowText(GetAncestor(w, GA_ROOT), title, 32);
    return wcsncmp(title, L"DemoTools2", 10) != 0;   // DemoTools2's own window (OpenGL may present through DXGI)
}

static void Finish(Rec* r, ID3D11DeviceContext* ctx) {
    // the copies still in the ring, oldest first
    if (ctx && g_error.empty() && r->p.realtime) {
        for (long long i = std::max(r->processed, r->copied - 2); i < r->copied && g_error.empty(); ++i)
            ProcessRT(r, ctx, (int)(i % 3), i + 1 < r->copied ? r->times[(i + 1) % 3] : 0);
        if (g_error.empty() && r->w) EmitRT(r);
        Log("capture: real time - %lld frames from the game blended, %lld repeated (the game showed none in their time)",
            r->blended, r->repeated);
    } else if (ctx && g_error.empty())
        for (long long i = std::max(r->processed, r->copied - 2); i < r->copied && g_error.empty(); ++i) Process(r, ctx, (int)(i % 3));
    if (r->w) r->w->Close();   // the writer finishes and deletes itself
    r->w = nullptr;
    Release(r);
}

static void OnPresent(IDXGISwapChain* sc) {
    if (!g_on.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> g(g_lock);
    Rec* r = g_rec;
    if (!r) return;
    if (r->sc != sc) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (FAILED(sc->GetDesc(&d)) || !IsGameWindow(d.OutputWindow)) return;
        if (r->sc && r->copied) { Fail(r, "the game changed its window during the recording"); }
        r->sc = sc;
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (r->dev) { r->dev->GetImmediateContext(&ctx); }
    if (r->stopping) {
        Finish(r, ctx);
        if (ctx) ctx->Release();
        delete r;
        g_rec = nullptr;
        g_on = false;
        return;
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) {
        if (ctx) ctx->Release();
        Fail(r, "the game isn't drawing with DirectX 11 (Vulkan?) - DTRecord needs DirectX 11");
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    if (!r->dev) {
        bb->GetDevice(&dev);
        r->dev = dev;
        r->dev->GetImmediateContext(&ctx);
        r->width = td.Width;
        r->height = td.Height;
        r->fmt = td.Format;
        uint8_t probe[16] = {};
        uint8_t out[12];
        if (!RowToBgr(r->fmt, probe, out, 1)) { bb->Release(); ctx->Release(); Fail(r, "the game's picture format isn't supported (" + std::to_string((int)td.Format) + ")"); return; }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc = {1, 0};
        for (auto*& s : r->staging)
            if (FAILED(r->dev->CreateTexture2D(&sd, nullptr, &s))) { bb->Release(); ctx->Release(); Fail(r, "couldn't make the read-back textures"); return; }
        if (td.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC rd = td;
            rd.SampleDesc = {1, 0};
            rd.BindFlags = 0;
            rd.CPUAccessFlags = 0;
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.MiscFlags = 0;
            if (FAILED(r->dev->CreateTexture2D(&rd, nullptr, &r->resolve))) { bb->Release(); ctx->Release(); Fail(r, "couldn't make the resolve texture"); return; }
        }
        r->w = new Writer();
        std::string why;
        if (!r->w->Start(r->p, (int)r->width, (int)r->height, &why)) { delete r->w; r->w = nullptr; bb->Release(); ctx->Release(); Fail(r, why); return; }
    } else if ([&] { ID3D11Device* d2 = nullptr; bb->GetDevice(&d2); const bool other = d2 != r->dev; if (d2) d2->Release(); return other; }()) {
        bb->Release();
        if (ctx) ctx->Release();
        Fail(r, "the game restarted its renderer during the recording");
        return;
    } else if (td.Width != r->width || td.Height != r->height || td.Format != r->fmt) {
        bb->Release();
        if (ctx) ctx->Release();
        Fail(r, "the game's resolution changed during the recording");
        return;
    }
    if (r->resolve) {
        ctx->ResolveSubresource(r->resolve, 0, bb, 0, td.Format);
        ctx->CopyResource(r->staging[r->copied % 3], r->resolve);
    } else {
        ctx->CopyResource(r->staging[r->copied % 3], bb);
    }
    bb->Release();
    if (r->p.realtime) {
        const uint64_t t = Now();
        r->times[r->copied % 3] = t;
        if (!r->copied) { r->t0 = t; g_startTime = t; }
        ++r->copied;
        if (r->copied >= 3) {
            const long long i = r->copied - 3;
            ProcessRT(r, ctx, (int)(i % 3), r->times[(i + 1) % 3]);
            r->processed = i + 1;
        }
        ctx->Release();
        return;
    }
    ++r->copied;
    if (r->copied >= 3) Process(r, ctx, (int)((r->copied - 3) % 3));
    ctx->Release();
}

// ------------------------------------------------------------------ the hook
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentFn g_present = nullptr;
static Present1Fn g_present1 = nullptr;

static void Seen(IDXGISwapChain* sc) {
    if (g_seen.load(std::memory_order_relaxed)) return;
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d)) && IsGameWindow(d.OutputWindow)) { g_seen = true; Log("capture: the game presents through DirectX 11 - DTRecord can be used"); }
}
static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        Seen(sc);
        OnPresent(sc);
    }
    return g_present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        Seen(sc);
        OnPresent(sc);
    }
    return g_present1(sc, sync, flags, pp);
}

static bool PatchSlot(void** vt, int idx, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

// ------------------------------------------------------------------ finding CS2's swap chain
// DTRecord sits in front of IDXGISwapChain::Present by changing one entry of the swap chain's function table (it lives
// in dxgi.dll and is shared by every swap chain). To reach that table it needs a swap chain.
//
// It must NOT make one of its own while HLAE is loaded: HLAE hooks D3D11CreateDevice and the DXGI factory's
// CreateSwapChain for the whole process, so a test device and swap chain made by DemoTools2 become "the game's" for
// HLAE - and once DemoTools2 releases them, HLAE uses a freed swap chain and CS2 crashes (in AfxHookSource2.dll,
// while Panorama draws). So with HLAE loaded, DemoTools2 only looks for the swap chain CS2 already has: a pointer
// (in HLAE's or the render system's data, or one step further) to an object whose function table is in dxgi.dll and
// whose Present has been hooked (HLAE hooks the game's). Nothing is called on an object before it passes those checks, and nothing
// is created.
struct MemRange { uintptr_t a, b; };
static std::vector<MemRange> g_mem;   // committed, readable memory (a snapshot, made once)
static void SnapshotMemory() {
    g_mem.clear();
    uintptr_t p = 0x10000;
    MEMORY_BASIC_INFORMATION m;
    while (p < 0x7FFFFFFF0000ull && VirtualQuery((void*)p, &m, sizeof m)) {
        const DWORD pr = m.Protect & 0xff;
        const bool readable = m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD) &&
                              (pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READ ||
                               pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY);
        const uintptr_t a = (uintptr_t)m.BaseAddress, b = a + m.RegionSize;
        if (readable) {
            if (!g_mem.empty() && g_mem.back().b == a) g_mem.back().b = b;
            else g_mem.push_back({a, b});
        }
        if (b <= p) break;
        p = b;
    }
}
static bool CanRead(uintptr_t p, size_t n) {
    auto it = std::upper_bound(g_mem.begin(), g_mem.end(), p, [](uintptr_t v, const MemRange& r) { return v < r.a; });
    if (it == g_mem.begin()) return false;
    --it;
    return p >= it->a && p + n <= it->b;
}
struct Image { uintptr_t a = 0, b = 0; };
static Image ImageOf(const char* name) {
    Image im;
    HMODULE m = GetModuleHandleA(name);
    if (!m) return im;
    auto nt = (IMAGE_NT_HEADERS64*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    im.a = (uintptr_t)m;
    im.b = im.a + nt->OptionalHeader.SizeOfImage;
    return im;
}
static bool In(const Image& im, uintptr_t p) { return p >= im.a && p < im.b; }
// Follows jmp rel32 / jmp [rip+x] (what a code hook puts at a function's start) a few times; where does it end?
static uintptr_t JumpTarget(uintptr_t code) {
    for (int i = 0; i < 4; ++i) {
        if (!CanRead(code, 14)) return code;
        const uint8_t* c = (const uint8_t*)code;
        if (c[0] == 0xE9) code = code + 5 + *(const int32_t*)(c + 1);
        else if (c[0] == 0xFF && c[1] == 0x25) {
            const uintptr_t slot = code + 6 + *(const int32_t*)(c + 2);
            if (!CanRead(slot, 8)) return code;
            code = *(const uintptr_t*)slot;
        } else return code;
    }
    return code;
}
// Is `obj` a DXGI swap chain whose Present is hooked (by HLAE; maybe also by an overlay on top)? Only reads memory.
// HLAE hooks exactly that function of the game's swap chain, so a dxgi.dll object whose function 8 jumps out of
// dxgi.dll is it.
static bool LooksLikeHookedSwapChain(uintptr_t obj, const Image& dxgi, const Image& hlae) {
    (void)hlae;
    if (!CanRead(obj, 8)) return false;
    const uintptr_t vt = *(const uintptr_t*)obj;
    if (!In(dxgi, vt) || !CanRead(vt, 23 * 8)) return false;
    const uintptr_t* v = (const uintptr_t*)vt;
    for (int i = 0; i < 3; ++i) if (!In(dxgi, v[i])) return false;   // IUnknown: QueryInterface, AddRef, Release in dxgi
    const uintptr_t present = v[8];
    if (!In(dxgi, present)) return false;
    const uintptr_t to = JumpTarget(present);
    return to != present && !In(dxgi, to);
}
static IDXGISwapChain* FindGameSwapChain() {
    const Image dxgi = ImageOf("dxgi.dll"), hlae = ImageOf("AfxHookSource2.dll");
    if (!dxgi.a || !hlae.a) return nullptr;
    SnapshotMemory();
    auto scanData = [&](const char* module, int depth) -> uintptr_t {
        HMODULE m = GetModuleHandleA(module);
        if (!m) return 0;
        auto base = (uint8_t*)m;
        auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (std::memcmp(sec[i].Name, ".data", 5)) continue;
            const uintptr_t a = (uintptr_t)base + sec[i].VirtualAddress, n = sec[i].Misc.VirtualSize / 8;
            if (!CanRead(a, n * 8)) continue;
            const uintptr_t* q = (const uintptr_t*)a;
            for (uintptr_t k = 0; k < n; ++k) {
                const uintptr_t p = q[k];
                if (p < 0x10000 || (p & 7)) continue;
                if (LooksLikeHookedSwapChain(p, dxgi, hlae)) return p;
                if (depth < 2 || !CanRead(p, 0x800)) continue;
                const uintptr_t* o = (const uintptr_t*)p;   // one step further: a field of the object it points to
                for (int f = 0; f < 0x800 / 8; ++f)
                    if (o[f] >= 0x10000 && !(o[f] & 7) && LooksLikeHookedSwapChain(o[f], dxgi, hlae)) return o[f];
            }
        }
        return 0;
    };
    uintptr_t sc = scanData("AfxHookSource2.dll", 1);
    if (!sc) sc = scanData("rendersystemdx11.dll", 2);
    return (IDXGISwapChain*)sc;
}

static bool HookTable(IDXGISwapChain* sc) {
    void** vt = *(void***)sc;
    bool ok = Readable(vt, 9 * sizeof(void*)) && PatchSlot(vt, 8, (void*)&PresentHook, (void**)&g_present);
    IDXGISwapChain1* sc1 = nullptr;
    if (ok && SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
        void** vt1 = *(void***)sc1;
        if (Readable(vt1, 23 * sizeof(void*))) PatchSlot(vt1, 22, (void*)&Present1Hook, (void**)&g_present1);
        sc1->Release();
    }
    return ok;
}

// Without HLAE: a hidden window and a tiny test device; their swap chain shares the game's function table.
static bool HookWithProbe() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "DT2CaptureProbe";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("DT2CaptureProbe", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    if (hw) {
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferCount = 1;
        d.BufferDesc.Width = 64;
        d.BufferDesc.Height = 64;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.OutputWindow = hw;
        d.SampleDesc.Count = 1;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* sc = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr) && sc) ok = HookTable(sc);
        else Log("capture: couldn't create a DirectX 11 test device (0x%08lx)", (unsigned long)hr);
        if (ctx) ctx->Release();
        if (dev) dev->Release();
        if (sc) sc->Release();
        DestroyWindow(hw);
    }
    UnregisterClassA("DT2CaptureProbe", wc.hInstance);
    return ok;
}

bool Install() {
    if (g_hooked) return true;
    if (CrashedLastTime("capture-install")) {
        SetFeature("capture", "DTRecord", Feat_Off, "Crashed last time - off for this session.");
        return false;
    }
    if (GetModuleHandleA("rendersystemvulkan.dll") && !GetModuleHandleA("rendersystemdx11.dll")) {
        Log("capture: CS2 runs in Vulkan mode - DTRecord needs DirectX 11");
        SetFeature("capture", "DTRecord", Feat_Off, "CS2 runs in Vulkan mode: DTRecord needs DirectX 11 (CS2's default). Record with HLAE.");
        return false;
    }
    Step step("capture-install");
    FirstRunBegin("capture-install");
    bool ok = false;
    const char* how = "";
    if (GetModuleHandleA("AfxHookSource2.dll")) {
        // HLAE loaded: never make a device or swap chain (see above) - use the game's own.
        const double t0 = NowMs();
        if (IDXGISwapChain* sc = FindGameSwapChain()) {
            ok = HookTable(sc);
            how = "the game's own swap chain";
        }
        Log("capture: looked for the game's swap chain in %.0f ms: %s", NowMs() - t0, ok ? "found" : "NOT found");
    } else {
        ok = HookWithProbe();
        how = "a test swap chain (HLAE isn't loaded)";
    }
    FirstRunEnd("capture-install");
    g_hooked = ok;
    if (ok) {
        Log("capture: Present hooked through %s - DTRecord is ready", how);
        SetFeature("capture", "DTRecord", Feat_Ok, "DemoTools2's own recorder (DirectX 11).");
    } else {
        SetFeature("capture", "DTRecord", Feat_Off, "Couldn't find CS2's DirectX 11 swap chain. Record with HLAE.");
    }
    return ok;
}

bool Available(std::string* why) {
    if (!g_hooked) { if (why) *why = "DTRecord couldn't hook DirectX 11 (see the Status tab) - use HLAE"; return false; }
    if (GetModuleHandleA("rendersystemvulkan.dll") && !g_seen) {
        if (why) *why = "CS2 runs in Vulkan mode - DTRecord needs DirectX 11 (remove -vulkan from the launch options), or use HLAE";
        return false;
    }
    return true;
}
bool SeenFrames() { return g_seen.load(); }

bool Begin(const Params& p, std::string* why) {
    std::string w;
    if (!Available(&w)) { if (why) *why = w; return false; }
    if (p.ffmpeg.empty()) { if (why) *why = "ffmpeg.exe wasn't found (choose it in the Record tab)"; return false; }
    std::lock_guard<std::mutex> g(g_lock);
    if (g_rec) { if (why) *why = "a recording is still finishing"; return false; }
    g_error.clear();
    g_lastFrames = 0;
    g_startTime = 0;
    g_rec = new Rec();
    g_rec->id = g_nextId++;
    g_rec->p = p;
    g_rec->p.samples = std::max(1, p.samples);
    g_on = true;
    return true;
}
void End() {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_rec) g_rec->stopping = true;   // the next Present finishes it (the GPU copies belong to that thread)
    // no Present coming (window minimised, game hung): finish here after a moment, without the last frames
    Rec* r = g_rec;
    if (r)
        std::thread([id = r->id] {
            Sleep(3000);
            std::lock_guard<std::mutex> g2(g_lock);
            Rec* r = g_rec;
            if (!r || r->id != id) return;   // finished by a Present already
            Log("capture: no frame came to finish the recording - finishing without the last %lld frame(s)", r->copied - r->processed);
            Finish(r, nullptr);
            delete r;
            g_rec = nullptr;
            g_on = false;
        }).detach();
}
bool Active() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_rec != nullptr;
}
long long FramesWritten() { return g_lastFrames.load(); }
std::string Error() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_error;
}

}  // namespace Capture

// ==================================================================== sound
namespace Sound {

// Windows 10 2004+ per-process loopback (audioclientactivationparams.h, not in every SDK)
enum { kActivationProcessLoopback = 1, kIncludeProcessTree = 0 };
struct LoopbackParams { DWORD targetPid; DWORD mode; };
struct ActivationParams { DWORD type; LoopbackParams loopback; };
static const GUID kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

class Activated final : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler) || id == kIID_IAgileObject) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs;
        if (!n) { CloseHandle(done); delete this; }
        return n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done);
        return S_OK;
    }
};

struct Chunk { uint64_t t; std::vector<int16_t> s; };   // t: 100 ns (QPC time) of the first sample
static std::mutex g_lock;
static std::thread g_thread;
static std::atomic<bool> g_stop{false}, g_running{false};
static std::vector<Chunk> g_chunks;
static uint64_t g_markStart = 0, g_markEnd = 0;
static std::string g_err;

static uint64_t Now100ns() { return Capture::Now(); }   // the same clock as the frames (QPC, 100 ns)

bool Available(std::string* why) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v{sizeof v};
    if (fn && fn(&v) == 0 && (v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 19041))) return true;
    if (why) *why = "recording CS2's sound by itself needs Windows 10 version 2004 or newer";
    return false;
}

static void Run(HANDLE started, HRESULT* result) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    do {
        using ActivateFn = HRESULT(WINAPI*)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
        HMODULE mm = LoadLibraryA("mmdevapi.dll");
        auto activate = mm ? (ActivateFn)GetProcAddress(mm, "ActivateAudioInterfaceAsync") : nullptr;
        if (!activate) break;
        ActivationParams ap{kActivationProcessLoopback, {GetCurrentProcessId(), kIncludeProcessTree}};
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof ap;
        pv.blob.pBlobData = (BYTE*)&ap;
        auto* h = new Activated();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        hr = activate(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, h, &op);
        if (SUCCEEDED(hr) && WaitForSingleObject(h->done, 3000) == WAIT_OBJECT_0) {
            HRESULT ar = E_FAIL;
            IUnknown* unk = nullptr;
            hr = op->GetActivateResult(&ar, &unk);
            if (SUCCEEDED(hr)) hr = ar;
            if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
            if (unk) unk->Release();
        } else if (SUCCEEDED(hr)) hr = E_ABORT;
        if (op) op->Release();
        h->Release();
        if (FAILED(hr) || !client) break;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2;
        wf.nSamplesPerSec = 48000;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = 48000 * 4;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                2000000, 0, &wf, nullptr);
        if (FAILED(hr)) break;
        if (FAILED(hr = client->SetEventHandle(ev))) break;
        if (FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap))) break;
        if (FAILED(hr = client->Start())) break;
    } while (false);
    *result = hr;
    SetEvent(started);
    if (SUCCEEDED(hr) && cap) {
        while (!g_stop) {
            WaitForSingleObject(ev, 50);
            UINT32 n = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&n)) && n) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 qpc = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
                Chunk c;
                c.t = qpc ? qpc : Now100ns();
                c.s.assign((size_t)frames * 2, 0);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) std::memcpy(c.s.data(), data, (size_t)frames * 4);
                cap->ReleaseBuffer(frames);
                std::lock_guard<std::mutex> g(g_lock);
                if (g_chunks.size() < 400000) g_chunks.push_back(std::move(c));   // ~ 1 hour of 10 ms packets
            }
        }
        client->Stop();
    }
    if (cap) cap->Release();
    if (client) client->Release();
    CloseHandle(ev);
    CoUninitialize();
    g_running = false;
}

bool Begin(std::string* why) {
    if (!Available(why)) return false;
    Cancel();
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_chunks.clear();
        g_markStart = g_markEnd = 0;
    }
    g_stop = false;
    g_running = true;
    HANDLE started = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    auto* res = new HRESULT(E_PENDING);
    g_thread = std::thread([started, res] { Run(started, res); });
    WaitForSingleObject(started, 5000);
    const HRESULT hr = *res;
    CloseHandle(started);
    if (FAILED(hr)) {
        g_stop = true;
        if (g_thread.joinable()) g_thread.join();
        delete res;
        char b[96];
        std::snprintf(b, sizeof b, "couldn't record CS2's sound (0x%08lx)", (unsigned long)hr);
        if (why) *why = b;
        Log("sound: %s", b);
        return false;
    }
    delete res;
    Log("sound: recording CS2's sound");
    return true;
}
void MarkStart() { std::lock_guard<std::mutex> g(g_lock); g_markStart = Now100ns(); }
void MarkStartAt(uint64_t t) { std::lock_guard<std::mutex> g(g_lock); g_markStart = t; }
void MarkEnd() { std::lock_guard<std::mutex> g(g_lock); g_markEnd = Now100ns(); }

void Cancel() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

bool End(const std::string& wav) {
    Sleep(150);   // the last packets of the clip
    Cancel();
    std::vector<Chunk> chunks;
    uint64_t a, b;
    {
        std::lock_guard<std::mutex> g(g_lock);
        chunks.swap(g_chunks);
        a = g_markStart;
        b = g_markEnd;
    }
    if (!a || b <= a) return false;
    // place every packet on the clip's time line by its timestamp; gaps (nothing playing) stay silent
    const size_t total = (size_t)((b - a) * 48000 / 10000000);
    std::vector<int16_t> out(total * 2, 0);
    for (const Chunk& c : chunks) {
        const long long off = (long long)std::llround(((long double)c.t - (long double)a) * 48000.0L / 10000000.0L);
        const long long frames = (long long)c.s.size() / 2;
        for (long long i = std::max(0LL, -off); i < frames && off + i < (long long)total; ++i) {
            out[(size_t)(off + i) * 2] = c.s[(size_t)i * 2];
            out[(size_t)(off + i) * 2 + 1] = c.s[(size_t)i * 2 + 1];
        }
    }
    FILE* f = std::fopen(wav.c_str(), "wb");
    if (!f) return false;
    const uint32_t data = (uint32_t)(out.size() * 2), riff = 36 + data, fmtLen = 16, rate = 48000, bps = 48000 * 4;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
    std::fwrite(&bps, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    std::fwrite(out.data(), 2, out.size(), f);
    std::fclose(f);
    Log("sound: %s (%.2f s)", wav.c_str(), total / 48000.0);
    return true;
}

}  // namespace Sound
