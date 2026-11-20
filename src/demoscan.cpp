// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
// CS2 demo kill scanner - see demoscan.h.
// Layout of a CS2 .dem: "PBDEMS2\0", two int32 offsets, then frames: varint command, varint tick, varint size,
// payload (snappy-compressed when command & 64). Packet frames carry a bit stream of net messages
// (ubitvar type, varint size, protobuf body); game events come as GE_Source1LegacyGameEventList (205, the event
// names and key names) and GE_Source1LegacyGameEvent (207, one event).
#include "demoscan.h"
#include <cstdio>
#include <cstring>
#include <map>

namespace DemoScan {

// ------------------------------------------------------------------ snappy
bool SnappyDecompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    uint64_t len = 0;
    for (int shift = 0; i < n && shift < 35; shift += 7) {
        const uint8_t b = in[i++];
        len |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
    }
    if (len > (1u << 30)) return false;
    out.clear();
    out.reserve((size_t)len);
    while (i < n) {
        const uint8_t tag = in[i++];
        const int type = tag & 3;
        if (type == 0) {   // literal
            size_t l = tag >> 2;
            if (l >= 60) {
                const int bytes = (int)l - 59;
                if (i + bytes > n) return false;
                l = 0;
                for (int k = 0; k < bytes; ++k) l |= (size_t)in[i + k] << (8 * k);
                i += bytes;
            }
            ++l;
            if (i + l > n) return false;
            out.insert(out.end(), in + i, in + i + l);
            i += l;
            continue;
        }
        size_t l, off;
        if (type == 1) {
            if (i + 1 > n) return false;
            l = ((tag >> 2) & 7) + 4;
            off = ((size_t)(tag >> 5) << 8) | in[i++];
        } else if (type == 2) {
            if (i + 2 > n) return false;
            l = (tag >> 2) + 1;
            off = in[i] | (in[i + 1] << 8);
            i += 2;
        } else {
            if (i + 4 > n) return false;
            l = (tag >> 2) + 1;
            off = in[i] | (in[i + 1] << 8) | (in[i + 2] << 16) | ((size_t)in[i + 3] << 24);
            i += 4;
        }
        if (off == 0 || off > out.size()) return false;
        const size_t st = out.size() - off;
        for (size_t k = 0; k < l; ++k) out.push_back(out[st + k]);
    }
    return out.size() == len;
}

// ------------------------------------------------------------------ protobuf (just what is needed)
struct Pb {
    const uint8_t* p;
    const uint8_t* e;
    Pb(const uint8_t* a, size_t n) : p(a), e(a + n) {}
    bool more() const { return p < e; }
    uint64_t varint(bool& ok) {
        uint64_t v = 0;
        for (int s = 0; s < 64; s += 7) {
            if (p >= e) { ok = false; return 0; }
            const uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7f) << s;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    // next field: number, wire type; for length-delimited returns the bytes, for varint the value, fixed32 as bits
    bool next(int& field, int& wire, uint64_t& v, const uint8_t*& bytes, size_t& len) {
        bool ok = true;
        const uint64_t key = varint(ok);
        if (!ok) return false;
        field = (int)(key >> 3);
        wire = (int)(key & 7);
        switch (wire) {
        case 0: v = varint(ok); return ok;
        case 1: if (e - p < 8) return false; std::memcpy(&v, p, 8); p += 8; return true;
        case 2: {
            len = (size_t)varint(ok);
            if (!ok || (size_t)(e - p) < len) return false;
            bytes = p;
            p += len;
            return true;
        }
        case 5: { if (e - p < 4) return false; uint32_t x; std::memcpy(&x, p, 4); v = x; p += 4; return true; }
        default: return false;
        }
    }
};

// ------------------------------------------------------------------ bit reader (net messages inside packets)
struct Bits {
    const uint8_t* d;
    size_t n, pos = 0;   // pos in bits
    Bits(const uint8_t* a, size_t len) : d(a), n(len * 8) {}
    bool left(size_t b) const { return pos + b <= n; }
    uint32_t read(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i, ++pos) v |= (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
    uint32_t ubitvar() {
        uint32_t r = read(6);
        switch (r & 0x30) {
        case 16: r = (r & 15) | (read(4) << 4); break;
        case 32: r = (r & 15) | (read(8) << 4); break;
        case 48: r = (r & 15) | (read(28) << 4); break;
        }
        return r;
    }
    uint32_t varint() {
        uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            if (!left(8)) return 0;
            const uint32_t b = read(8);
            v |= (b & 0x7f) << s;
            if (!(b & 0x80)) break;
        }
        return v;
    }
    void bytes(std::vector<uint8_t>& out, size_t count) {
        out.resize(count);
        if ((pos & 7) == 0) {
            std::memcpy(out.data(), d + (pos >> 3), count);
            pos += count * 8;
        } else
            for (size_t i = 0; i < count; ++i) out[i] = (uint8_t)read(8);
    }
};

// ------------------------------------------------------------------ game events
struct Descriptor { std::string name; std::vector<std::string> keys; };
struct Value { int type = 0; std::string s; double f = 0; int64_t i = 0; bool has = false; };

struct Scanner {
    Result r;
    std::map<int, Descriptor> desc;
    int deathId = -1, roundStartId = -1, matchStartId = -1, freezeEndId = -1, roundEndId = -1, postStartId = -1;
    std::vector<int> freezeEnds, roundEnds, postStarts;
    std::vector<int> eventRoundStarts;
    std::vector<int> matchStarts;
    std::map<int, Result::Player> names;

    void EventList(const uint8_t* b, size_t n) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        while (pb.more() && pb.next(f, w, v, by, len)) {
            if (f != 1 || w != 2) continue;
            Descriptor d;
            int id = -1;
            Pb q(by, len);
            int f2, w2;
            uint64_t v2;
            const uint8_t* b2;
            size_t l2;
            while (q.more() && q.next(f2, w2, v2, b2, l2)) {
                if (f2 == 1 && w2 == 0) id = (int)v2;
                else if (f2 == 2 && w2 == 2) d.name.assign((const char*)b2, l2);
                else if (f2 == 3 && w2 == 2) {
                    Pb k(b2, l2);
                    int f3, w3;
                    uint64_t v3;
                    const uint8_t* b3;
                    size_t l3;
                    std::string kn;
                    while (k.more() && k.next(f3, w3, v3, b3, l3)) if (f3 == 2 && w3 == 2) kn.assign((const char*)b3, l3);
                    d.keys.push_back(kn);
                }
            }
            if (id < 0) continue;
            if (d.name == "player_death") deathId = id;
            else if (d.name == "round_start") roundStartId = id;
            else if (d.name == "round_announce_match_start") matchStartId = id;
            else if (d.name == "round_freeze_end") freezeEndId = id;
            else if (d.name == "round_officially_ended") roundEndId = id;
            else if (d.name == "round_poststart") postStartId = id;
            desc[id] = std::move(d);
        }
    }

    void Event(const uint8_t* b, size_t n, int tick) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        int id = -1;
        std::vector<Value> vals;
        while (pb.more() && pb.next(f, w, v, by, len)) {
            if (f == 2 && w == 0) id = (int)v;
            else if (f == 3 && w == 2) {
                Value x;
                Pb k(by, len);
                int f2, w2;
                uint64_t v2;
                const uint8_t* b2;
                size_t l2;
                while (k.more() && k.next(f2, w2, v2, b2, l2)) {
                    x.has = true;
                    if (f2 == 1) x.type = (int)v2;
                    else if (f2 == 2 && w2 == 2) x.s.assign((const char*)b2, l2);
                    else if (f2 == 3 && w2 == 5) { float fl; uint32_t u = (uint32_t)v2; std::memcpy(&fl, &u, 4); x.f = fl; }
                    else if (f2 >= 4 && f2 <= 8 && w2 == 0) x.i = (int64_t)v2;
                }
                vals.push_back(std::move(x));
            }
        }
        if (id < 0) return;
        if (id == roundStartId) { eventRoundStarts.push_back(tick); return; }
        if (id == matchStartId) { matchStarts.push_back(tick); return; }
        if (id == freezeEndId) { freezeEnds.push_back(tick); return; }
        if (id == roundEndId) { roundEnds.push_back(tick); return; }
        if (id == postStartId) { postStarts.push_back(tick); return; }
        auto it = desc.find(id);
        if (it == desc.end()) return;
        const Descriptor& d = it->second;
        auto get = [&](const char* key) -> const Value* {
            for (size_t i = 0; i < d.keys.size() && i < vals.size(); ++i)
                if (d.keys[i] == key) return &vals[i];
            return nullptr;
        };
        // names, from any event that has both a player and a name
        if (const Value* nm = get("name"))
            if (const Value* u = get("userid"))
                if (!nm->s.empty()) {
                    Result::Player& p = names[(int)(u->i & 0xff)];
                    p.slot = (int)(u->i & 0xff);
                    p.name = nm->s;
                    if (const Value* x = get("xuid")) if (x->i) p.xuid = (uint64_t)x->i;
                }
        if (id != deathId) return;
        Kill k;
        k.tick = tick;
        auto slot = [&](const char* key) -> int {
            const Value* x = get(key);
            if (!x) return -1;
            const int64_t s = x->i;
            if (s < 0 || (s & 0xffff) == 0xffff) return -1;
            return (int)(s & 0xff);
        };
        k.attacker = slot("attacker");
        k.victim = slot("userid");
        k.assister = slot("assister");
        if (const Value* x = get("weapon")) k.weapon = x->s;
        auto flag = [&](const char* key) { const Value* x = get(key); return x && x->i != 0; };
        k.headshot = flag("headshot");
        k.wallbang = get("penetrated") && get("penetrated")->i > 0;
        k.noscope = flag("noscope");
        k.thruSmoke = flag("thrusmoke");
        k.attackerBlind = flag("attackerblind");
        r.kills.push_back(k);
    }

    void Packet(const uint8_t* b, size_t n, int tick) {
        Bits bits(b, n);
        std::vector<uint8_t> msg;
        while (bits.left(8)) {
            const uint32_t type = bits.ubitvar();
            const uint32_t size = bits.varint();
            if (!bits.left((size_t)size * 8)) break;
            if (type == 205 || type == 207) {
                bits.bytes(msg, size);
                if (type == 205) EventList(msg.data(), msg.size());
                else Event(msg.data(), msg.size(), tick);
            } else
                bits.pos += (size_t)size * 8;
        }
    }

    // CDemoPacket { bytes data = 3 }
    void DemoPacket(const uint8_t* b, size_t n, int tick) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        while (pb.more() && pb.next(f, w, v, by, len))
            if (f == 3 && w == 2) Packet(by, len, tick);
    }
    // CDemoFullPacket { CDemoStringTables string_table = 1; CDemoPacket packet = 2 }
    void FullPacket(const uint8_t* b, size_t n, int tick) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        while (pb.more() && pb.next(f, w, v, by, len))
            if (f == 2 && w == 2) DemoPacket(by, len, tick);
    }
    // CDemoFileHeader { map_name = 5 }
    void Header(const uint8_t* b, size_t n) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        while (pb.more() && pb.next(f, w, v, by, len))
            if (f == 5 && w == 2) r.map.assign((const char*)by, len);
    }
    // CDemoFileInfo { playback_ticks = 2; CGameInfo game_info = 4 { CCSGameInfo cs = 4 { repeated int32 round_start_ticks = 1 } } }
    void FileInfo(const uint8_t* b, size_t n) {
        Pb pb(b, n);
        int f, w;
        uint64_t v;
        const uint8_t* by;
        size_t len;
        while (pb.more() && pb.next(f, w, v, by, len)) {
            if (f == 2 && w == 0) r.ticks = (int)v;
            if (f != 4 || w != 2) continue;
            Pb g(by, len);
            int f2, w2;
            uint64_t v2;
            const uint8_t* b2;
            size_t l2;
            while (g.more() && g.next(f2, w2, v2, b2, l2)) {
                if (w2 != 2) continue;
                Pb c(b2, l2);
                int f3, w3;
                uint64_t v3;
                const uint8_t* b3;
                size_t l3;
                std::vector<int> starts;
                while (c.more() && c.next(f3, w3, v3, b3, l3)) {
                    if (f3 != 1) continue;
                    if (w3 == 0) starts.push_back((int)v3);
                    else if (w3 == 2) {   // packed
                        Pb pk(b3, l3);
                        bool ok = true;
                        while (pk.more() && ok) { const uint64_t x = pk.varint(ok); if (ok) starts.push_back((int)x); }
                    }
                }
                if (!starts.empty()) r.roundStarts = starts;
            }
        }
    }
};

Result Scan(const uint8_t* data, size_t size) {
    Scanner s;
    if (size < 16 || std::memcmp(data, "PBDEMS2", 8) != 0) {
        s.r.error = "not a CS2 demo (.dem from CS2 starts with PBDEMS2)";
        return s.r;
    }
    // DEM_FileInfo sits after DEM_Stop; the header points at it (round start ticks, demo length)
    {
        uint32_t infoOff;
        std::memcpy(&infoOff, data + 8, 4);
        if (infoOff > 16 && infoOff < size) {
            Pb fi(data + infoOff, size - infoOff);
            bool ok2 = true;
            const uint32_t c = (uint32_t)fi.varint(ok2);
            fi.varint(ok2);
            const uint64_t l = fi.varint(ok2);
            if (ok2 && (c & ~64u) == 2 && (uint64_t)(fi.e - fi.p) >= l) {
                std::vector<uint8_t> tmp;
                const uint8_t* b = fi.p;
                size_t n = (size_t)l;
                if (!(c & 64) || SnappyDecompress(b, n, tmp)) {
                    if (c & 64) { b = tmp.data(); n = tmp.size(); }
                    s.FileInfo(b, n);
                }
            }
        }
    }
    Pb pb(data + 16, size - 16);
    std::vector<uint8_t> buf;
    bool ok = true;
    while (pb.more()) {
        const uint32_t cmdRaw = (uint32_t)pb.varint(ok);
        const uint32_t tickRaw = (uint32_t)pb.varint(ok);
        const uint64_t len = pb.varint(ok);
        if (!ok || (uint64_t)(pb.e - pb.p) < len) break;
        const uint8_t* body = pb.p;
        size_t blen = (size_t)len;
        pb.p += len;
        const uint32_t cmd = cmdRaw & ~64u;
        const int tick = tickRaw == 0xffffffffu ? 0 : (int)tickRaw;
        if (cmd == 0) break;   // DEM_Stop
        const bool wanted = cmd == 1 || cmd == 2 || cmd == 7 || cmd == 8 || cmd == 13;
        if (!wanted) continue;
        if (cmdRaw & 64) {
            if (!SnappyDecompress(body, blen, buf)) continue;
            body = buf.data();
            blen = buf.size();
        }
        switch (cmd) {
        case 1: s.Header(body, blen); break;
        case 2: s.FileInfo(body, blen); break;
        case 7: case 8: s.DemoPacket(body, blen, tick); break;
        case 13: s.FullPacket(body, blen, tick); break;
        }
    }
    Result& r = s.r;
    if (r.roundStarts.empty()) {
        // from the events: rounds after the last "match start" (warmup doesn't count)
        // The first real round starts just before "match start" is announced (that comes at the end of its
        // freeze time), so take the last round start at or before it.
        int from = 0;
        if (!s.matchStarts.empty())
            for (int t : s.eventRoundStarts) if (t <= s.matchStarts.back()) from = t;
        for (int t : s.eventRoundStarts) if (t >= from) r.roundStarts.push_back(t);
    }
    // demos without round_start: other round events (each marks one round)
    auto fromList = [&](const std::vector<int>& v, int shift) {
        if (!r.roundStarts.empty() || v.empty()) return;
        const int from = s.matchStarts.empty() ? 0 : s.matchStarts.back() - 64 * 30;
        for (int t : v) if (t >= from) r.roundStarts.push_back(std::max(0, t - shift));
    };
    fromList(s.postStarts, 0);
    fromList(s.freezeEnds, 64 * 20);   // freeze time ends ~15-20 s after the round starts
    if (r.roundStarts.empty() && !s.roundEnds.empty()) {   // a round starts where the previous one officially ended
        r.roundStarts.push_back(0);
        for (int t : s.roundEnds) r.roundStarts.push_back(t);
    }
    r.matchStart = s.matchStarts.empty() ? -1 : s.matchStarts.back();
    r.eventRounds = (int)s.eventRoundStarts.size();
    for (Kill& k : r.kills) {
        k.round = 0;
        for (size_t i = 0; i < r.roundStarts.size(); ++i) if (k.tick >= r.roundStarts[i]) k.round = (int)i + 1;
    }
    for (auto& kv : s.names) r.players.push_back(kv.second);
    if (s.deathId < 0) { r.error = "no kill events found in this demo"; return r; }
    r.ok = true;
    return r;
}

Result ScanFile(const std::string& path) {
    Result r;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = "couldn't open " + path; return r; }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    const long long size = (long long)ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    std::vector<uint8_t> data;
    if (size > 0 && size < (2ll << 30)) {
        data.resize((size_t)size);
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
    if (data.empty()) { r.error = "couldn't read " + path; return r; }
    return Scan(data.data(), data.size());
}

}  // namespace DemoScan
