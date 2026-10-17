// XGRAPHICS shader assembler replacement. The engine generates vertex/pixel shader SOURCE TEXT at runtime and
// assembles it with XGAssembleShader (0x2F0DE3) into Xbox microcode / a register-combiner definition, which it
// copies into its program pool and later hands to LoadVertexShaderProgram / SelectVertexShaderDirect /
// SetPixelShaderProgram. Here the source is recorded (id), and the "compiled" output is a correctly shaped
// placeholder that carries the id, so the D3D11 side knows which (HLSL-translated) shader is meant.
//   vertex: 4-byte header {0x78, 1, instruction count, 0} + count * 16 bytes, tag in the first slot
//   pixel : 240-byte D3DPIXELSHADERDEF, tag in its first two dwords
#include "d3d.h"
#include "container.h"
#include <string>
#include <vector>
#include <map>

static const DWORD TAG_VS = 0x53565047, TAG_PS = 0x53505047;      // 'GPVS', 'GPPS'
static std::vector<XgShader> g_shaders;                // local ids (keys of the D3D11 caches)
static std::map<DWORD, DWORD> g_by_hash;               // source hash -> local id
static SRWLOCK g_shader_lock = SRWLOCK_INIT;

// Netplay: the tag in game memory carries the SOURCE HASH, not a local sequence number - each PC assembles shaders in
// its own order (menus, intro, lobby), so after a state sync the host's ids meant other shaders here (2026-10-07: the
// client drew the arena with the wrong programs - flat orange, smeared geometry). The sources themselves are kept in
// game memory (registry below, in the container's shared page), so a hash this PC never assembled is resolved from
// the host's copy after a sync.
struct SyncedSource { DWORD hash, len; char text[1]; };
struct Registry { DWORD count, cap; SyncedSource **list; };     // shared page +0xA40 (game memory)
static Registry g_local_registry;
static Registry *registry()
{
    char *page = (char *)container_shared_page();
    return page ? (Registry *)(page + 0xA40) : &g_local_registry;
}

static DWORD source_hash(const std::string &s)
{
    DWORD h = 0x811C9DC5;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193;
    return h ? h : 1;
}

// caller holds the lock exclusively
static DWORD local_id(DWORD hash, bool pixel, const std::string &text)
{
    auto it = g_by_hash.find(hash);
    if (it != g_by_hash.end())
        return it->second;
    DWORD id = (DWORD)g_shaders.size();
    g_shaders.push_back({pixel, text});
    g_by_hash[hash] = id;
    return id;
}

static void registry_add(DWORD hash, const std::string &text)
{
    Registry *r = registry();
    for (DWORD i = 0; i < r->count; i++)
        if (r->list[i]->hash == hash)
            return;
    if (r->count == r->cap) {                           // grow (game memory: synced with the rest)
        DWORD cap = r->cap ? r->cap * 2 : 128;
        SyncedSource **l = (SyncedSource **)game_alloc(cap * sizeof *l);
        if (r->list) memcpy(l, r->list, r->count * sizeof *l);
        r->list = l;                                    // the old table is left (small, and never freed: a state
        r->cap = cap;                                   // sync may still be copying it)
    }
    SyncedSource *s = (SyncedSource *)game_alloc(sizeof(SyncedSource) + text.size());
    s->hash = hash;
    s->len = (DWORD)text.size();
    memcpy(s->text, text.data(), text.size());
    r->list[r->count++] = s;
}

// tag hash -> local id (registers a source found only in game memory, e.g. the host's after a sync)
static DWORD resolve(DWORD hash)
{
    AcquireSRWLockShared(&g_shader_lock);
    auto it = g_by_hash.find(hash);
    DWORD id = it != g_by_hash.end() ? it->second : 0xFFFFFFFF;
    ReleaseSRWLockShared(&g_shader_lock);
    if (id != 0xFFFFFFFF)
        return id;
    Registry *r = registry();
    for (DWORD i = 0; i < r->count; i++) {
        const SyncedSource *s = r->list[i];
        if (s->hash != hash)
            continue;
        std::string text(s->text, s->len);
        size_t first = text.find_first_not_of(" \t\r\n");
        bool pixel = first != std::string::npos && (text.compare(first, 2, "ps") == 0 || text.compare(first, 3, "xps") == 0);
        AcquireSRWLockExclusive(&g_shader_lock);
        id = local_id(hash, pixel, text);
        ReleaseSRWLockExclusive(&g_shader_lock);
        port_log("shader %08X: source taken from game memory (synced) -> local #%u\n", hash, id);
        return id;
    }
    port_log("shader %08X: unknown here and not in the synced registry\n", hash);
    return 0xFFFFFFFF;
}

const XgShader *xg_shader(DWORD id)
{
    AcquireSRWLockShared(&g_shader_lock);
    const XgShader *s = id < g_shaders.size() ? &g_shaders[id] : nullptr;
    ReleaseSRWLockShared(&g_shader_lock);
    return s;
}

DWORD xg_tag_vs_hash(const BYTE *microcode)            // -> source hash or 0
{
    return microcode && *(const DWORD *)(microcode + 4) == TAG_VS ? *(const DWORD *)(microcode + 8) : 0;
}
DWORD xg_hash_id(DWORD hash) { return hash ? resolve(hash) : 0xFFFFFFFF; }
DWORD xg_tag_vs(const BYTE *microcode) { return xg_hash_id(xg_tag_vs_hash(microcode)); }   // -> local id
DWORD xg_tag_ps(const DWORD *psdef)
{
    return psdef && psdef[0] == TAG_PS ? xg_hash_id(psdef[1]) : 0xFFFFFFFF;
}

struct XgBuffer {                                      // XGRAPHICS XGBuffer: refcount, data, size
    DWORD refs;
    void *data;
    DWORD size;
};

static XgBuffer *make_buffer(DWORD size)
{
    XgBuffer *b = (XgBuffer *)game_alloc(sizeof(XgBuffer));      // game-visible (memory container)
    b->refs = 1;
    b->data = game_alloc(size ? size : 4);
    b->size = size;
    return b;
}

static int count_instructions(const std::string &src)
{
    int n = 0;
    size_t i = 0;
    while (i < src.size()) {
        size_t e = src.find('\n', i);
        if (e == std::string::npos)
            e = src.size();
        std::string line = src.substr(i, e - i);
        i = e + 1;
        size_t c = line.find_first_not_of(" \t\r");
        if (c == std::string::npos)
            continue;
        line = line.substr(c);
        if (line.compare(0, 2, "//") == 0 || line[0] == ';' || line[0] == '#' || line.compare(0, 3, "vs.") == 0 ||
            line.compare(0, 4, "xvs.") == 0 || line.compare(0, 3, "ps.") == 0 || line.compare(0, 4, "xps.") == 0 ||
            line.compare(0, 5, "xvss.") == 0 || line.compare(0, 3, "def") == 0)
            continue;
        n++;
    }
    return n;
}

static void save_source(DWORD id, bool pixel, const std::string &src)
{
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char *slash = strrchr(path, 0x5C);
    strcpy(slash + 1, "shaders");
    CreateDirectoryA(path, nullptr);
    _snprintf(slash + 1, 64, "shaders\\%s_%03u.txt", pixel ? "ps" : "vs", id);
    if (FILE *f = fopen(path, "wb")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
}

extern "C" {

// HRESULT XGAssembleShader(name, src, len, flags, ppConstants, ppCompiled, ppErrors, ppListing, include, ctx, ppHFile)
HRESULT __stdcall xg_XGAssembleShader(const char *name, const char *src, DWORD len, DWORD flags, XgBuffer **constants,
                                      XgBuffer **compiled, XgBuffer **errors, XgBuffer **listing, void *include,
                                      void *ctx, XgBuffer **hfile)
{
    std::string text(src, len);
    size_t first = text.find_first_not_of(" \t\r\n");
    bool pixel = first != std::string::npos && (text.compare(first, 2, "ps") == 0 || text.compare(first, 3, "xps") == 0);
    DWORD hash = source_hash(text);
    AcquireSRWLockExclusive(&g_shader_lock);
    DWORD id = local_id(hash, pixel, text);
    registry_add(hash, text);
    ReleaseSRWLockExclusive(&g_shader_lock);
    save_source(id, pixel, text);

    if (constants) *constants = nullptr;
    if (errors) *errors = nullptr;
    if (listing) *listing = nullptr;
    if (hfile) *hfile = nullptr;
    if (!compiled)
        return S_OK;
    if (pixel) {
        XgBuffer *b = make_buffer(240);
        DWORD *def = (DWORD *)b->data;
        def[0] = TAG_PS;
        def[1] = hash;
        *compiled = b;
    } else {
        int n = count_instructions(text);
        if (n < 1) n = 1;
        if (n > 255) n = 255;
        XgBuffer *b = make_buffer(4 + n * 16);
        BYTE *p = (BYTE *)b->data;
        p[0] = 0x78;                                    // Xbox vertex shader program
        p[1] = 1;
        p[2] = (BYTE)n;
        *(DWORD *)(p + 4) = TAG_VS;
        *(DWORD *)(p + 8) = hash;
        *compiled = b;
    }
    static int logged;
    if (logged++ < 64)
        port_log("XGAssembleShader #%u (%s, %u bytes source) -> tagged %s\n", id, pixel ? "pixel" : "vertex", len,
                 pixel ? "PSDEF" : "microcode");
    return S_OK;
}

}
