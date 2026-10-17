// Direct reads of game data for the editor: a loose copy under <game dir>\data wins, else the file is read straight
// from gladius.bec (the engine's loader can't be used mid-frame without disturbing its async file system).
// gladius.bec (version 3): " ceb", u16 version, u16 alignment, u32 file count, u32 ?, then 16-byte entries sorted by
// hash: u32 hash, u32 offset, u32 stored size (0 = uncompressed; top bit = duplicate), u32 size. hash = CRC32 table
// (no initial / final xor) over the lower-case "data/..." path. Compressed data is a zlib stream.
#include "port.h"
#include <string>
#include <vector>

const char *port_game_dir();                            // xapi_file.cpp

// ------------------------------------------------------------------------------------------------ inflate
namespace {
struct Inflate {
    const uint8_t *src;
    size_t len, pos;
    uint32_t bitbuf;
    int bitcnt;
    std::string *out;
    bool bad;

    int bit()
    {
        if (!bitcnt) {
            if (pos >= len) { bad = true; return 0; }
            bitbuf = src[pos++];
            bitcnt = 8;
        }
        int b = bitbuf & 1;
        bitbuf >>= 1;
        bitcnt--;
        return b;
    }
    int bits(int n)
    {
        int v = 0;
        for (int i = 0; i < n; i++) v |= bit() << i;
        return v;
    }

    struct Huff { short count[16], symbol[288]; };
    static void build(Huff &h, const uint8_t *lengths, int n)
    {
        memset(h.count, 0, sizeof h.count);
        for (int i = 0; i < n; i++) h.count[lengths[i]]++;
        h.count[0] = 0;
        short offs[16];
        offs[1] = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
        for (int i = 0; i < n; i++)
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = (short)i;
    }
    int decode(const Huff &h)
    {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; l++) {
            code |= bit();
            int c = h.count[l];
            if (code - c < first) return h.symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
            if (bad) return -1;
        }
        bad = true;
        return -1;
    }

    bool codes(const Huff &lit, const Huff &dist)
    {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
                                        83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
                                       12, 12, 13, 13};
        for (;;) {
            int sym = decode(lit);
            if (bad || sym < 0) return false;
            if (sym < 256) { out->push_back((char)sym); continue; }
            if (sym == 256) return true;
            sym -= 257;
            if (sym >= 29) return false;
            int n = lbase[sym] + bits(lext[sym]);
            int ds = decode(dist);
            if (bad || ds < 0 || ds >= 30) return false;
            size_t d = (size_t)dbase[ds] + bits(dext[ds]);
            if (d > out->size()) return false;
            size_t from = out->size() - d;
            for (int i = 0; i < n; i++) out->push_back((*out)[from + i]);
        }
    }

    bool run()
    {
        int last;
        do {
            last = bit();
            int type = bits(2);
            if (type == 0) {                            // stored
                bitcnt = 0;
                if (pos + 4 > len) return false;
                unsigned n = src[pos] | src[pos + 1] << 8;
                pos += 4;
                if (pos + n > len) return false;
                out->append((const char *)src + pos, n);
                pos += n;
            } else if (type == 1) {                     // fixed codes
                static Huff lit, dist;
                static bool made;
                if (!made) {
                    uint8_t l[288];
                    int i = 0;
                    for (; i < 144; i++) l[i] = 8;
                    for (; i < 256; i++) l[i] = 9;
                    for (; i < 280; i++) l[i] = 7;
                    for (; i < 288; i++) l[i] = 8;
                    build(lit, l, 288);
                    for (i = 0; i < 30; i++) l[i] = 5;
                    build(dist, l, 30);
                    made = true;
                }
                if (!codes(lit, dist)) return false;
            } else if (type == 2) {                     // dynamic codes
                int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t l[320] = {};
                for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)bits(3);
                Huff lc;
                build(lc, l, 19);
                memset(l, 0, sizeof l);
                for (int i = 0; i < nlen + ndist;) {
                    int sym = decode(lc);
                    if (bad || sym < 0) return false;
                    if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
                    int rep, val = 0;
                    if (sym == 16) { if (!i) return false; val = l[i - 1]; rep = 3 + bits(2); }
                    else if (sym == 17) rep = 3 + bits(3);
                    else rep = 11 + bits(7);
                    if (i + rep > nlen + ndist) return false;
                    while (rep--) l[i++] = (uint8_t)val;
                }
                Huff lit, dist;
                build(lit, l, nlen);
                build(dist, l + nlen, ndist);
                if (!codes(lit, dist)) return false;
            } else {
                return false;
            }
        } while (!last && !bad);
        return !bad;
    }
};
}

// Inflate a zlib stream (expect = decompressed size, 0 = any).
static bool zlib_inflate(const uint8_t *src, size_t len, size_t expect, std::string &out)
{
    if (len < 2 || (src[0] & 0x0F) != 8) return false;
    Inflate z = {src, len, 2, 0, 0, &out, false};
    out.clear();
    out.reserve(expect);
    return z.run() && (!expect || out.size() == expect);
}

// ------------------------------------------------------------------------------------------------ bec
// Read a whole file from disk.
static bool read_disk(const std::string &path, std::string &out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < 0x40000000;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD n = 0;
        ok = out.empty() || (ReadFile(f, &out[0], (DWORD)out.size(), &n, nullptr) && n == out.size());
    }
    CloseHandle(f);
    return ok;
}

// The bec's path hash.
static uint32_t bec_hash(const std::string &key)
{
    static uint32_t T[256];
    if (!T[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            T[i] = c;
        }
    uint32_t v = 0;
    for (unsigned char c : key) v = T[(v ^ c) & 0xFF] ^ (v >> 8);
    return v;
}

// key: "data/mesh/misc/x.pax" (any case / slashes). The loose copy first, then gladius.bec.
bool port_read_game_file(const std::string &key_in, std::string &out)
{
    std::string key;
    for (char c : key_in) key += c == (char)0x5C ? '/' : (char)tolower((unsigned char)c);
    std::string loose = std::string(port_game_dir()) + (char)0x5C + key;
    for (auto &c : loose) if (c == '/') c = (char)0x5C;
    if (read_disk(loose, out)) return true;

    std::string bec = std::string(port_game_dir()) + (char)0x5C + "gladius.bec";
    HANDLE f = CreateFileA(bec.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    uint8_t hdr[16];
    DWORD n = 0;
    if (ReadFile(f, hdr, 16, &n, nullptr) && n == 16 && !memcmp(hdr, " ceb", 4)) {
        uint32_t count;
        memcpy(&count, hdr + 8, 4);
        std::vector<uint32_t> ents((size_t)count * 4);
        if (count && count < 1000000 && ReadFile(f, ents.data(), count * 16, &n, nullptr) && n == count * 16) {
            uint32_t h = bec_hash(key);
            size_t lo = 0, hi = count;
            while (lo < hi) {                           // entries are sorted by hash
                size_t mid = (lo + hi) / 2;
                if (ents[mid * 4] < h) lo = mid + 1; else hi = mid;
            }
            if (lo < count && ents[lo * 4] == h) {
                uint32_t off = ents[lo * 4 + 1], stored = ents[lo * 4 + 2] & 0x7FFFFFFF, size = ents[lo * 4 + 3];
                std::string raw(stored ? stored : size, 0);
                LARGE_INTEGER at;
                at.QuadPart = off;
                if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, &raw[0], (DWORD)raw.size(), &n, nullptr) &&
                    n == raw.size()) {
                    if (stored && (uint8_t)raw[0] == 0x78)
                        ok = zlib_inflate((const uint8_t *)raw.data(), raw.size(), size, out);
                    else {
                        out.swap(raw);
                        ok = true;
                    }
                }
            }
        }
    }
    CloseHandle(f);
    if (!ok) port_log("bec_read: %s not found (loose or gladius.bec)\n", key.c_str());
    return ok;
}
