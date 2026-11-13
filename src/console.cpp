// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#include "console.h"
#include "game.h"
#include "util.h"
#include <cstdarg>
#include <deque>
#include <mutex>

namespace Console {

// ---- the game's command objects (layouts from HLAE's CS2 SDK headers, advancedfx-prop, MIT)
// CCmd: name, help, flags, callback (ICommandCallback*), 0x0101 (= callback is an interface), 0, 1, next
struct CCmd {
    const char* name;
    const char* help;
    int64_t flags;
    void* callback;
    size_t unk32 = 0x0101;
    size_t unk40 = 0;
    size_t unk48 = 1;
    size_t next = 0xffff;
};
// ICommandCallback has one virtual function: CommandCallback(void* unknown, CCommand* args).
struct Callback {
    void** vtable;
    Handler handler;
    const char* name;
};
// CCommand: +0x00 int argv0 size; +0x08 CUtlVectorFixedGrowable<char,512> (data pointer at +0x10) holding the whole
// command line; ... +0x438 argument count, +0x440 argument pointers.
static const ptrdiff_t kCmdLine = 0x10, kArgc = 0x438, kArgv = 0x440;

struct Entry { CCmd cmd; Callback cb; std::string name, help; };
static std::deque<Entry> g_entries;   // stable addresses: the game keeps pointers to them
static bool g_installed = false;

using MsgFn = void (*)(const char*, ...);
static MsgFn Msg() {
    static MsgFn fn = []() -> MsgFn {
        HMODULE tier0 = GetModuleHandleA("tier0.dll");
        return tier0 ? (MsgFn)GetProcAddress(tier0, "Msg") : nullptr;
    }();
    return fn;
}

static std::mutex g_outLock;
static std::deque<std::string> g_out;

void Print(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (MsgFn m = Msg()) m("%s\n", buf);
    std::lock_guard<std::mutex> g(g_outLock);
    std::string all(buf);
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t nl = all.find('\n', pos);
        g_out.push_back(all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    while (g_out.size() > 300) g_out.pop_front();
}

std::vector<std::string> RecentOutput() {
    std::lock_guard<std::mutex> g(g_outLock);
    return std::vector<std::string>(g_out.begin(), g_out.end());
}

Args Tokenize(const std::string& line) {
    Args out;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n) break;
        std::string tok;
        if (line[i] == '"') {
            ++i;
            while (i < n && line[i] != '"') tok += line[i++];
            if (i < n) ++i;
        } else {
            while (i < n && line[i] != ' ' && line[i] != '\t') tok += line[i++];
        }
        out.push_back(tok);
    }
    return out;
}

static void Dispatch(Callback* cb, const Args& args) {
    if (!cb || !cb->handler) return;
    Step step("console command");
    cb->handler(args);
}

// Called by CS2 on the game thread (MSVC member function: this, unknown, CCommand*).
static void CommandCallback(Callback* self, void*, uint8_t* command) {
    Args args;
    const char* line = Readable(command + kCmdLine, 8) ? Get<const char*>(command, kCmdLine) : nullptr;
    if (line && Readable(line, 1) && strnlen(line, 1024) < 1024) {
        args = Tokenize(line);
    } else if (Readable(command + kArgc, 16)) {
        int argc = Get<int>(command, kArgc);
        char** argv = Get<char**>(command, kArgv);
        for (int i = 0; i < argc && i < 64 && Readable(argv + i); ++i)
            if (Readable(argv[i], 1)) args.push_back(argv[i]);
    }
    if (args.empty()) args.push_back(self->name);
    Dispatch(self, args);
}
static void* g_vtable[1] = {(void*)&CommandCallback};

void Add(const char* name, const char* help, Handler handler) {
    g_entries.emplace_back();
    Entry& e = g_entries.back();
    e.name = name;
    e.help = help;
    e.cb.vtable = g_vtable;
    e.cb.handler = handler;
    e.cb.name = e.name.c_str();
    e.cmd.name = e.name.c_str();
    e.cmd.help = e.help.c_str();
    e.cmd.flags = 0;
    e.cmd.callback = &e.cb;
}

bool Install() {
    if (g_installed) return true;
    void* cvar = Game::CvarSystem();
    if (!cvar) { Log("console commands: VEngineCvar007 not found - use the command line in the DemoTools2 window"); return false; }
    // ICvar::RegisterConCommand is function 42. It returns a small struct, which MSVC member functions return
    // through a hidden pointer passed right after "this".
    using RegisterFn = void* (*)(void* self, void* result, CCmd* cmd, int64_t flags);
    auto reg = (RegisterFn)VFunc(cvar, 42);
    if (!Readable((void*)reg, 16)) { Log("console commands: RegisterConCommand not readable"); return false; }
    int n = 0;
    Step step("registering console commands");
    FirstRunBegin("console-register");
    for (Entry& e : g_entries) {
        size_t result[2] = {0xffff, 0};
        reg(cvar, result, &e.cmd, 0);
        ++n;
    }
    FirstRunEnd("console-register");
    g_installed = true;
    Log("console commands registered: %d (dt2_calc, dt2_cam, dt2_preset - type one for help)", n);
    return true;
}

bool Run(const std::string& line) {
    Args args = Tokenize(line);
    if (args.empty()) return false;
    for (Entry& e : g_entries)
        if (_stricmp(e.name.c_str(), args[0].c_str()) == 0) { Dispatch(&e.cb, args); return true; }
    Print("Unknown DemoTools2 command: %s (try dt2_calc, dt2_cam or dt2_preset)", args[0].c_str());
    return false;
}

}   // namespace Console
