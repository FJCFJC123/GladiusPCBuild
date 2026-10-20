// Loose data folder ("mods"): a file under <game dir>\data\ replaces the copy in gladius.bec, and files the bec
// doesn't have can be added. Lookups in the bec index (0x1FA680) and the .pak bundles (0x1FA620) report "not
// found" for loose files, so the engine's own loose-file path reads them. data\audio and data\movies are not mods.
// port_mods_crc() (paths + contents) goes into the netplay version hash.
#include "port.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

const char *port_game_dir();                            // xapi_file.cpp: D:

static std::unordered_set<std::string> g_files;         // "data/levels/.../x.prp" (lowercase, '/')
static SRWLOCK g_files_lock = SRWLOCK_INIT;             // lookups (game threads) vs. port_mods_rescan (editor)
static std::unordered_set<std::string> g_used;          // logged once each
static SRWLOCK g_used_lock = SRWLOCK_INIT;
static uint32_t g_crc;

static uint32_t crc32(const void *data, size_t n, uint32_t crc)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// "d:\data\Levels\x.prp" -> "data/levels/x.prp"
static std::string key_of(const char *name)
{
    std::string k;
    for (const char *p = name; *p; p++) {
        char c = *p == 0x5C ? '/' : (char)tolower((unsigned char)*p);
        if (c == '/' && (k.empty() || k.back() == '/')) continue;
        k += c;
    }
    if (k.size() > 2 && k[1] == ':') k.erase(0, 2);    // drive letter
    if (!k.empty() && k[0] == '/') k.erase(0, 1);
    return k;
}

// Collect every file under dir as lowercase keys (skipping data/audio and data/movies).
static void scan(const std::string &dir, const std::string &rel, std::unordered_set<std::string> &out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string r = key_of((rel + "/" + fd.cFileName).c_str());
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (r == "data/audio" || r == "data/movies") continue;
            scan(dir + "\\" + fd.cFileName, r, out);
        } else {
            out.insert(r);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// True when `name` is a loose file (logged once per file and site).
static bool loose(const char *name, const char *site)
{
    if (!name) return false;
    std::string k = key_of(name);
    AcquireSRWLockShared(&g_files_lock);
    bool have = g_files.count(k) != 0;
    ReleaseSRWLockShared(&g_files_lock);
    if (!have) return false;
    AcquireSRWLockExclusive(&g_used_lock);
    bool first = g_used.insert(k + "|" + site).second;
    ReleaseSRWLockExclusive(&g_used_lock);
    if (first) port_log("mods: loose %s (%s \"%s\")\n", k.c_str(), site, name);
    return true;
}

typedef void *(__thiscall *BecLookup)(void *mgr, const char *name, uint32_t *size);
static const BecLookup bec_lookup = (BecLookup)0x1FA680;

void port_editor_note_file(const char *name);         // editor.cpp: the level's prop files
// Bec index lookup: loose files are "not in the bec".
static void *__fastcall lookup_hook(void *mgr, void *, const char *name, uint32_t *size)
{
    port_editor_note_file(name);
    return loose(name, "open") ? nullptr : bec_lookup(mgr, name, size);
}

// File-exists check: loose files count as present.
static void *__fastcall exists_hook(void *mgr, void *, const char *name, uint32_t *size)
{
    return loose(name, "exists") ? (void *)1 : bec_lookup(mgr, name, size);
}

typedef void *(__thiscall *PakLookup)(void *mgr, const char *name);
static const PakLookup pak_lookup = (PakLookup)0x1FA620;

// .pak lookup: loose files are "not in a pak".
static void *__fastcall pak_hook(void *mgr, void *, const char *name)
{
    // the name arrives before path expansion ("$d\Config\x.bin"; $l = data\levels, $t = data\texture)
    std::string full = name ? name : "";
    if (full.size() > 2 && full[0] == '$' && (full[2] == 0x5C || full[2] == '/')) {
        char c = (char)tolower((unsigned char)full[1]);
        const char *base = c == 'd' ? "data" : c == 'l' ? "data\\levels" : c == 't' ? "data\\texture" : nullptr;
        if (base) full = base + full.substr(2);
    }
    return loose(full.c_str(), "pak") ? nullptr : pak_lookup(mgr, name);
}

// enabled gameplay codes count as mods: netplay pairs only PCs with the same set
uint32_t port_codes_mask();                             // codes.cpp
uint32_t port_mods_crc()
{
    uint32_t codes = port_codes_mask();
    return codes ? crc32(&codes, sizeof codes, g_crc) : g_crc;
}

// Editor: loose files with this extension, sorted.
void port_mods_list(const char *ext, std::vector<std::string> &out)
{
    size_t el = strlen(ext);
    AcquireSRWLockShared(&g_files_lock);
    for (const std::string &k : g_files)
        if (k.size() > el && !_stricmp(k.c_str() + k.size() - el, ext)) out.push_back(k);
    ReleaseSRWLockShared(&g_files_lock);
    std::sort(out.begin(), out.end());
}

// Editor: rescan the folder after saving files.
void port_mods_rescan()
{
    std::unordered_set<std::string> now;
    scan(std::string(port_game_dir()) + "\\data", "data", now);
    unsigned added = 0;
    AcquireSRWLockExclusive(&g_files_lock);
    for (const std::string &k : now)
        if (!g_files.count(k)) {
            added++;
            port_log("mods: new loose file %s\n", k.c_str());
        }
    unsigned removed = (unsigned)(g_files.size() + added - now.size());
    g_files.swap(now);
    ReleaseSRWLockExclusive(&g_files_lock);
    port_log("mods: rescanned - %u loose files (%u new, %u gone)\n", (unsigned)g_files.size(), added, removed);
}

static bool install_hooks();

// Sound table caps: SoundTbs.pak (255 tables) and pltbin.pak (0x6C00 bytes) load into fixed buffers without size
// checks, and more arenas overflow them. Point both loaders at bigger buffers.
static __declspec(align(16)) uint8_t g_soundtbs[1024 * 0x7C];
static __declspec(align(16)) uint8_t g_pltbin[0x40000];

static void raise_sound_caps()
{
    struct { DWORD at; int len; const char *code; DWORD old, now; } p[] = {
        {0x18E5B6, 2, "\x81\xC6", 0x493DF0, (DWORD)g_soundtbs},         // add esi, table
        {0x1C2F15, 1, "\xBF", 0x4BF110, (DWORD)g_pltbin},                 // mov edi, buffer (the copy)
        {0x1C2F36, 3, "\xC7\x45\x04", 0x4BF110, (DWORD)g_pltbin},         // mov [ebp+4], buffer
        {0x1C2F3D, 2, "\x83\x3D", 0x4BF110, (DWORD)g_pltbin},             // cmp [buffer], 1
        {0x1C2F5E, 2, "\x8B\x0D", 0x4BF114, (DWORD)g_pltbin + 4},         // mov ecx, [buffer + 4] (record count)
    };
    for (auto &x : p)
        if (memcmp((void *)x.at, x.code, x.len) || *(DWORD *)(x.at + x.len) != x.old) {
            port_log("mods: sound table code unexpected at %06X - caps NOT raised\n", x.at);
            return;
        }
    for (auto &x : p)
        *(DWORD *)(x.at + x.len) = x.now;
    port_log("mods: sound caps raised - SoundTbs.pak %u tables (was 255), pltbin.pak %u bytes (was %u)\n",
             (unsigned)(sizeof g_soundtbs / 0x7C), (unsigned)sizeof g_pltbin, 0x6C00u);
}

// Scan the loose folder, hash it for netplay and install the lookup hooks.
void port_mods_init()
{
    raise_sound_caps();
    std::string root = std::string(port_game_dir()) + "\\data";
    scan(root, "data", g_files);
    if (g_files.empty()) {
        port_log("mods: no loose files under %s (gladius.bec only)\n", root.c_str());
        if (port_is_editor() && install_hooks())           // the editor adds files while it runs
            port_log("mods: editor - loose data folder hooks installed\n");
        return;
    }
    // version hash: sorted paths + contents
    std::vector<std::string> keys(g_files.begin(), g_files.end());
    std::sort(keys.begin(), keys.end());
    DWORD t0 = GetTickCount();
    uint64_t bytes = 0;
    std::vector<uint8_t> buf(1 << 20);
    for (const std::string &k : keys) {
        g_crc = crc32(k.data(), k.size() + 1, g_crc);
        std::string path = std::string(port_game_dir()) + "\\" + k;
        std::replace(path.begin(), path.end(), '/', (char)0x5C);
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD n;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) {
            g_crc = crc32(buf.data(), n, g_crc);
            bytes += n;
        }
        CloseHandle(f);
    }
    if (!g_crc) g_crc = 1;                              // 0 = no mods
    if (!install_hooks()) {
        g_crc = 0;
        return;
    }
    port_log("mods: %u loose files under %s win over gladius.bec (%.1f MB hashed in %lu ms, crc %08X)\n",
             (unsigned)keys.size(), root.c_str(), bytes / 1048576.0, GetTickCount() - t0, g_crc);
    for (size_t i = 0; i < keys.size() && i < 20; i++) port_log("  %s\n", keys[i].c_str());
    if (keys.size() > 20) port_log("  ... and %u more\n", (unsigned)(keys.size() - 20));
}

// Redirect the bec / pak lookups and let a bec miss fall through to the loose open.
static bool install_hooks()
{
    struct { DWORD site, target; void *fn; } calls[] = {{0x1FCAC6, 0x1FA680, (void *)lookup_hook},
                                                       {0x1C89CD, 0x1FA680, (void *)lookup_hook},
                                                       {0x1AE920, 0x1FA680, (void *)exists_hook},
                                                       {0x1AE67E, 0x1FA620, (void *)pak_hook},
                                                       {0x1FCA8A, 0x1FA620, (void *)pak_hook}};
    bool ok = *(WORD *)0x1FCAD0 == 0x3474;              // jz 0x1FCB06 (open fails on a bec miss)
    for (auto &c : calls)
        ok = ok && *(BYTE *)c.site == 0xE8 && c.site + 5 + *(int32_t *)(c.site + 1) == c.target;
    if (!ok) {
        port_log("mods: file manager code unexpected - loose data folder NOT enabled\n");
        return false;
    }
    for (auto &c : calls)
        *(int32_t *)(c.site + 1) = (int32_t)((DWORD)c.fn - (c.site + 5));
    *(BYTE *)0x1FCAD1 = 0x0E;                           // bec miss -> loose open 0x1FCAE0
    return true;
}
