// Loose data folder ("mods"): a file under <game dir>\data\ replaces the copy in gladius.bec, and files the bec
// doesn't have can be added. Lookups in the bec index (0x1FA680) and the .pak bundles (0x1FA620) report "not
// found" for loose files, so the engine's own loose-file path reads them. data\audio and data\movies are not mods.
// port_mods_crc() (paths + contents) goes into the netplay version hash.
//
// Live: a watcher thread rescans the folder whenever something in it changes ([mods] LiveReload=1, the default), so
// files added, edited or deleted while the game runs count from then on - the game reads a file when it loads it, so
// an edit shows the next time that part of the game loads (a new level, screen or battle). The netplay hash follows
// (gladiusnet reads port_mods_crc() live and holds it during a session).
#include "port.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

const char *port_game_dir();                            // xapi_file.cpp: D:

static std::unordered_set<std::string> g_files;         // "data/levels/.../x.prp" (lowercase, '/')
static SRWLOCK g_files_lock = SRWLOCK_INIT;             // lookups (game threads) vs. refresh (watcher, editor)
static std::unordered_set<std::string> g_used;          // logged once each
static SRWLOCK g_used_lock = SRWLOCK_INIT;
static volatile uint32_t g_crc;                         // read by gladiusnet's threads

static volatile LONG g_reload_items, g_reload_skills;   // set by refresh, done by port_mods_frame (game thread)

struct FileInfo { uint64_t size, mtime; uint32_t crc; };
static std::unordered_map<std::string, FileInfo> g_info;   // the last scan, with each file's content crc
static SRWLOCK g_scan_lock = SRWLOCK_INIT;              // one refresh at a time (watcher vs. editor)

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

// Collect every file under dir as lowercase keys with size and write time (skipping data/audio and data/movies).
static void scan(const std::string &dir, const std::string &rel, std::unordered_map<std::string, FileInfo> &out)
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
            FileInfo &fi = out[r];
            fi.size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            fi.mtime = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
            fi.crc = 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// A loose file's content crc; false while it can't be read (being written).
static bool file_crc(const std::string &key, uint32_t *crc, uint64_t *bytes)
{
    std::string path = std::string(port_game_dir()) + "\\" + key;
    std::replace(path.begin(), path.end(), '/', (char)0x5C);
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    static std::vector<uint8_t> buf(1 << 20);           // refresh() runs under g_scan_lock
    uint32_t c = 0;
    DWORD n;
    BOOL ok;
    while ((ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr)) && n) {
        c = crc32(buf.data(), n, c);
        *bytes += n;
    }
    CloseHandle(f);
    *crc = c;
    return ok != 0;
}

// Rescan the folder: the lookup set, each new or changed file's crc and the netplay hash (sorted paths + file crcs).
// `initial`: the startup scan (lists the files instead of the changes).
static void refresh(bool initial)
{
    AcquireSRWLockExclusive(&g_scan_lock);
    std::string root = std::string(port_game_dir()) + "\\data";
    std::unordered_map<std::string, FileInfo> now;
    scan(root, "data", now);
    DWORD t0 = GetTickCount();
    uint64_t bytes = 0;
    std::vector<std::string> added, changed, removed;
    for (auto &kv : now) {
        auto old = g_info.find(kv.first);
        if (old != g_info.end() && old->second.size == kv.second.size && old->second.mtime == kv.second.mtime) {
            kv.second.crc = old->second.crc;
            continue;
        }
        if (!file_crc(kv.first, &kv.second.crc, &bytes)) kv.second.mtime = 0;   // hashed again next time
        (old == g_info.end() ? added : changed).push_back(kv.first);
    }
    for (auto &kv : g_info)
        if (!now.count(kv.first)) removed.push_back(kv.first);
    std::vector<std::string> keys;
    keys.reserve(now.size());
    for (auto &kv : now) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    uint32_t crc = 0;
    for (const std::string &k : keys) {
        crc = crc32(k.data(), k.size() + 1, crc);
        crc = crc32(&now[k].crc, 4, crc);
    }
    std::unordered_set<std::string> set(keys.begin(), keys.end());
    AcquireSRWLockExclusive(&g_files_lock);
    g_files.swap(set);
    ReleaseSRWLockExclusive(&g_files_lock);
    g_info.swap(now);
    g_crc = keys.empty() ? 0 : crc ? crc : 1;           // 0 = no mods
    if (!initial)                                       // (a file still being written waits for the next rescan)
        for (auto *list : {&added, &changed, &removed})
            for (const std::string &k : *list) {
                if (list != &removed && !g_info[k].mtime) continue;
                if (k == "data/config/items.tok") InterlockedExchange(&g_reload_items, 1);
                if (k == "data/config/skills.tok") InterlockedExchange(&g_reload_skills, 1);
            }
    if (initial) {
        if (keys.empty()) {
            port_log("mods: no loose files under %s (gladius.bec only)\n", root.c_str());
        } else {
            port_log("mods: %u loose files under %s win over gladius.bec (%.1f MB hashed in %lu ms, crc %08X)\n",
                     (unsigned)keys.size(), root.c_str(), bytes / 1048576.0, GetTickCount() - t0, (unsigned)g_crc);
            for (size_t i = 0; i < keys.size() && i < 20; i++) port_log("  %s\n", keys[i].c_str());
            if (keys.size() > 20) port_log("  ... and %u more\n", (unsigned)(keys.size() - 20));
        }
    } else if (!added.empty() || !changed.empty() || !removed.empty()) {
        port_log("mods: data folder changed - %u new, %u changed, %u gone (%u loose files, crc %08X)\n",
                 (unsigned)added.size(), (unsigned)changed.size(), (unsigned)removed.size(), (unsigned)keys.size(),
                 (unsigned)g_crc);
        int shown = 0;
        for (auto *list : {&added, &changed, &removed})
            for (const std::string &k : *list)
                if (shown++ < 20)
                    port_log("  %s %s\n", list == &added ? "new" : list == &changed ? "changed" : "gone", k.c_str());
        if (shown > 20) port_log("  ... and %d more\n", shown - 20);
    }
    ReleaseSRWLockExclusive(&g_scan_lock);
}

// Watcher: a change anywhere under data\ (or the folder appearing / going away) rescans once things go quiet for
// 300 ms, so a tool copying many files rescans once.
static DWORD WINAPI watch_thread(void *)
{
    std::string root = std::string(port_game_dir()) + "\\data";
    for (;;) {
        HANDLE h = FindFirstChangeNotificationA(root.c_str(), TRUE,
                                                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (h == INVALID_HANDLE_VALUE) {                // no data folder (yet)
            Sleep(2000);
            if (GetFileAttributesA(root.c_str()) != INVALID_FILE_ATTRIBUTES) refresh(false);
            continue;
        }
        for (;;) {
            DWORD w = WaitForSingleObject(h, 2000);
            if (w == WAIT_TIMEOUT) {
                if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) break;   // folder deleted / renamed
                continue;
            }
            if (w != WAIT_OBJECT_0 || !FindNextChangeNotification(h)) break;
            while (WaitForSingleObject(h, 300) == WAIT_OBJECT_0)
                if (!FindNextChangeNotification(h)) break;
            refresh(false);
        }
        FindCloseChangeNotification(h);
        refresh(false);
    }
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

// Pre-tokenized copies of a .tok: X.tok.brf + X_lines.bin + X_strings.bin (skills: the loader 0xB7390 uses them when
// 0xB5DE0 finds X.tok.brf and otherwise tokenizes X.tok itself). With a loose X.tok they count as missing, from the
// bec and the loose folder alike, so the text is what loads and mods need only the .tok.
static bool tok_shadowed(const char *name)
{
    if (!name) return false;
    std::string k = key_of(name), base;
    auto ends = [&](const char *s) {
        size_t n = strlen(s);
        return k.size() > n && !k.compare(k.size() - n, n, s);
    };
    if (ends(".tok.brf")) base = k.substr(0, k.size() - 4);
    else if (ends("_lines.bin")) base = k.substr(0, k.size() - 10) + ".tok";
    else if (ends("_strings.bin")) base = k.substr(0, k.size() - 12) + ".tok";
    else return false;
    AcquireSRWLockShared(&g_files_lock);
    bool have = g_files.count(base) != 0;
    ReleaseSRWLockShared(&g_files_lock);
    if (have) {
        AcquireSRWLockExclusive(&g_used_lock);
        bool first = g_used.insert(k + "|shadowed").second;
        ReleaseSRWLockExclusive(&g_used_lock);
        if (first) port_log("mods: %s ignored - loose %s loads as text\n", k.c_str(), base.c_str());
    }
    return have;
}

typedef void *(__thiscall *BecLookup)(void *mgr, const char *name, uint32_t *size);
static const BecLookup bec_lookup = (BecLookup)0x1FA680;

void port_editor_note_file(const char *name);         // editor.cpp: the level's prop files
// Bec index lookup: loose files are "not in the bec".
static void *__fastcall lookup_hook(void *mgr, void *, const char *name, uint32_t *size)
{
    port_editor_note_file(name);
    if (tok_shadowed(name)) return nullptr;
    return loose(name, "open") ? nullptr : bec_lookup(mgr, name, size);
}

// File-exists check: loose files count as present, shadowed .tok binaries as absent.
static void *__fastcall exists_hook(void *mgr, void *, const char *name, uint32_t *size)
{
    if (tok_shadowed(name)) return nullptr;
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
    if (tok_shadowed(full.c_str())) return nullptr;
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

// Editor: rescan the folder after saving files (right away, without waiting for the watcher).
void port_mods_rescan()
{
    refresh(false);
}

// Live items.tok / skills.tok: the startup loaders run again and update the records in place, so every pointer and
// index the game holds (equipment, shop stock, learned skills, class lists) stays valid and sees the new values.
//   items  (0x790A0): records of 0x60 bytes at [0x3AD768], [0x3AD770] used. The one ITEMCREATE call (0x7928D ->
//          0x78540) resets and refills the next record; while reloading it is pointed at the record of the same name
//          (0x77200, find by name) instead, so only items new to the file are appended. Then 0x789E0 links each
//          item's skill names (+0x40 / +0x44) to skills again.
//   skills (0xB7390): SKILLCREATE (0xB7250) already redefines a skill that exists (unhooks it from the name table,
//          resets the record with 0xB7000 and rehooks it), so the loader just runs again. Its pre-tokenized copy
//          (skills.tok.brf + _lines / _strings, read at startup into [0x3F0720..28]) is dropped first so the loader
//          checks again and a loose skills.tok loads as text.
// Run on the game thread from the pad poll hook (before the frame's game code), never while a netplay session or
// replay runs (the other PCs wouldn't have the change) - those wait for it to end. Items or skills removed from a file
// keep their old records; more items / skills than the startup count allowed (ITEMCOUNT / SKILLCOUNT) aren't added.
bool gn_det_active();                                   // gn_core.cpp
static bool g_items_reloading;

typedef void *(__cdecl *ItemCreate)(const char *name, void *, void *, void *, void *);
static void *__cdecl item_create_hook(const char *name, void *a, void *b, void *c, void *d)
{
    const ItemCreate create = (ItemCreate)0x78540;
    BYTE *rec = g_items_reloading && name ? ((BYTE *(__cdecl *)(const char *))0x77200)(name) : nullptr;
    if (!rec) return create(name, a, b, c, d);
    int &used = *(int *)0x3AD770, keep = used;
    used = (int)((rec - *(BYTE **)0x3AD768) / 0x60);    // 0x78540 fills record [used]
    void *r = create(name, a, b, c, d);
    used = keep;
    return r;
}

static bool install_reload_hooks()
{
    const DWORD site = 0x7928D;
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != 0x78540 || *(DWORD *)0x6DA90 != 0x343B34 ||
        *(DWORD *)0x6DAB3 != 0x343B04 || *(DWORD *)0xB5E05 != 0x348AF0) {   // push items / skills / .brf name
        port_log("mods: config loader code unexpected - no live items.tok / skills.tok reloads\n");
        return false;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)item_create_hook - (site + 5));
    return true;
}

static bool g_reload_hooked;

void port_mods_frame()
{
    if (!g_reload_hooked || !(g_reload_items | g_reload_skills)) return;
    static bool told;
    if (gn_det_active()) {
        if (!told) port_log("mods: config reload waits - a netplay session / replay is running\n");
        told = true;
        return;
    }
    told = false;
    DWORD t0 = GetTickCount();
    if (InterlockedExchange(&g_reload_skills, 0)) {
        // A redefinition drops the skill's name (0x1CB1A0, freed at 0 references) before interning it again
        // (0x1CB0C0); an extra reference held across the reload keeps every name where it is, for anything else
        // holding it.
        // The skill name table is the string table object at 0x3F0B68 (both are thiscall on it).
        typedef const char *(__thiscall *Intern)(void *table, const char *);
        typedef void(__thiscall *Release)(void *table, const char *);
        void *const table = (void *)0x3F0B68;
        const char *const fallback = (const char *)0x37D978;   // "SkillStringBufferError" (not in the table)
        int before = *(int *)0x3F0744;
        std::vector<const char *> names;
        for (int i = 0; i < before; i++) {
            const char *n = *(const char **)(*(BYTE **)0x3F073C + i * 0x48);
            if (!n || n == fallback) continue;
            const char *r = ((Intern)0x1CB0C0)(table, n);
            if (r == n) names.push_back(n);
            else if (r) ((Release)0x1CB1A0)(table, r);  // not this table's string: leave it alone
        }
        *(DWORD *)0x3F0720 = *(DWORD *)0x3F0724 = *(DWORD *)0x3F0728 = 0;
        ((void(__cdecl *)(const char *))0xB7390)("$d\\Config\\skills.tok");
        for (const char *n : names) ((Release)0x1CB1A0)(table, n);
        ((void(__cdecl *)())0x789E0)();                 // items' skills: new skills can now be found
        port_log("mods: skills.tok reloaded - %d skills (%d new) in %lu ms\n", *(int *)0x3F0744,
                 *(int *)0x3F0744 - before, GetTickCount() - t0);
    }
    if (InterlockedExchange(&g_reload_items, 0)) {
        int before = *(int *)0x3AD770;
        g_items_reloading = true;
        ((void(__cdecl *)(const char *))0x790A0)("$d\\Config\\items.tok");
        g_items_reloading = false;
        ((void(__cdecl *)())0x789E0)();
        port_log("mods: items.tok reloaded - %d items (%d new) in %lu ms\n", *(int *)0x3AD770,
                 *(int *)0x3AD770 - before, GetTickCount() - t0);
    }
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

// buildpaks\gamestartup.pak bundles the startup config (items.tok, classdefs.tok, skills.tok.brf / _lines / _strings,
// prizes.tok, the units\*.idx tables, CurSchool.tok, SwingMeter.tok). The GC data has no such pak and the loaders
// read each file on their own, so skip it: the mount call (0x1E5D00, one current pak) gets a name that doesn't
// exist and takes its "no file" return, and the config comes from the loose folder or gladius.bec file by file.
typedef void(__cdecl *PakMountFn)(const char *name);
static void __cdecl startup_pak_mount(const char *)
{
    ((PakMountFn)0x1E5D00)("$d\\buildpaks\\gamestartup_disabled.pak");
}

static void skip_startup_pak()
{
    const DWORD site = 0x6DA8A;
    BYTE *p = (BYTE *)site;
    if (*(BYTE *)0x6DA85 != 0x68 || *(DWORD *)0x6DA86 != 0x343B48 || p[0] != 0xE8 ||
        site + 5 + *(int32_t *)(p + 1) != 0x1E5D00) {
        port_log("mods: startup code unexpected - gamestartup.pak still loads\n");
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)startup_pak_mount - (site + 5));
    port_log("mods: gamestartup.pak skipped - startup config loads file by file\n");
}

// Scan the loose folder, hash it for netplay, install the lookup hooks and start the watcher.
void port_mods_init()
{
    raise_sound_caps();
    skip_startup_pak();
    char ini[MAX_PATH];                                 // [mods] LiveReload: watch data\ while the game runs
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    bool live = GetPrivateProfileIntA("mods", "LiveReload", 1, ini) != 0 || port_is_editor();
    refresh(true);
    if (g_files.empty() && !live) return;               // gladius.bec only, no hooks
    if (!install_hooks()) {
        AcquireSRWLockExclusive(&g_files_lock);
        g_files.clear();
        ReleaseSRWLockExclusive(&g_files_lock);
        g_crc = 0;
        return;
    }
    if (live) {
        g_reload_hooked = install_reload_hooks();
        CloseHandle(CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr));
        port_log("mods: live reload on - data folder changes count while the game runs%s\n",
                 g_reload_hooked ? " (items.tok / skills.tok reload in place)" : "");
    }
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
