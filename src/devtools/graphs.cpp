// Graphs tab: frame times every frame (always), the renderer profiler's CPU zones and GPU passes (switched on from
// the tab, or [debug] Profile=1), and memory four times a second (container arenas, the game's tracked heap, the
// process, every SmallHeap). The game thread samples into rings; the tools window reads them without a lock (one
// writer, and a sample being written while it is drawn is only a torn float).
#include "devtools.h"
#include <windows.h>
#include <psapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include "imgui.h"

static const float MB = 1.0f / 1048576.0f;

// ------------------------------------------------------------------------------------------------ samples
struct FrameSample {
    float wall, cpu, present;
    uint32_t draws;
    bool prof;
    float game, gpu;
    float zone[DEV_ZONES];
    float pass[DEV_PASSES];
    uint32_t counters[DEV_COUNTERS];
};

enum { ARENAS = 5 };
struct MemSample {
    float t;                                            // seconds since the first sample
    float arena[ARENAS];                                // MB in use
    float tracked;                                      // game tracked heap MB (0x418BD0)
    float priv, vspace;                                 // process private bytes, address space used (MB)
};

struct HeapRow {
    std::string name;
    uint32_t len, used, high;
};

static const int FRAME_RING = 3600, MEM_RING = 2400;   // 60 s at 60 fps, 10 min at 4 Hz
static FrameSample g_fr[FRAME_RING];
static volatile LONG g_fr_n;                            // samples written in all
static MemSample g_mem[MEM_RING];
static volatile LONG g_mem_n;

static std::mutex g_snap_lock;                          // the latest whole-table snapshots
static std::vector<HeapRow> g_heaps;
static DevArena g_arena[ARENAS];
static bool g_arena_ok[ARENAS];
static uint32_t g_tracked_blocks;
static int g_n_zones, g_n_passes, g_n_counters;
static bool g_prof_ini;
static volatile bool g_prof_want;

// ------------------------------------------------------------------------------------------------ guarded reads
static bool rd(uint32_t addr, void *out, size_t n)
{
    __try {
        memcpy(out, (const void *)(uintptr_t)addr, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint32_t rd32(uint32_t addr)
{
    uint32_t v = 0;
    return rd(addr, &v, 4) ? v : 0;
}

static void rd_str(uint32_t addr, char *buf, size_t cap)
{
    size_t i = 0;
    for (char c; i + 1 < cap && rd(addr + (uint32_t)i, &c, 1) && c; i++) buf[i] = c;
    buf[i] = 0;
}

// SmallHeap list 0x41E40C: node = heap; +0x6C size in dwords, +0x70 name, +0x74 bytes used, +0x78 high-water
static void read_heaps(std::vector<HeapRow> &out)
{
    int n = 0;
    for (uint32_t h = rd32(0x41E40C); h && n < 4096; h = rd32(h), n++) {
        uint32_t len = rd32(h + 0x6C) * 4;
        if (!len) continue;
        char name[64];
        uint32_t np = rd32(h + 0x70);
        if (np) rd_str(np, name, sizeof name);
        else strcpy(name, "?");
        out.push_back({name, len, rd32(h + 0x74), rd32(h + 0x78)});
    }
}

// ------------------------------------------------------------------------------------------------ game thread
void graphs_frame()
{
    DevFrameStats s;
    g_host->frame_stats(&s);
    static uint32_t last_frame;
    if (s.frame && s.frame != last_frame) {
        last_frame = s.frame;
        FrameSample &f = g_fr[g_fr_n % FRAME_RING];
        f.wall = s.wall_ms, f.cpu = s.cpu_ms, f.present = s.present_ms, f.draws = s.draws, f.prof = s.prof;
        f.game = s.game_ms, f.gpu = s.gpu_ms;
        memcpy(f.zone, s.zone_ms, sizeof f.zone);
        memcpy(f.pass, s.gpu_pass_ms, sizeof f.pass);
        memcpy(f.counters, s.counters, sizeof f.counters);
        g_n_zones = s.n_zones, g_n_passes = s.n_passes, g_n_counters = s.n_counters;
        g_prof_ini = s.prof_ini;
        InterlockedIncrement(&g_fr_n);
    }

    static DWORD t0, last;
    DWORD now = GetTickCount();
    if (!t0) t0 = now;
    if (last && now - last < 250) return;
    last = now;
    MemSample &m = g_mem[g_mem_n % MEM_RING];
    memset(&m, 0, sizeof m);
    m.t = (now - t0) / 1000.0f;
    DevArena a[ARENAS];
    bool ok[ARENAS];
    for (int i = 0; i < ARENAS; i++) {
        ok[i] = g_host->arena(i, &a[i]);
        m.arena[i] = ok[i] ? a[i].used * MB : 0;
    }
    m.tracked = rd32(0x418BD0) * MB;
    PROCESS_MEMORY_COUNTERS_EX pmc = {sizeof pmc};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        m.priv = pmc.PrivateUsage * MB;
    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) m.vspace = (float)((ms.ullTotalVirtual - ms.ullAvailVirtual) * MB);
    std::vector<HeapRow> heaps;
    read_heaps(heaps);
    {
        std::lock_guard<std::mutex> l(g_snap_lock);
        g_heaps.swap(heaps);
        memcpy(g_arena, a, sizeof a);
        memcpy(g_arena_ok, ok, sizeof ok);
        g_tracked_blocks = rd32(0x418BD4);
    }
    InterlockedIncrement(&g_mem_n);
}

// ------------------------------------------------------------------------------------------------ plot
struct Series {
    const char *name;
    ImU32 col;
    float (*get)(const void *sample, int arg);
    int arg;
    bool on = true;
};

static float nice_ceil(float v)
{
    if (v <= 0) return 1;
    float p = powf(10, floorf(log10f(v)));
    for (float m : {1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f})
        if (v <= m * p) return m * p;
    return 10 * p;
}

// n samples (sample(i), oldest first), one line or one stacked band per series. Many samples per pixel column:
// lines show the column maximum (spikes stay visible), stacked bands the column average.
static void plot(const char *id, std::vector<Series> &ser, int n, const void *(*sample)(int i, void *ctx), void *ctx,
                 float height, bool stacked, const float *guides, int n_guides, const char *unit,
                 void (*hover_label)(int i, void *ctx, char *out, size_t cap))
{
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, lw = 46;   // left: axis labels
    ImVec2 a(p0.x + lw, p0.y), b(p0.x + w, p0.y + height);
    float pw = b.x - a.x;
    ImGui::InvisibleButton("plot", ImVec2(w, height));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(20, 22, 28, 255));
    dl->AddRect(a, b, IM_COL32(70, 70, 80, 255));
    if (n < 2 || pw < 8) {
        dl->AddText(ImVec2(a.x + 6, a.y + 4), IM_COL32(150, 150, 150, 255), n < 2 ? "no data yet" : "");
        ImGui::PopID();
        return;
    }

    int cols = (int)pw;
    if (n < cols) cols = n;
    float maxv = 0;
    // per column values (cols x series)
    static std::vector<float> v;
    v.assign((size_t)cols * ser.size(), 0);
    for (int c = 0; c < cols; c++) {
        int i0 = (int)((int64_t)c * n / cols), i1 = (int)((int64_t)(c + 1) * n / cols);
        if (i1 <= i0) i1 = i0 + 1;
        float sum_stack = 0;
        for (size_t k = 0; k < ser.size(); k++) {
            if (!ser[k].on) continue;
            float acc = 0, mx = 0;
            for (int i = i0; i < i1; i++) {
                float x = ser[k].get(sample(i, ctx), ser[k].arg);
                acc += x;
                mx = x > mx ? x : mx;
            }
            float val = stacked ? acc / (i1 - i0) : mx;
            v[c * ser.size() + k] = val;
            if (stacked) sum_stack += val;
            else maxv = val > maxv ? val : maxv;
        }
        if (stacked) maxv = sum_stack > maxv ? sum_stack : maxv;
    }
    float top = nice_ceil(maxv * 1.05f);
    auto Y = [&](float val) { return b.y - (val / top) * (b.y - a.y); };
    float colw = pw / cols;

    // grid + guides
    char lab[32];
    for (int g = 1; g <= 3; g++) {
        float gv = top * g / 4, y = Y(gv);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(45, 45, 55, 255));
    }
    _snprintf(lab, sizeof lab, "%g %s", top, unit);
    dl->AddText(ImVec2(p0.x, a.y), IM_COL32(160, 160, 160, 255), lab);
    dl->AddText(ImVec2(p0.x, b.y - 14), IM_COL32(160, 160, 160, 255), "0");
    for (int g = 0; g < n_guides; g++) {
        if (guides[g] > top) continue;
        float y = Y(guides[g]);
        for (float x = a.x; x < b.x; x += 8) dl->AddLine(ImVec2(x, y), ImVec2(x + 4 < b.x ? x + 4 : b.x, y), IM_COL32(200, 200, 90, 160));
        _snprintf(lab, sizeof lab, "%g", guides[g]);
        dl->AddText(ImVec2(p0.x, y - 7), IM_COL32(200, 200, 90, 220), lab);
    }

    dl->PushClipRect(a, b, true);
    if (stacked) {
        for (int c = 0; c < cols; c++) {
            float x0 = a.x + c * colw, x1 = x0 + (colw > 1 ? colw : 1), cum = 0;
            for (size_t k = 0; k < ser.size(); k++) {
                if (!ser[k].on) continue;
                float val = v[c * ser.size() + k];
                if (val > 0) dl->AddRectFilled(ImVec2(x0, Y(cum + val)), ImVec2(x1, Y(cum)), ser[k].col);
                cum += val;
            }
        }
    } else {
        static std::vector<ImVec2> pts;
        for (size_t k = 0; k < ser.size(); k++) {
            if (!ser[k].on) continue;
            pts.clear();
            for (int c = 0; c < cols; c++)
                pts.push_back(ImVec2(a.x + (cols > 1 ? c * pw / (cols - 1) : 0), Y(v[c * ser.size() + k])));
            dl->AddPolyline(pts.data(), (int)pts.size(), ser[k].col, 0, 1.5f);
        }
    }
    dl->PopClipRect();

    if (hovered) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= a.x) {
            int i = (int)((mx - a.x) / pw * n);
            i = i < 0 ? 0 : i >= n ? n - 1 : i;
            dl->AddLine(ImVec2(mx, a.y), ImVec2(mx, b.y), IM_COL32(255, 255, 255, 90));
            ImGui::BeginTooltip();
            char hl[96];
            hover_label(i, ctx, hl, sizeof hl);
            ImGui::TextUnformatted(hl);
            float total = 0;
            for (auto &s : ser) {
                if (!s.on) continue;
                float x = s.get(sample(i, ctx), s.arg);
                total += x;
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.col), "%-12s %8.2f %s", s.name, x, unit);
            }
            if (stacked) ImGui::Text("%-12s %8.2f %s", "total", total, unit);
            ImGui::EndTooltip();
        }
    }
    // legend: a checkbox per series (ticked = drawn), the tick in the series colour
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 3));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));
    for (size_t k = 0; k < ser.size(); k++) {
        if (k) ImGui::SameLine(0, 12);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(ser[k].col);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, c);
        ImGui::PushStyleColor(ImGuiCol_Text, ser[k].on ? c : ImVec4(c.x, c.y, c.z, 0.45f));
        ImGui::PushID((int)k);
        ImGui::Checkbox(ser[k].name, &ser[k].on);
        ImGui::PopID();
        ImGui::PopStyleColor(2);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// ------------------------------------------------------------------------------------------------ views
struct FrameView {                                      // a window of frame samples (live ring or a paused copy)
    const FrameSample *ring;
    int ring_size;
    LONG first;                                         // index of the oldest sample shown
    int n;
};
static const void *frame_at(int i, void *ctx)
{
    FrameView *v = (FrameView *)ctx;
    return &v->ring[(v->first + i) % v->ring_size];
}
static void frame_label(int i, void *ctx, char *out, size_t cap)
{
    FrameView *v = (FrameView *)ctx;
    _snprintf(out, cap, "%d frames ago", v->n - 1 - i);
    out[cap - 1] = 0;
}
struct MemView {
    const MemSample *ring;
    LONG first;
    int n;
};
static const void *mem_at(int i, void *ctx)
{
    MemView *v = (MemView *)ctx;
    return &v->ring[(v->first + i) % MEM_RING];
}
static void mem_label(int i, void *ctx, char *out, size_t cap)
{
    MemView *v = (MemView *)ctx;
    const MemSample *last = (const MemSample *)mem_at(v->n - 1, ctx), *s = (const MemSample *)mem_at(i, ctx);
    _snprintf(out, cap, "%.1f s ago", last->t - s->t);
    out[cap - 1] = 0;
}

static float g_wall(const void *s, int) { return ((const FrameSample *)s)->wall; }
static float g_cpu(const void *s, int) { return ((const FrameSample *)s)->cpu; }
static float g_present(const void *s, int) { return ((const FrameSample *)s)->present; }
static float g_draws(const void *s, int) { return (float)((const FrameSample *)s)->draws; }
static float g_game(const void *s, int) { return ((const FrameSample *)s)->game; }
static float g_gpu(const void *s, int) { return ((const FrameSample *)s)->gpu; }
static float g_zone(const void *s, int z) { return ((const FrameSample *)s)->zone[z]; }
static float g_pass(const void *s, int p) { return ((const FrameSample *)s)->pass[p]; }
static float g_counter(const void *s, int c) { return (float)((const FrameSample *)s)->counters[c]; }
static float g_arena_mb(const void *s, int i) { return ((const MemSample *)s)->arena[i]; }
static float g_tracked(const void *s, int) { return ((const MemSample *)s)->tracked; }
static float g_priv(const void *s, int) { return ((const MemSample *)s)->priv; }
static float g_vspace(const void *s, int) { return ((const MemSample *)s)->vspace; }

static ImU32 palette(int i)
{
    static const ImU32 k[] = {IM_COL32(90, 170, 255, 255), IM_COL32(255, 150, 60, 255), IM_COL32(120, 220, 120, 255),
                              IM_COL32(230, 90, 90, 255),  IM_COL32(200, 130, 255, 255), IM_COL32(240, 220, 90, 255),
                              IM_COL32(90, 220, 220, 255), IM_COL32(255, 120, 200, 255), IM_COL32(170, 170, 170, 255),
                              IM_COL32(150, 110, 70, 255), IM_COL32(110, 140, 255, 255), IM_COL32(200, 255, 150, 255)};
    return k[i % (sizeof k / sizeof k[0])];
}

static void bar(float frac, const char *text, float width)
{
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    ImVec4 c = frac > 0.9f ? ImVec4(0.85f, 0.25f, 0.25f, 1) : frac > 0.75f ? ImVec4(0.85f, 0.6f, 0.2f, 1)
                                                                           : ImVec4(0.3f, 0.55f, 0.85f, 1);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
    ImGui::ProgressBar(frac, ImVec2(width, 0), text);
    ImGui::PopStyleColor();
}

static void section_frames(const FrameView &fv)
{
    if (!ImGui::CollapsingHeader("Frame time", ImGuiTreeNodeFlags_DefaultOpen)) return;
    FrameView v = fv;
    double sum = 0, mx = 0;
    int slow = 0;
    static std::vector<float> walls;
    walls.clear();
    for (int i = 0; i < v.n; i++) {
        const FrameSample *s = (const FrameSample *)frame_at(i, &v);
        sum += s->wall, mx = s->wall > mx ? s->wall : mx;
        slow += s->wall > 17.5f;
        walls.push_back(s->wall);
    }
    if (v.n) {
        std::sort(walls.begin(), walls.end());
        float p99 = walls[(size_t)(walls.size() * 0.99f) < walls.size() ? (size_t)(walls.size() * 0.99f) : walls.size() - 1];
        const FrameSample *last = (const FrameSample *)frame_at(v.n - 1, &v);
        ImGui::Text("%.1f fps average, 1%% low %.1f fps | frame %.2f ms average, %.2f ms worst | %d slow (> 17.5 ms) "
                    "of %d | now: cpu %.2f ms, present %.2f ms, %u draws",
                    sum > 0 ? v.n * 1000.0 / sum : 0.0, p99 > 0 ? 1000.0f / p99 : 0.0f, sum / v.n, mx, slow, v.n,
                    last->cpu, last->present, last->draws);
    }
    static std::vector<Series> ser = {{"frame", IM_COL32(235, 235, 235, 255), g_wall, 0},
                                      {"cpu", IM_COL32(255, 150, 60, 255), g_cpu, 0},
                                      {"present", IM_COL32(90, 170, 255, 255), g_present, 0}};
    static const float guides[] = {16.67f, 33.33f};
    plot("frames", ser, v.n, frame_at, &v, 170, false, guides, 2, "ms", frame_label);
    ImGui::TextDisabled("cpu = game + draw submission since the last Present returned; present = time blocked in Present "
                        "(waiting for vsync or the GPU)");
    static std::vector<Series> dser = {{"draws", IM_COL32(120, 220, 120, 255), g_draws, 0}};
    plot("draws", dser, v.n, frame_at, &v, 70, false, nullptr, 0, "", frame_label);
}

static void section_profiler(const FrameView &fv)
{
    if (!ImGui::CollapsingHeader("Renderer breakdown (CPU zones, GPU passes)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool on = g_prof_ini || g_prof_want;
    ImGui::BeginDisabled(g_prof_ini);
    if (ImGui::Checkbox("Renderer profiler", &on)) {
        g_prof_want = on;
        g_host->prof_live(on);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(g_prof_ini ? "(on by [debug] Profile=1 in gladiusport.ini)"
                                   : "(GPU timestamp queries + CPU zone timers; a little overhead while on)");
    FrameView v = fv;
    int first_prof = v.n;                               // only the frames since it came on
    for (int i = v.n - 1; i >= 0 && ((const FrameSample *)frame_at(i, &v))->prof; i--) first_prof = i;
    v.first += first_prof, v.n -= first_prof;
    if (!v.n) {
        if (on) ImGui::TextDisabled("waiting for frames...");
        return;
    }
    int np = g_n_passes, nz = g_n_zones, nc = g_n_counters;

    static std::vector<Series> gser;
    if (gser.empty())
        for (int p = 0; p < np; p++) gser.push_back({g_host->prof_name(1, p), palette(p), g_pass, p});
    static const float guides[] = {16.67f};
    ImGui::TextUnformatted("GPU time per pass (stacked; a few frames behind the CPU; when the CPU is the slow side, GPU "
                           "idle time between command batches lands in the pass that was running)");
    plot("gpu", gser, v.n, frame_at, &v, 150, true, guides, 1, "ms", frame_label);

    // the zones that add up to the frame; the rest nest inside "shim"
    static std::vector<Series> cser;
    if (cser.empty()) {
        cser.push_back({"game", IM_COL32(255, 150, 60, 255), g_game, 0});
        const char *const top[] = {"shim", "present", "netwait", "pace"};
        for (int z = 0, k = 1; z < nz; z++)
            for (const char *t : top)
                if (!strcmp(g_host->prof_name(0, z), t)) cser.push_back({t, palette(k++), g_zone, z});
        cser.push_back({"gpu", IM_COL32(230, 90, 90, 255), g_gpu, 0});
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("CPU per frame: game code, D3D shim (all draw work), Present, netplay waits; GPU total");
    plot("cpu", cser, v.n, frame_at, &v, 150, false, guides, 1, "ms", frame_label);

    if (ImGui::TreeNode("Zone and counter averages over the window")) {
        if (ImGui::BeginTable("zones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("CPU zone");
            ImGui::TableSetupColumn("avg ms");
            ImGui::TableSetupColumn("max ms");
            ImGui::TableHeadersRow();
            for (int z = 0; z < nz; z++) {
                double s = 0, m = 0;
                for (int i = 0; i < v.n; i++) {
                    float x = ((const FrameSample *)frame_at(i, &v))->zone[z];
                    s += x, m = x > m ? x : m;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_host->prof_name(0, z));
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s / v.n);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", m);
            }
            ImGui::EndTable();
        }
        ImGui::SameLine();
        if (ImGui::BeginTable("passes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("GPU pass");
            ImGui::TableSetupColumn("avg ms");
            ImGui::TableSetupColumn("max ms");
            ImGui::TableHeadersRow();
            for (int p = 0; p < np; p++) {
                double s = 0, m = 0;
                for (int i = 0; i < v.n; i++) {
                    float x = ((const FrameSample *)frame_at(i, &v))->pass[p];
                    s += x, m = x > m ? x : m;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_host->prof_name(1, p));
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s / v.n);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", m);
            }
            ImGui::EndTable();
        }
        ImGui::SameLine();
        if (ImGui::BeginTable("counters", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("counter (per frame)");
            ImGui::TableSetupColumn("avg");
            ImGui::TableSetupColumn("max");
            ImGui::TableHeadersRow();
            for (int c = 0; c < nc; c++) {
                double s = 0, m = 0;
                for (int i = 0; i < v.n; i++) {
                    float x = g_counter(frame_at(i, &v), c);
                    s += x, m = x > m ? x : m;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_host->prof_name(2, c));
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", s / v.n);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", m);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("shim includes every zone below it except present; texture includes texupload, lookup "
                            "includes compile, program includes most draw zones");
        ImGui::TreePop();
    }
}

static void section_memory(const MemView &mv)
{
    if (!ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen)) return;
    DevArena arena[ARENAS];
    bool ok[ARENAS];
    std::vector<HeapRow> heaps;
    uint32_t blocks;
    {
        std::lock_guard<std::mutex> l(g_snap_lock);
        memcpy(arena, g_arena, sizeof arena);
        memcpy(ok, g_arena_ok, sizeof ok);
        heaps = g_heaps;
        blocks = g_tracked_blocks;
    }
    MemView v = mv;
    static std::vector<Series> ser;
    if (ser.empty() && v.n) {                           // after the first sample: which arenas exist
        for (int i = 0; i < ARENAS; i++)
            if (ok[i]) ser.push_back({arena[i].name, palette(i), g_arena_mb, i});
        ser.push_back({"tracked", IM_COL32(240, 220, 90, 255), g_tracked, 0});
        ser.push_back({"private", IM_COL32(170, 170, 170, 255), g_priv, 0});
        ser.push_back({"addr space", IM_COL32(255, 120, 200, 255), g_vspace, 0});
        for (auto &s : ser)                             // the big ones hide the small: start with the game's own
            if (!strcmp(s.name, "phys") || !strcmp(s.name, "private") || !strcmp(s.name, "addr space")) s.on = false;
    }
    plot("mem", ser, v.n, mem_at, &v, 170, false, nullptr, 0, "MB", mem_label);
    ImGui::TextDisabled("arenas = memory container in use; tracked = the game's own heap counter; private = whole process "
                        "(renderer, driver, textures); addr space = 32-bit address space used. Tick a name to draw it.");

    if (v.n) {
        const MemSample *last = (const MemSample *)mem_at(v.n - 1, &v);
        ImGui::Text("game tracked heap %.2f MB in %u blocks | process private %.0f MB | address space %.0f MB of 4096",
                    last->tracked, blocks, last->priv, last->vspace);
    }
    bool any = false;
    for (bool o : ok) any |= o;
    if (!any) ImGui::TextDisabled("memory container off: no arena numbers");
    else if (ImGui::BeginTable("arenas", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("arena");
        ImGui::TableSetupColumn("in use", ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableSetupColumn("high-water (address)", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("failed allocs");
        ImGui::TableHeadersRow();
        for (int i = 0; i < ARENAS; i++) {
            if (!ok[i]) continue;
            const DevArena &a = arena[i];
            char t[64];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.name);
            ImGui::TableNextColumn();
            _snprintf(t, sizeof t, "%.2f MB (%.0f%%)", a.used * MB, a.size ? a.used * 100.0 / a.size : 0.0);
            bar(a.size ? (float)a.used / a.size : 0, t, -1);
            ImGui::TableNextColumn();
            _snprintf(t, sizeof t, "%.2f MB (%.0f%%)", a.hwm * MB, a.size ? a.hwm * 100.0 / a.size : 0.0);
            bar(a.size ? (float)a.hwm / a.size : 0, t, -1);
            ImGui::TableNextColumn();
            ImGui::Text("%u MB", a.size >> 20);
            ImGui::TableNextColumn();
            if (a.failed) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%ld", a.failed);
            else ImGui::TextDisabled("0");
        }
        ImGui::EndTable();
    }

    if (ImGui::TreeNodeEx("SmallHeaps", ImGuiTreeNodeFlags_DefaultOpen, "SmallHeaps (%d)", (int)heaps.size())) {
        static char filter[64];
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##hf", "filter", filter, sizeof filter);
        ImGui::SameLine();
        ImGui::TextDisabled("fixed-size engine heaps (max ~128 KB each); a full one overflows into the game heap "
                            "(port fix, port.log \"smallheap:\")");
        if (ImGui::BeginTable("heaps", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable |
                                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, 320))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("in use", ImGuiTableColumnFlags_WidthFixed, 230);
            ImGui::TableSetupColumn("high-water", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                                       ImGuiTableColumnFlags_PreferSortDescending, 230);
            ImGui::TableHeadersRow();
            static int sort_col = 3;
            static bool sort_desc = true;
            if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs())
                if (ss->SpecsCount) sort_col = ss->Specs[0].ColumnIndex, sort_desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            std::sort(heaps.begin(), heaps.end(), [](const HeapRow &x, const HeapRow &y) {
                double a, b;
                switch (sort_col) {
                case 0: return sort_desc ? _stricmp(x.name.c_str(), y.name.c_str()) > 0 : _stricmp(x.name.c_str(), y.name.c_str()) < 0;
                case 1: a = x.len, b = y.len; break;
                case 2: a = (double)x.used / x.len, b = (double)y.used / y.len; break;
                default: a = (double)x.high / x.len, b = (double)y.high / y.len; break;
                }
                return sort_desc ? a > b : a < b;
            });
            for (auto &h : heaps) {
                std::string lower = h.name, f = filter;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                if (!f.empty() && lower.find(f) == std::string::npos) continue;
                char t[64];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(h.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u K", (h.len + 1023) >> 10);
                ImGui::TableNextColumn();
                _snprintf(t, sizeof t, "%u (%.0f%%)", h.used, h.used * 100.0 / h.len);
                bar((float)h.used / h.len, t, -1);
                ImGui::TableNextColumn();
                _snprintf(t, sizeof t, "%u (%.0f%%)", h.high, h.high * 100.0 / h.len);
                bar((float)h.high / h.len, t, -1);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}

// ------------------------------------------------------------------------------------------------ tab
void panel_graphs()
{
    if (!ImGui::BeginTabItem("Graphs")) return;
    static bool paused;
    static int frame_secs = 10, mem_mins = 2;
    static std::vector<FrameSample> frozen_fr;
    static std::vector<MemSample> frozen_mem;
    static LONG frozen_fr_n, frozen_mem_n;
    if (ImGui::Checkbox("Pause", &paused) && paused) {  // freeze copies: the rings keep filling
        frozen_fr.assign(g_fr, g_fr + FRAME_RING);
        frozen_mem.assign(g_mem, g_mem + MEM_RING);
        frozen_fr_n = g_fr_n, frozen_mem_n = g_mem_n;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("frame window (s)", &frame_secs, 1, 60);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("memory window (min)", &mem_mins, 1, 10);
    ImGui::SameLine();
    ImGui::TextDisabled("hover a graph for values");

    LONG fn = paused ? frozen_fr_n : g_fr_n;
    int want = frame_secs * 60;
    int n = fn < want ? (int)fn : want;
    if (n > FRAME_RING - 2) n = FRAME_RING - 2;         // keep clear of the sample being written
    FrameView fv = {paused ? frozen_fr.data() : g_fr, FRAME_RING, (fn - n) % FRAME_RING, n};
    LONG mn = paused ? frozen_mem_n : g_mem_n;
    int mwant = mem_mins * 240;
    int m = mn < mwant ? (int)mn : mwant;
    if (m > MEM_RING - 2) m = MEM_RING - 2;
    MemView mv = {paused ? frozen_mem.data() : g_mem, (mn - m) % MEM_RING, m};

    ImGui::BeginChild("graphs", ImVec2(0, 0));
    section_frames(fv);
    section_profiler(fv);
    section_memory(mv);
    ImGui::EndChild();
    ImGui::EndTabItem();
}
