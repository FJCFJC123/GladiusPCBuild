// HD texture replacement ([textures] Pack = folder of <name>.png). Named textures are built by
// tex_Bitmap::BuildFromPTTP (0x1EC250, loose .tga) and BuildFromBundleEntry (0x1EC600, .ptx bundles); their callers
// are redirected here to note Data -> (name, Format, content sample). At load time a background job decodes the PNG,
// checks it is the same picture as the Xbox texture, rebuilds any padding, builds mips and creates the D3D11 texture.
// The game thread never waits: the original shows until the HD one is ready. Swizzled textures only (linear ones
// address texels in pixels).
#include "d3d.h"
#include <wincodec.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

DWORD d3d_tex_sample_sum(DWORD data, DWORD format, DWORD size);   // d3d_render.cpp

enum { THUMB = 32 };                                            // content check: THUMB x THUMB samples
struct NameRec {
    std::string key;
    DWORD format, sum;
    UINT real_w, real_h;                                        // image size before power-of-two padding
};
struct Job {
    NameRec r;
    DWORD thumb[THUMB * THUMB];                                 // Xbox texels (BGRA) over the image area
};
struct HdState {
    bool done;
    ID3D11ShaderResourceView *srv;                              // nullptr once done = no replacement
};
static CRITICAL_SECTION g_cs;                                   // everything below
static CONDITION_VARIABLE g_cv;
static std::map<DWORD, NameRec> g_names;                       // Data address -> texture built there
volatile LONG g_tex_names_gen = 1;                              // bumped on every g_names change (render-side caches)
static std::map<std::string, HdState> g_state;                 // key -> HD load state
static std::deque<Job *> g_jobs;
static std::map<std::string, std::string> g_index;          // key -> pack file name (read-only after init)
static char g_pack[MAX_PATH];
static bool g_enabled, g_same_size;
static int g_max_diff;
static unsigned g_hits, g_misses;

// pack lookup key: lower case, extensions stripped - "MAIN1" (bundle entry), "$t\gui\cursor.tga" (loose file) and
// "main1.tga.png" (pack file) all meet as "main1"
static std::string pack_key(const char *name)
{
    std::string k(name);
    for (char &c : k)
        c = (char)tolower((unsigned char)c);
    for (int pass = 0; pass < 2; pass++)
        for (const char *ext : {".png", ".tga", ".bmp"})
            if (k.size() > 4 && !k.compare(k.size() - 4, 4, ext))
                k.resize(k.size() - 4);
    return k;
}

static void tex_dims(DWORD format, UINT *w, UINT *h)
{
    *w = 1u << ((format >> 20) & 0xF);
    *h = 1u << ((format >> 24) & 0xF);
}

// the texture's image area (padded textures: the top-left real_w x real_h)
static void image_area(const NameRec &r, UINT *iw, UINT *ih)
{
    UINT xw, xh;
    tex_dims(r.format, &xw, &xh);
    *iw = r.real_w && r.real_w <= xw ? r.real_w : xw;
    *ih = r.real_h && r.real_h <= xh ? r.real_h : xh;
}

// Queue the HD load for this texture (caller holds g_cs); Data must hold the texture's pixels right now.
static void request(const NameRec &r, DWORD data)
{
    if (g_state.count(r.key))
        return;
    if (!g_index.count(r.key)) {
        g_state[r.key] = {true, nullptr};
        UINT xw, xh;
        tex_dims(r.format, &xw, &xh);
        if (g_misses++ < 300)
            port_log("textures: no replacement for %s (%ux%u)\n", r.key.c_str(), xw, xh);
        return;
    }
    Job *j = new Job;
    j->r = r;
    UINT iw, ih;
    image_area(r, &iw, &ih);
    for (UINT y = 0; y < THUMB; y++)
        for (UINT x = 0; x < THUMB; x++)
            j->thumb[y * THUMB + x] = d3d_texel(data, r.format, (x * 2 + 1) * iw / (THUMB * 2), (y * 2 + 1) * ih / (THUMB * 2));
    g_state[r.key] = {false, nullptr};
    g_jobs.push_back(j);
    WakeConditionVariable(&g_cv);
}

static void note_bitmap(void *bitmap)                           // bitmap built: note Data -> its name, start loading
{
    const BYTE *b = (const BYTE *)bitmap;
    const XPixelContainer *t = (const XPixelContainer *)(b + 0x74);
    if (!t->Data || t->Data == 0xFFFFFFFF || t->Size)
        return;
    DWORD fmt = (t->Format >> 8) & 0xFF;
    if (!d3d_xformat(fmt).dxt && d3d_xformat(fmt).linear)
        return;
    const char *name = (const char *)(b + 0x20), *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    if (!*base)
        return;
    NameRec r = {pack_key(base), t->Format, d3d_tex_sample_sum(t->Data, t->Format, 0), *(const WORD *)(b + 0x18),
                 *(const WORD *)(b + 0x1A)};
    EnterCriticalSection(&g_cs);
    g_names[t->Data] = r;
    InterlockedIncrement(&g_tex_names_gen);
    if (g_enabled)
        request(r, t->Data);
    LeaveCriticalSection(&g_cs);
}

typedef void(__fastcall *BuildFn)(void *bitmap, void *edx, void *data, int flag);
static void __fastcall build_from_pttp(void *bitmap, void *edx, void *data, int flag)
{
    ((BuildFn)0x1EC250)(bitmap, edx, data, flag);
    note_bitmap(bitmap);
}

// tex_Bitmap::BuildFromBundleEntry (0x1EC600). Entry: +1 'A'/'B'/'C' (DXT1/3/5), +0x10 data offset (0 = a piece of
// the shared atlas, skipped), +0x14 name.
typedef void *(__fastcall *BuildEntryFn)(void *bitmap, void *edx, const BYTE *entry, int param, int flags);
static void *__fastcall build_from_entry(void *bitmap, void *edx, const BYTE *entry, int param, int flags)
{
    void *r = ((BuildEntryFn)0x1EC600)(bitmap, edx, entry, param, flags);
    if (*(const DWORD *)(entry + 0x10))
        note_bitmap(bitmap);
    return r;
}

// ---------------------------------------------------------------- worker side
// Decode a PNG to BGRA with WIC.
static bool load_png(IWICImagingFactory *wic, const char *path, std::vector<BYTE> &px, UINT *w, UINT *h)
{
    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2);
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    bool ok = SUCCEEDED(wic->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                       &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(w, h)) && *w && *h;
    if (ok) {
        px.resize((size_t)*w * *h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, *w * 4, (UINT)px.size(), px.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

// How far the HD image is from the Xbox texels: mean over a thumbnail grid of the alpha / colour difference, 0..255.
static int image_diff(const Job &j, const std::vector<BYTE> &px, UINT w, UINT h, UINT cover_w, UINT cover_h)
{
    UINT iw, ih;                                                // the thumb samples are over the image area
    image_area(j.r, &iw, &ih);
    float sx = (float)w / cover_w, sy = (float)h / cover_h;     // the HD image spans cover_w x cover_h Xbox texels
    unsigned long long total = 0;
    for (UINT ty = 0; ty < THUMB; ty++)
        for (UINT tx = 0; tx < THUMB; tx++) {
            UINT xx = (tx * 2 + 1) * iw / (THUMB * 2), yy = (ty * 2 + 1) * ih / (THUMB * 2);
            UINT x0 = (UINT)(xx * sx), x1 = (UINT)((xx + 1) * sx), y0 = (UINT)(yy * sy), y1 = (UINT)((yy + 1) * sy);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            unsigned s[4] = {}, n = 0;
            for (UINT y = y0; y < y1; y++)
                for (UINT x = x0; x < x1; x++, n++)
                    for (int c = 0; c < 4; c++)
                        s[c] += px[((size_t)y * w + x) * 4 + c];
            if (!n)
                continue;
            DWORD o = j.thumb[ty * THUMB + tx];
            int hb = s[0] / n, hg = s[1] / n, hr = s[2] / n, ha = s[3] / n;
            int ob = o & 255, og = (o >> 8) & 255, orr = (o >> 16) & 255, oa = o >> 24;
            int da = abs(ha - oa);
            int dc = (abs(hb - ob) + abs(hg - og) + abs(hr - orr)) / 3 * (ha < oa ? ha : oa) / 255;
            total += da > dc ? da : dc;
        }
    return (int)(total / (THUMB * THUMB));
}

// Padded texture (e.g. a 306x276 portrait in 512x512): when the pack image has the image's shape at a whole scale,
// rebuild the Xbox layout at that scale.
static bool pad_to_texture(std::vector<BYTE> &px, UINT *w, UINT *h, const NameRec &r, UINT xw, UINT xh)
{
    if (!r.real_w || !r.real_h || (r.real_w == xw && r.real_h == xh) || r.real_w > xw || r.real_h > xh)
        return false;
    UINT s = *w / r.real_w;
    if (!s || *w != r.real_w * s || *h != r.real_h * s)
        return false;
    UINT tw = xw * s, th = xh * s;
    std::vector<BYTE> out((size_t)tw * th * 4, 0);
    for (UINT y = 0; y < *h; y++)
        memcpy(&out[(size_t)y * tw * 4], &px[(size_t)y * *w * 4], *w * 4);
    px.swap(out);
    *w = tw, *h = th;
    return true;
}

// Immutable BGRA8 texture with a box-filtered mip chain.
static ID3D11ShaderResourceView *make_srv(std::vector<BYTE> &px, UINT w, UINT h)
{
    std::vector<std::vector<BYTE>> levels;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    levels.push_back(std::move(px));
    UINT lw = w, lh = h;
    init.push_back({levels.back().data(), lw * 4, 0});
    while (lw > 1 || lh > 1) {
        UINT nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        const std::vector<BYTE> &src = levels.back();
        std::vector<BYTE> dst((size_t)nw * nh * 4);
        for (UINT y = 0; y < nh; y++)
            for (UINT x = 0; x < nw; x++) {
                UINT x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : x0;
                UINT y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : y0;
                for (int c = 0; c < 4; c++)
                    dst[((size_t)y * nw + x) * 4 + c] =
                        (BYTE)((src[((size_t)y0 * lw + x0) * 4 + c] + src[((size_t)y0 * lw + x1) * 4 + c] +
                                src[((size_t)y1 * lw + x0) * 4 + c] + src[((size_t)y1 * lw + x1) * 4 + c] + 2) / 4);
            }
        levels.push_back(std::move(dst));
        lw = nw, lh = nh;
        init.push_back({levels.back().data(), lw * 4, 0});
    }
    for (size_t i = 0; i < levels.size(); i++)                  // vector moves may have relocated the buffers
        init[i].pSysMem = levels[i].data();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = (UINT)levels.size();
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(g_d3d.dev->CreateTexture2D(&td, init.data(), &tex)) || !tex)
        return nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    g_d3d.dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

static ID3D11ShaderResourceView *load_job(IWICImagingFactory *wic, const Job &j)
{
    const NameRec &r = j.r;
    UINT xw, xh, w = 0, h = 0;
    tex_dims(r.format, &xw, &xh);
    char path[MAX_PATH * 2];
    _snprintf(path, sizeof path, "%s\\%s", g_pack, g_index.find(r.key)->second.c_str());
    std::vector<BYTE> px;
    if (!load_png(wic, path, px, &w, &h)) {
        port_log("textures: %s could not be decoded\n", path);
        return nullptr;
    }
    // the HD image covers the image area: padded ones (shape of the area) or the whole texture (shape of the texture)
    UINT iw, ih;
    image_area(r, &iw, &ih);
    bool padded = (iw != xw || ih != xh) && w * ih == h * iw && w * xh != h * xw;
    if (!padded && w * xh != h * xw) {
        port_log("textures: %s is %ux%u, not the Xbox %ux%u (image %ux%u) shape - kept\n", r.key.c_str(), w, h, xw, xh,
                 iw, ih);
        return nullptr;
    }
    int d = padded ? image_diff(j, px, w, h, iw, ih) : image_diff(j, px, w, h, xw, xh);
    if (d > g_max_diff) {
        port_log("textures: %s differs from the Xbox image (diff %d) - kept\n", r.key.c_str(), d);
        return nullptr;
    }
    if (padded && !pad_to_texture(px, &w, &h, r, xw, xh)) {
        port_log("textures: %s is %ux%u, not a whole multiple of the %ux%u image - kept\n", r.key.c_str(), w, h, iw, ih);
        return nullptr;
    }
    if (w * h <= xw * xh && !g_same_size) {
        port_log("textures: %s is %ux%u, not larger than the Xbox %ux%u - kept\n", r.key.c_str(), w, h, xw, xh);
        return nullptr;
    }
    ID3D11ShaderResourceView *srv = make_srv(px, w, h);
    if (srv && g_hits++ < 300)
        port_log("textures: %s %ux%u -> %ux%u (diff %d)\n", r.key.c_str(), xw, xh, w, h, d);
    return srv;
}

static DWORD WINAPI worker(void *)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        port_log("textures: WIC unavailable - worker stopped\n");
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        EnterCriticalSection(&g_cs);
        while (g_jobs.empty())
            SleepConditionVariableCS(&g_cv, &g_cs, INFINITE);
        Job *j = g_jobs.front();
        g_jobs.pop_front();
        LeaveCriticalSection(&g_cs);
        ID3D11ShaderResourceView *srv = load_job(wic, *j);
        EnterCriticalSection(&g_cs);
        g_state[j->r.key] = {true, srv};
        LeaveCriticalSection(&g_cs);
        delete j;
    }
}

// ---------------------------------------------------------------- render side
// Asset name key ("shadow", "sky_noon_clear_l", ...) of the named texture at this Data address; "" if unknown or the
// memory now holds something else.
std::string tex_key(DWORD data, DWORD format)
{
    std::string k;
    EnterCriticalSection(&g_cs);
    auto it = g_names.find(data);
    if (it != g_names.end() && it->second.format == format)
        k = it->second.key;
    LeaveCriticalSection(&g_cs);
    return k;
}

ID3D11ShaderResourceView *tex_replace_srv(DWORD data, DWORD format, DWORD size, bool *pending)
{
    *pending = false;
    if (!g_enabled || !g_d3d.hd_textures || size)
        return nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    EnterCriticalSection(&g_cs);
    auto it = g_names.find(data);
    if (it != g_names.end() && it->second.format == format && it->second.sum == d3d_tex_sample_sum(data, format, 0)) {
        request(it->second, data);
        HdState &st = g_state[it->second.key];
        if (!st.done)
            *pending = true;
        else if ((srv = st.srv) != nullptr)
            srv->AddRef();                                      // the caller's reference; the state keeps its own
    }
    LeaveCriticalSection(&g_cs);
    return srv;
}

static void redirect(DWORD site, DWORD target, void *fn)
{
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t *)(p + 1) != target) {
        port_log("textures: unexpected call at %06X, not hooked\n", site);
        return;
    }
    *(int32_t *)(p + 1) = (int32_t)((DWORD)fn - (site + 5));
}

void port_tex_replace_init(const char *ini)
{
    InitializeCriticalSection(&g_cs);
    InitializeConditionVariable(&g_cv);
    if (GetPrivateProfileIntA("debug", "NoTexHooks", 0, ini)) {   // leave the engine's texture builders alone (bisect)
        port_log("textures: name hooks OFF ([debug] NoTexHooks) - no HD textures / blob-shadow hiding\n");
        return;
    }
    redirect(0x1EC8EC, 0x1EC250, (void *)build_from_pttp);     // names always (tex_key: blob shadows, sky ...)
    redirect(0x1EC9C9, 0x1EC250, (void *)build_from_pttp);
    redirect(0x1797A8, 0x1EC600, (void *)build_from_entry);
    char val[MAX_PATH];
    GetPrivateProfileStringA("textures", "Pack", "", val, sizeof val, ini);
    if (!val[0])
        return;
    if (val[1] == ':' || val[0] == '\\')
        strcpy(g_pack, val);
    else {                                                      // relative to the exe
        GetModuleFileNameA(nullptr, g_pack, MAX_PATH);
        strcpy(strrchr(g_pack, '\\') + 1, val);
    }
    size_t n = strlen(g_pack);
    while (n && (g_pack[n - 1] == '\\' || g_pack[n - 1] == '/'))
        g_pack[--n] = 0;
    DWORD a = GetFileAttributesA(g_pack);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        port_log("textures: pack folder %s not found - replacement off\n", g_pack);
        return;
    }
    // [textures] Exclude = comma-separated name patterns (* ?) kept original. Default: eyes (the cinematic models reuse
    // eye texture names with another layout).
    char excl[512];
    GetPrivateProfileStringA("textures", "Exclude", "eye*,*_eye*,*eyes*", excl, sizeof excl, ini);
    std::vector<std::string> pats;
    for (char *t = strtok(excl, ", "); t; t = strtok(nullptr, ", "))
        pats.push_back(t);
    auto wild = [](const char *p, const char *n) {      // case-insensitive * / ? match
        const char *star = nullptr, *ss = nullptr;
        while (*n) {
            if (*p == '*') { star = p++, ss = n; continue; }
            if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*n)) { p++, n++; continue; }
            if (star) { p = star + 1, n = ++ss; continue; }
            return false;
        }
        while (*p == '*')
            p++;
        return !*p;
    };
    unsigned excluded = 0;
    char pat[MAX_PATH + 8];
    _snprintf(pat, sizeof pat, "%s\\*.png", g_pack);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            std::string k = pack_key(fd.cFileName);
            bool skip = false;
            for (const std::string &x : pats)
                skip = skip || wild(x.c_str(), k.c_str());
            if (skip)
                excluded++;
            else
                g_index[k] = fd.cFileName;
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
        if (excluded)
            port_log("textures: %u pack images excluded ([textures] Exclude)\n", excluded);
    }
    g_same_size = GetPrivateProfileIntA("textures", "ReplaceSameSize", 0, ini) != 0;
    g_max_diff = GetPrivateProfileIntA("textures", "MaxDiff", 24, ini);
    int threads = GetPrivateProfileIntA("textures", "Threads", 3, ini);
    for (int i = 0; i < (threads < 1 ? 1 : threads > 8 ? 8 : threads); i++)
        CloseHandle(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
    g_enabled = true;
    g_d3d.hd_textures = true;                                   // F4 toggles
    port_log("textures: replacement pack %s (%u images), %d workers, max diff %d%s\n", g_pack,
             (unsigned)g_index.size(), threads, g_max_diff, g_same_size ? ", same-size too" : "");
}
