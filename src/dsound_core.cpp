// Xbox DirectSound -> software mixer -> WASAPI. Xbox buffers/streams are plain C functions taking pThis, except the
// stream's XMediaObject methods (Process etc.), which the game calls through a vtable.
//
// Sources: buffers (PCM or Xbox ADPCM in game-written memory, play/loop/position) and streams (queued media
// packets, completed when consumed). Each source is decoded, resampled (linear) to the output rate and mixed with
// its volume and mix-bin routing (speaker bins -> stereo). Cursors report the mixer's real position.
const char *port_ini_name();   // port_main.cpp: gladiusport.ini (gladiusport_padN.ini for -gnpad=N if present)
#include "dsound.h"
#include "handles.h"
#include "container.h"
#include <stdarg.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <intrin.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#pragma comment(lib, "ole32.lib")

void dsound_stub_call(DWORD addr, const char *name, const char *fmt, ...)
{
    static std::map<std::string, bool> seen;
    if (seen[name])
        return;
    seen[name] = true;
    char args[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(args, sizeof args - 1, fmt, ap);
    va_end(ap);
    args[sizeof args - 1] = 0;
    port_log("DSOUND stub first call: %s(%s) [xbe %06X]\n", name, args, addr);
}

#pragma pack(push, 1)
struct XWaveFormat {                 // WAVEFORMATEX
    WORD wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct XMixBinPair {                 // DSMIXBINVOLUMEPAIR
    DWORD dwMixBin;
    LONG lVolume;
};
struct XMixBins {                    // DSMIXBINS
    DWORD dwMixBinCount;
    const XMixBinPair *lpMixBinVolumePairs;
};
struct XBufferDesc {                 // Xbox DSBUFFERDESC
    DWORD dwSize, dwFlags, dwBufferBytes;
    XWaveFormat *lpwfxFormat;
    const XMixBins *lpMixBins;
    DWORD dwInputMixBin;
};
struct XStreamDesc {                 // Xbox DSSTREAMDESC
    DWORD dwFlags, dwMaxAttachedPackets;
    XWaveFormat *lpwfxFormat;
    void *lpfnCallback, *lpvContext;
    const XMixBins *lpMixBins;
};
struct XMediaPacket {
    void *pvBuffer;
    DWORD dwMaxSize;
    DWORD *pdwCompletedSize, *pdwStatus;
    HANDLE hCompletionEvent;
    LONGLONG *prtTimestamp;
};
#pragma pack(pop)

static const DWORD DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4, DSBPLAY_LOOPING = 1;
static const DWORD XMEDIAPACKET_STATUS_SUCCESS = 0, XMEDIAPACKET_STATUS_PENDING = 0x8000000A,
                   XMEDIAPACKET_STATUS_FLUSHED = 0x80004004;
static const WORD WAVE_FORMAT_XBOX_ADPCM = 0x69;

// ------------------------------------------------------------------------------------------------ Xbox ADPCM
// IMA ADPCM, 36 bytes per channel per block = 4-byte header (int16 history, uint8 step index) + 32 bytes of
// nibbles = 64 samples; channels interleave in 4-byte groups after the headers.
static const int k_ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int k_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static void adpcm_decode_block(const BYTE *blk, int channels, short out[][64])
{
    for (int c = 0; c < channels; c++) {
        int hist = (short)(blk[c * 4] | blk[c * 4 + 1] << 8), idx = blk[c * 4 + 2];
        if (idx > 88)
            idx = 88;
        const BYTE *data = blk + channels * 4;
        for (int g = 0; g < 8; g++) {                   // 8 groups of 4 bytes (8 samples) per channel
            const BYTE *q = data + (g * channels + c) * 4;
            for (int k = 0; k < 8; k++) {
                int nib = (q[k >> 1] >> ((k & 1) * 4)) & 15, step = k_ima_steps[idx];
                int diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                hist += (nib & 8) ? -diff : diff;
                hist = hist < -32768 ? -32768 : hist > 32767 ? 32767 : hist;
                idx += k_ima_index[nib];
                idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                out[c][g * 8 + k] = (short)hist;
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ sources
struct Packet {
    XMediaPacket p;
    DWORD used;                                          // bytes consumed
};

// Fixed-size packet queue (plain data, no heap): sources are game memory (memory container), and a netplay state
// sync copies them between machines - a std::deque would carry the other PC's heap pointers.
struct PacketRing {
    static const DWORD CAP = 32;
    Packet q[CAP];
    DWORD head, n;
    bool empty() const { return n == 0; }
    DWORD size() const { return n; }
    Packet &front() { return q[head]; }
    void pop_front() { head = (head + 1) % CAP; n--; }
    void push_back(const Packet &pk)
    {
        if (n < CAP) q[(head + n++) % CAP] = pk;
    }
    void clear() { head = n = 0; }
    struct It {
        PacketRing *r;
        DWORD i;
        Packet &operator*() const { return r->q[(r->head + i) % CAP]; }
        It &operator++() { ++i; return *this; }
        bool operator!=(const It &o) const { return i != o.i; }
    };
    It begin() { return {this, 0}; }
    It end() { return {this, n}; }
};

// Sources are game memory: the game holds their addresses, and with the memory container on they (and the list of
// them, below) are synced between netplay machines with everything else. Only the WASAPI output is machine-local.
struct Source {
    GAME_ALLOCATED
    // format
    WORD tag, channels, bits, align;
    DWORD rate;
    // buffer data (buffers) or packet queue (streams)
    bool is_stream;
    BYTE *data;
    DWORD bytes;
    PacketRing packets;
    DWORD max_packets;
    // playback state
    bool playing, looping, paused;
    DWORD loop_start, loop_len;                         // bytes (0 len = whole buffer)
    LONG volume;                                        // hundredths of dB
    DWORD freq;                                         // 0 = format rate
    XMixBinPair bins[8];
    DWORD nbins;
    LONG refs;
    // decoder / resampler
    DWORD frame;                                        // next frame to read (buffers: absolute frame index)
    LONG adpcm_block;                                   // block cached in dec (-1 none)
    short dec[2][64];
    float h[4][2], phase;                               // resampler history x[-1], x0, x1, x2 (interp x0..x1)
    bool primed;
    bool fed;                                           // stream had data last mix (glitch counter: fed -> dry)
};

static CRITICAL_SECTION g_mix;
// the mixer's list of live sources: in the container's shared page when it is on (synced game memory)
struct SourceList {
    DWORD count;
    Source *list[255];
};
static SourceList g_sources_local;
static SourceList &sources()
{
    static SourceList *l;
    if (!l)
        l = container_on() ? (SourceList *)((char *)container_shared_page() + 0x400) : &g_sources_local;
    return *l;
}
static DWORD g_out_rate = 48000;

static void mix_init_lock()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID *) -> BOOL {
        InitializeCriticalSectionAndSpinCount(&g_mix, 2000);
        return TRUE;
    }, nullptr, nullptr);
}
// Netplay state sync freeze: while a sync hashes and copies game memory, game threads may not change sound state
// (submit / flush / play...) - a packet submitted between the host's hashing and copying landed in the copied queue
// while its status word (another page, hashed before) still said "done" on the client -> the game refilled a queued
// buffer -> full-scale noise (2026-10-07). The mixer keeps playing; the syncing thread itself is exempt.
static volatile LONG g_api_freeze;
static volatile DWORD g_freezer_tid, g_mixer_tid;
static void api_gate()
{
    DWORD me = GetCurrentThreadId();
    while (g_api_freeze && me != g_mixer_tid && me != g_freezer_tid)
        Sleep(1);
}
void dsound_sync_freeze(bool on)
{
    if (on) {
        if (InterlockedIncrement(&g_api_freeze) == 1)
            g_freezer_tid = GetCurrentThreadId();
    } else if (InterlockedDecrement(&g_api_freeze) == 0)
        g_freezer_tid = 0;
}
struct MixLock {
    MixLock() { api_gate(); EnterCriticalSection(&g_mix); }
    ~MixLock() { LeaveCriticalSection(&g_mix); }
};

static DWORD frames_per_block(const Source *s) { return s->tag == WAVE_FORMAT_XBOX_ADPCM ? 64 : 1; }
static DWORD bytes_to_frames(const Source *s, DWORD b)
{
    return s->tag == WAVE_FORMAT_XBOX_ADPCM ? b / s->align * 64 : b / (s->align ? s->align : 1);
}
static DWORD frames_to_bytes(const Source *s, DWORD f)
{
    return s->tag == WAVE_FORMAT_XBOX_ADPCM ? f / 64 * s->align : f * s->align;
}

// one frame from contiguous memory at frame index f (block-relative for ADPCM via the cache)
static void read_mem_frame(Source *s, const BYTE *base, DWORD f, LONG block_id, float out[2])
{
    if (s->tag == WAVE_FORMAT_XBOX_ADPCM) {
        if (s->adpcm_block != block_id) {
            adpcm_decode_block(base + (f / 64) * s->align, s->channels, s->dec);
            s->adpcm_block = block_id;
        }
        DWORD i = f % 64;
        out[0] = s->dec[0][i] / 32768.0f;
        out[1] = s->channels > 1 ? s->dec[1][i] / 32768.0f : out[0];
    } else if (s->bits == 8) {
        const BYTE *p = base + f * s->align;
        out[0] = (p[0] - 128) / 128.0f;
        out[1] = s->channels > 1 ? (p[1] - 128) / 128.0f : out[0];
    } else {
        const short *p = (const short *)(base + f * s->align);
        out[0] = p[0] / 32768.0f;
        out[1] = s->channels > 1 ? p[1] / 32768.0f : out[0];
    }
}

// [netplay] session trace of audio state the game can observe (status changes it reads, packet completions).
// The mixer runs on the WASAPI clock, so these land on machine-dependent frames; dtrace is a no-op outside a
// session.
void dtrace(const char *fmt, ...);                      // gn_core.cpp
static void audio_trace_status(const void *src, DWORD status, const char *what)
{
    static std::map<const void *, DWORD> last;          // under MixLock
    auto it = last.find(src);
    if (it != last.end() && it->second == status)
        return;
    last[src] = status;
    dtrace("  audio %s %p status %X", what, src, status);
}

static void complete_packet(Packet &pk, DWORD status, DWORD size)
{
    dtrace("  audio packet %p done %X", pk.p.pvBuffer, status);
    if (pk.p.pdwCompletedSize)
        *pk.p.pdwCompletedSize = size;
    if (pk.p.pdwStatus)
        *pk.p.pdwStatus = status;
    if (pk.p.hCompletionEvent)
        SetEvent(vh_real(pk.p.hCompletionEvent));
}

// next source frame; false = nothing to play (buffer ended / stream starved)
static bool next_frame(Source *s, float out[2])
{
    if (!s->is_stream) {
        if (!s->data || !s->bytes)
            return false;
        DWORD total = bytes_to_frames(s, s->bytes);
        DWORD lstart = bytes_to_frames(s, s->loop_start);
        DWORD lend = s->loop_len ? bytes_to_frames(s, s->loop_start + s->loop_len) : total;
        if (lend > total)
            lend = total;
        if (s->looping && s->frame >= lend)
            s->frame = lstart < lend ? lstart : 0;
        if (s->frame >= total) {                        // one-shot finished: stopped, rewound
            s->playing = false;
            s->frame = 0;
            return false;
        }
        read_mem_frame(s, s->data, s->frame, (LONG)(s->frame / frames_per_block(s)), out);
        s->frame++;
        return true;
    }
    while (!s->packets.empty()) {
        Packet &pk = s->packets.front();
        DWORD frames = bytes_to_frames(s, pk.p.dwMaxSize);
        DWORD f = bytes_to_frames(s, pk.used) + s->frame;   // s->frame = frame within current block
        if (f >= frames) {
            complete_packet(pk, XMEDIAPACKET_STATUS_SUCCESS, pk.p.dwMaxSize);
            s->packets.pop_front();
            s->frame = 0;
            s->adpcm_block = -1;
            continue;
        }
        // block ids must differ per packet: use the packet buffer address mixed with the block
        LONG block_id = (LONG)((DWORD)pk.p.pvBuffer + f / frames_per_block(s));
        read_mem_frame(s, (const BYTE *)pk.p.pvBuffer, f, block_id, out);
        s->frame++;
        if (s->frame >= frames_per_block(s)) {             // advance whole blocks into pk.used
            pk.used += frames_to_bytes(s, frames_per_block(s));
            s->frame = 0;
        }
        return true;
    }
    return false;
}

static float db_gain(LONG mb) { return mb <= -10000 ? 0.0f : powf(10.0f, mb / 2000.0f); }

// per source-channel stereo gains from volume + mix bins (speaker bins only; FX sends / I3DL2 are not rendered)
static void source_gains(const Source *s, float g[2][2])
{
    float v = db_gain(s->volume);
    g[0][0] = g[0][1] = g[1][0] = g[1][1] = 0;
    if (!s->nbins) {
        if (s->channels > 1)
            g[0][0] = g[1][1] = v;
        else
            g[0][0] = g[0][1] = v;
        return;
    }
    for (DWORD i = 0; i < s->nbins; i++) {
        int ch = s->channels > 1 ? (int)(i % s->channels) : 0;
        if (ch > 1)
            continue;
        float b = v * db_gain(s->bins[i].lVolume);
        DWORD bin = s->bins[i].dwMixBin;
        float l = 0, r = 0;
        if (bin == 0 || bin == 4 || (bin >= 6 && bin <= 13 && !(bin & 1)))
            l = b;
        else if (bin == 1 || bin == 5 || (bin >= 6 && bin <= 13 && (bin & 1)))
            r = b;
        else if (bin == 2)
            l = r = b * 0.7071f;
        g[ch][0] = l > g[ch][0] ? l : g[ch][0];
        g[ch][1] = r > g[ch][1] ? r : g[ch][1];
    }
    if (s->channels == 1) {                              // mono: the decoders copy the sample into both slots
        g[1][0] = g[1][1] = 0;                           // (out[1] = out[0]) - gain it once, not twice (+6 dB:
    }                                                    // voice lines over music overloaded the limiter, 2026-10-07)
}

// 4-point, 3rd-order Hermite between y0 and y1
static inline float hermite(float ym1, float y0, float y1, float y2, float t)
{
    float c1 = 0.5f * (y1 - ym1), c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2, c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * t + c2) * t + c1) * t + y0;
}

// Look-ahead peak limiter: gain follows the loudest sample in the next LA frames (attack completes before the
// peak arrives) and recovers slowly - the whole signal is turned down, the waveform itself is never bent.
static const int LA = 96;                                // 2 ms at 48 kHz
static float g_la_buf[LA][2], g_la_target[LA], g_lim_gain = 1.0f;
static int g_la_pos;

static void limit(float *out, UINT frames)
{
    const float ceiling = 0.97f, release = 1.0f - expf(-1.0f / (0.12f * g_out_rate));
    for (UINT i = 0; i < frames; i++) {
        float l = out[i * 2], r = out[i * 2 + 1];
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        g_la_target[g_la_pos] = peak > ceiling ? ceiling / peak : 1.0f;
        float need = 1.0f;
        for (int k = 0; k < LA; k++)
            need = g_la_target[k] < need ? g_la_target[k] : need;
        if (need < g_lim_gain)
            g_lim_gain += (need - g_lim_gain) * 0.25f;   // fast but smooth attack within the look-ahead window
        else
            g_lim_gain += (need - g_lim_gain) * release;
        int o = (g_la_pos + 1) % LA;                     // oldest frame leaves the delay line now
        out[i * 2] = g_la_buf[o][0] * g_lim_gain;
        out[i * 2 + 1] = g_la_buf[o][1] * g_lim_gain;
        g_la_buf[g_la_pos][0] = l, g_la_buf[g_la_pos][1] = r;
        g_la_pos = o;
    }
}

void dsound_dump_streams(const char *tag);
static volatile LONG g_sync_hold;                       // netplay state sync running: the mixer outputs silence
// glitch counters (netplay logs them per turn segment): device buffer ran dry before we refilled it (audible gap),
// mix() calls that took > 5 ms, and mix() calls in which a playing stream had no data (starved)
static volatile LONG g_stat_underrun, g_stat_slowmix, g_stat_starve;
void dsound_audio_stats(uint32_t out[3])
{
    out[0] = (uint32_t)InterlockedExchange(&g_stat_underrun, 0);
    out[1] = (uint32_t)InterlockedExchange(&g_stat_slowmix, 0);
    out[2] = (uint32_t)InterlockedExchange(&g_stat_starve, 0);
}

static void mix(float *out, UINT frames)
{
    memset(out, 0, frames * 2 * sizeof(float));
    if (g_sync_hold)                                     // don't block on g_mix holding a WASAPI buffer (underrun)
        return;
    MixLock lock;
    SourceList &sl = sources();
    struct Probe { Source *s; float raw, gain; UINT read, pinned; };       // loudness probe (netplay "too loud + distorted" hunt)
    Probe probe[64];
    int nprobe = 0;
    for (DWORD si = 0; si < sl.count; si++) {
        Source *s = sl.list[si];
        if (!s->playing || s->paused)
            continue;
        float g[2][2];
        source_gains(s, g);
        float raw = 0;
        UINT nread = 0, npinned = 0;
        float step = (float)(s->freq ? s->freq : s->rate) / g_out_rate;
        for (UINT i = 0; i < frames; i++) {
            if (!s->primed) {                            // history: silence before the first frame, then x0..x2
                float f[2];
                if (!next_frame(s, f))
                    break;
                memset(s->h, 0, sizeof s->h);
                s->h[2][0] = f[0], s->h[2][1] = f[1];
                if (next_frame(s, f))
                    s->h[3][0] = f[0], s->h[3][1] = f[1];
                s->phase = 0;
                s->primed = true;
            }
            s->phase += step;
            bool ended = false;
            while (s->phase >= 1.0f) {
                float f[2] = {0, 0};
                if (next_frame(s, f)) {
                    s->fed = true;
                    float a = fabsf(f[0]) > fabsf(f[1]) ? fabsf(f[0]) : fabsf(f[1]);
                    raw = a > raw ? a : raw;
                    nread++;
                    if (a >= 0.999f) npinned++;
                }
                else if (!s->is_stream)                 // buffer finished: play out the history, then stop
                    ended = true;
                else if (s->fed)                        // stream ran dry (starved, or its line ended)
                    s->fed = false, InterlockedIncrement(&g_stat_starve);
                memmove(s->h[0], s->h[1], sizeof(float) * 6);
                s->h[3][0] = f[0], s->h[3][1] = f[1];   // starved stream: silence, keeps its position
                s->phase -= 1.0f;
            }
            float c0 = hermite(s->h[0][0], s->h[1][0], s->h[2][0], s->h[3][0], s->phase);
            float c1 = hermite(s->h[0][1], s->h[1][1], s->h[2][1], s->h[3][1], s->phase);
            out[i * 2] += c0 * g[0][0] + c1 * g[1][0];
            out[i * 2 + 1] += c0 * g[0][1] + c1 * g[1][1];
            if (ended) {
                s->primed = false;
                break;
            }
        }
        if (npinned > 32) {                             // decoding garbage: what is this stream reading?
            static DWORD lastBad;
            if (GetTickCount() - lastBad > 1000 && s->is_stream && !s->packets.empty()) {
                lastBad = GetTickCount();
                Packet &pk = s->packets.front();
                port_log("audio GARBAGE src %p: %u of %u frames pinned, front packet %p size %u used %u status %u, "
                         "queue %u\n", s, npinned, nread, pk.p.pvBuffer, pk.p.dwMaxSize, pk.used,
                         pk.p.pdwStatus ? *pk.p.pdwStatus : 99, (unsigned)s->packets.size());
                dsound_dump_streams("bad");
            }
        }
        if (nprobe < 64) {
            float gm = 0;
            for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) gm = g[a][b] > gm ? g[a][b] : gm;
            probe[nprobe++] = {s, raw, gm, nread, npinned};
        }
    }
    float peak = 0;
    for (UINT i = 0; i < frames * 2; i++) peak = fabsf(out[i]) > peak ? fabsf(out[i]) : peak;
    static DWORD lastProbe;
    UINT pinnedAll = 0;
    for (int k = 0; k < nprobe; k++) pinnedAll += probe[k].pinned;
    if ((peak > 1.5f || pinnedAll >= 16) && GetTickCount() - lastProbe > 500) {    // real overload / clipped data
        lastProbe = GetTickCount();
        SYSTEMTIME st;
        GetLocalTime(&st);
        port_log("audio LOUD %02d:%02d:%02d.%03d mix peak %.2f (limiter gain %.2f), %u sources:\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, peak, g_lim_gain, sl.count);
        for (int k = 0; k < nprobe; k++)
            if (probe[k].raw > 0.01f)
                port_log("  src %p %s fmt %u/%ub ch %u %u Hz vol %ld bins %u gain %.2f raw peak %.2f -> %.2f, %u of %u "
                         "frames at full scale\n", probe[k].s, probe[k].s->is_stream ? "stream" : "buffer", probe[k].s->tag,
                         probe[k].s->bits, probe[k].s->channels, probe[k].s->rate, probe[k].s->volume, probe[k].s->nbins,
                         probe[k].gain, probe[k].raw, probe[k].raw * probe[k].gain, probe[k].pinned, probe[k].read);
    }
    limit(out, frames);
}

// ------------------------------------------------------------------------------------------------ output
// [debug] RecordAudio=N: also write the first N seconds of the mix to build\audio.wav (float stereo)
static FILE *g_rec;
static DWORD g_rec_left, g_rec_total;

static void record_init()
{
    char ini[MAX_PATH], val[32];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    strcpy(strrchr(ini, 0x5C) + 1, port_ini_name());
    GetPrivateProfileStringA("debug", "RecordAudio", "", val, sizeof val, ini);
    DWORD secs = strtoul(val, nullptr, 10);
    if (!secs)
        return;
    const char *pad = strstr(GetCommandLineA(), "-gnpad=");     // a second instance on this PC: its own file
    int inst = pad ? atoi(pad + 7) : 0;
    if (inst > 0) _snprintf(strrchr(ini, 0x5C) + 1, 32, "audio_pad%d.wav", inst);
    else strcpy(strrchr(ini, 0x5C) + 1, "audio.wav");
    g_rec = fopen(ini, "wb");
    SYSTEMTIME st;
    GetLocalTime(&st);                                  // lines the recording up with gladiusnet.log
    port_log("audio: recording %s from %02d:%02d:%02d.%03d (%u s)\n", strrchr(ini, 0x5C) + 1, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, secs);
    g_rec_left = secs * g_out_rate;
    BYTE hdr[44] = {};
    fwrite(hdr, 1, 44, g_rec);                          // header written when done
}

static void record(const float *p, UINT frames)
{
    if (!g_rec || !g_rec_left)
        return;
    UINT n = frames < g_rec_left ? frames : g_rec_left;
    fwrite(p, 8, n, g_rec);
    g_rec_left -= n;
    g_rec_total += n;
    if (g_rec_left)
        return;
    DWORD data = g_rec_total * 8, riff = 36 + data, fmt_len = 16, rate = g_out_rate, bps = g_out_rate * 8;
    WORD tag = 3, ch = 2, align = 8, bits = 32;
    fseek(g_rec, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_rec); fwrite(&riff, 4, 1, g_rec); fwrite("WAVEfmt ", 1, 8, g_rec);
    fwrite(&fmt_len, 4, 1, g_rec); fwrite(&tag, 2, 1, g_rec); fwrite(&ch, 2, 1, g_rec); fwrite(&rate, 4, 1, g_rec);
    fwrite(&bps, 4, 1, g_rec); fwrite(&align, 2, 1, g_rec); fwrite(&bits, 2, 1, g_rec);
    fwrite("data", 1, 4, g_rec); fwrite(&data, 4, 1, g_rec);
    fclose(g_rec);
    g_rec = nullptr;
    port_log("audio: recording written (%u frames)\n", g_rec_total);
}

static DWORD WINAPI mixer_thread(void *)
{
    g_mixer_tid = GetCurrentThreadId();
    record_init();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient *ac = nullptr;
    IAudioRenderClient *rc = nullptr;
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT32 buf_frames = 0;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                         (void **)&en)) &&
              SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac));
    if (ok) {
        WAVEFORMATEX wf = {};
        wf.wFormatTag = 3;                               // WAVE_FORMAT_IEEE_FLOAT
        wf.nChannels = 2;
        wf.nSamplesPerSec = g_out_rate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = g_out_rate * 8;
        ok = SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, &wf, nullptr)) &&     // 40 ms buffer
             SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetBufferSize(&buf_frames)) &&
             SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), (void **)&rc)) && SUCCEEDED(ac->Start());
    }
    port_log("audio: %s, %u Hz stereo float, buffer %u frames\n", ok ? "WASAPI shared" : "no device - timed silent mixer",
             g_out_rate, buf_frames);
    if (ac) {                                           // what the device really runs at (Windows converts to it)
        WAVEFORMATEX *mf = nullptr;
        REFERENCE_TIME def = 0, mn = 0;
        if (SUCCEEDED(ac->GetMixFormat(&mf)) && mf) {
            ac->GetDevicePeriod(&def, &mn);
            port_log("audio: device mix format %u Hz %u ch %u bit tag %04X, period %.1f ms (min %.1f)\n",
                     mf->nSamplesPerSec, mf->nChannels, mf->wBitsPerSample, mf->wFormatTag, def / 10000.0, mn / 10000.0);
            CoTaskMemFree(mf);
        }
    }
    std::vector<float> scratch(g_out_rate / 100 * 2);
    // -gnmute: a second instance on this PC mixes (and records) as usual but stays silent - two copies of the same
    // sounds ~100 ms apart (the spectator trails the player) sound like crunching
    const bool muted = strstr(GetCommandLineA(), "-gnmute") != nullptr;
    if (muted)
        port_log("audio: muted (-gnmute)\n");
    for (;;) {
        if (ok) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad)))
                continue;
            UINT32 n = buf_frames - pad;
            if (!n)
                continue;
            if (!pad)
                InterlockedIncrement(&g_stat_underrun);
            BYTE *p = nullptr;
            if (FAILED(rc->GetBuffer(n, &p)))
                continue;
            DWORD m0 = GetTickCount();
            mix((float *)p, n);
            if (GetTickCount() - m0 > 5)
                InterlockedIncrement(&g_stat_slowmix);
            record((float *)p, n);
            rc->ReleaseBuffer(n, muted ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        } else {
            Sleep(10);
            mix(scratch.data(), g_out_rate / 100);
            record(scratch.data(), g_out_rate / 100);
        }
    }
}

static void audio_start()
{
    static bool started;
    if (started)
        return;
    started = true;
    mix_init_lock();
    CloseHandle(CreateThread(nullptr, 0, mixer_thread, nullptr, 0, nullptr));
}

// ------------------------------------------------------------------------------------------------ objects
static Source *new_source(const XWaveFormat *f, bool stream)
{
    mix_init_lock();
    Source *s = new Source();
    s->is_stream = stream;
    s->tag = f ? f->wFormatTag : 1;
    s->channels = f && f->nChannels ? f->nChannels : 2;
    s->bits = f ? f->wBitsPerSample : 16;
    s->rate = f && f->nSamplesPerSec ? f->nSamplesPerSec : 44100;
    s->align = f && f->nBlockAlign ? f->nBlockAlign : (WORD)(s->channels * 2);
    s->adpcm_block = -1;
    s->refs = 1;
    MixLock lock;
    SourceList &sl = sources();
    if (sl.count < sizeof sl.list / sizeof sl.list[0])
        sl.list[sl.count++] = s;
    else
        port_log("dsound: more than %u sources - this one stays silent\n", (unsigned)(sizeof sl.list / sizeof sl.list[0]));
    return s;
}

static void set_bins(Source *s, const XMixBins *mb)
{
    if (!mb || !mb->lpMixBinVolumePairs)
        return;
    MixLock lock;
    s->nbins = mb->dwMixBinCount < 8 ? mb->dwMixBinCount : 8;
    memcpy(s->bins, mb->lpMixBinVolumePairs, s->nbins * sizeof(XMixBinPair));
}

static void set_bin_volumes(Source *s, const XMixBins *mb)
{
    if (!mb || !mb->lpMixBinVolumePairs)
        return;
    MixLock lock;
    for (DWORD i = 0; i < mb->dwMixBinCount; i++)
        for (DWORD k = 0; k < s->nbins; k++)
            if (s->bins[k].dwMixBin == mb->lpMixBinVolumePairs[i].dwMixBin)
                s->bins[k].lVolume = mb->lpMixBinVolumePairs[i].lVolume;
}

static void destroy_source(Source *s)
{
    MixLock lock;
    for (Packet &pk : s->packets)
        complete_packet(pk, XMEDIAPACKET_STATUS_FLUSHED, 0);
    s->packets.clear();
    SourceList &sl = sources();
    for (DWORD i = 0; i < sl.count; i++)
        if (sl.list[i] == s) {
            sl.list[i] = sl.list[--sl.count];           // order does not matter to the mixer
            break;
        }
    if (s->data)
        game_free(s->data);
    delete s;
}

static DWORD g_fake_dsound[16];

// --- stream object: vtable for XMediaObject (AddRef, Release, GetInfo, GetStatus, Process, Discontinuity, Flush)
struct StreamVtbl {
    void *fn[7];
};
struct Stream {
    GAME_ALLOCATED
    StreamVtbl *vtbl;
    Source *src;
};

static ULONG __stdcall st_AddRef(Stream *s) { return InterlockedIncrement(&s->src->refs); }
static ULONG __stdcall st_Release(Stream *s)
{
    LONG r = InterlockedDecrement(&s->src->refs);
    if (r <= 0) {
        destroy_source(s->src);
        delete s;
        return 0;
    }
    return r;
}
static HRESULT __stdcall st_GetInfo(Stream *s, DWORD *info)
{
    if (info) {                                         // XMEDIAINFO: flags, input size, output size, lookahead
        memset(info, 0, 16);
        info[1] = s->src->align;
    }
    return S_OK;
}
static HRESULT __stdcall st_GetStatus(Stream *s, DWORD *status)
{
    MixLock lock;
    if (status) {
        *status = s->src->packets.size() < s->src->max_packets ? 1 : 0;   // XMO_STATUSF_ACCEPT_INPUT_DATA
        audio_trace_status(s->src, *status, "stream");
    }
    return S_OK;
}
static HRESULT __stdcall st_Process(Stream *s, XMediaPacket *in, XMediaPacket *out)
{
    api_gate();                                         // writes the status word before taking the mix lock
    if (!in)
        return S_OK;
    if (in->pdwCompletedSize)
        *in->pdwCompletedSize = 0;
    if (in->pdwStatus)
        *in->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
    if (in->hCompletionEvent)
        ResetEvent(vh_real(in->hCompletionEvent));
    MixLock lock;
    s->src->packets.push_back({*in, 0});
    s->src->playing = true;
    return S_OK;
}
static HRESULT __stdcall st_Discontinuity(Stream *s) { return S_OK; }
bool gn_is_main_thread();                               // gn_core.cpp

// netplay trace: who flushes a stream (game return addresses above this call)
static void trace_flush(const Stream *s, const void *sp0)
{
    const DWORD *sp = (const DWORD *)sp0;
    char st[128];
    int o = 0, n = 0;
    for (int i = 0; i < 96 && n < 8 && o < 110; i++)
        if (sp[i] >= 0x12000 && sp[i] < 0x2F0000) o += _snprintf(st + o, sizeof st - o, " %06X", sp[i]), n++;
    st[o < (int)sizeof st ? o : (int)sizeof st - 1] = 0;
    dtrace("  audio flush %p (%u packets) %s tid %u <-%s", s->src, (unsigned)s->src->packets.size(),
           gn_is_main_thread() ? "main" : "OTHER", GetCurrentThreadId(), st);
}

static HRESULT __stdcall st_Flush(Stream *s)
{
    trace_flush(s, _AddressOfReturnAddress());
    MixLock lock;
    for (Packet &pk : s->src->packets)
        complete_packet(pk, XMEDIAPACKET_STATUS_FLUSHED, 0);
    s->src->packets.clear();
    s->src->frame = 0;
    s->src->adpcm_block = -1;
    s->src->primed = false;
    return S_OK;
}
static StreamVtbl g_stream_vtbl = {{(void *)st_AddRef, (void *)st_Release, (void *)st_GetInfo, (void *)st_GetStatus,
                                    (void *)st_Process, (void *)st_Discontinuity, (void *)st_Flush}};

static Source *new_buffer(const XBufferDesc *d)
{
    Source *b = new_source(d ? d->lpwfxFormat : nullptr, false);
    b->bytes = d ? d->dwBufferBytes : 0;
    b->data = b->bytes ? (BYTE *)game_alloc(b->bytes) : nullptr;     // the game writes it (Lock)
    if (d)
        set_bins(b, d->lpMixBins);
    return b;
}

extern "C" {

HRESULT __stdcall dsound_DirectSoundCreate(void *guid, void **out, void *unused)
{
    audio_start();
    *out = g_fake_dsound;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSound_CreateSoundBuffer(void *ds, const XBufferDesc *d, Source **out, void *unused)
{
    audio_start();
    *out = new_buffer(d);
    return S_OK;
}
HRESULT __stdcall dsound_DirectSoundCreateBuffer(const XBufferDesc *d, Source **out)
{
    audio_start();
    *out = new_buffer(d);
    return S_OK;
}
HRESULT __stdcall dsound_DirectSoundCreateStream(const XStreamDesc *d, Stream **out)
{
    audio_start();
    Stream *s = new Stream();
    s->vtbl = &g_stream_vtbl;
    s->src = new_source(d ? d->lpwfxFormat : nullptr, true);
    s->src->max_packets = d && d->dwMaxAttachedPackets ? d->dwMaxAttachedPackets : 4;
    if (d)
        set_bins(s->src, d->lpMixBins);
    *out = s;
    return S_OK;
}

// --- buffers
HRESULT __stdcall dsound_IDirectSoundBuffer_Lock(Source *b, DWORD off, DWORD n, void **p1, DWORD *n1, void **p2,
                                                 DWORD *n2, DWORD flags)
{
    if (!b->data) {
        *p1 = nullptr, *n1 = 0;
        if (p2) *p2 = nullptr;
        if (n2) *n2 = 0;
        return S_OK;
    }
    if (flags & 2)                                      // DSBLOCK_ENTIREBUFFER
        off = 0, n = b->bytes;
    off %= b->bytes;
    DWORD first = n <= b->bytes - off ? n : b->bytes - off;
    *p1 = b->data + off;
    *n1 = first;
    if (p2) *p2 = first < n ? b->data : nullptr;
    if (n2) *n2 = first < n ? n - first : 0;
    MixLock lock;                                       // ADPCM block cache may cover rewritten memory
    b->adpcm_block = -1;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_Unlock(Source *b, void *p1, DWORD n1, void *p2, DWORD n2)
{
    MixLock lock;
    b->adpcm_block = -1;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_Play(Source *b, DWORD r1, DWORD r2, DWORD flags)
{
    MixLock lock;
    if (!b->playing)
        b->primed = false;
    b->playing = true;
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_Stop(Source *b)
{
    MixLock lock;
    b->playing = false;
    b->primed = false;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_StopEx(Source *b, DWORD t_lo, DWORD t_hi, DWORD flags)
{
    MixLock lock;
    if (flags & 2)                                      // DSBSTOPEX_RELEASEWAVEFORM: let a looping sound end
        b->looping = false;
    else
        b->playing = false, b->primed = false;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_GetStatus(Source *b, DWORD *status)
{
    MixLock lock;
    *status = b->playing ? (DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0)) : 0;
    audio_trace_status(b, *status, "buffer");
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_GetCurrentPosition(Source *b, DWORD *play, DWORD *write)
{
    MixLock lock;
    DWORD pos = b->bytes ? frames_to_bytes(b, b->frame) % b->bytes : 0;
    if (play) *play = pos;
    if (write) *write = b->bytes ? (pos + b->align) % b->bytes : 0;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetCurrentPosition(Source *b, DWORD pos)
{
    MixLock lock;
    b->frame = bytes_to_frames(b, pos);
    b->primed = false;
    return S_OK;
}
ULONG __stdcall dsound_IDirectSoundBuffer_Release(Source *b)
{
    LONG r = InterlockedDecrement(&b->refs);
    if (r <= 0) {
        destroy_source(b);
        return 0;
    }
    return r;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetVolume(Source *b, LONG v)
{
    MixLock lock;
    b->volume = v;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetFrequency(Source *b, DWORD f)
{
    MixLock lock;
    b->freq = f;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetMixBins(Source *b, const XMixBins *mb)
{
    set_bins(b, mb);
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetMixBinVolumes_8(Source *b, const XMixBins *mb)
{
    set_bin_volumes(b, mb);
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetLoopRegion(Source *b, DWORD start, DWORD len)
{
    MixLock lock;
    b->loop_start = start;
    b->loop_len = len;
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundBuffer_SetHeadroom(Source *b, DWORD h) { return S_OK; }
HRESULT __stdcall dsound_IDirectSoundBuffer_SetFilter(Source *b, void *f) { return S_OK; }

// --- streams (C entry points; pThis = the stream object)
HRESULT __stdcall dsound_IDirectSoundStream_SetVolume(Stream *s, LONG v) { return dsound_IDirectSoundBuffer_SetVolume(s->src, v); }
HRESULT __stdcall dsound_IDirectSoundStream_SetFrequency(Stream *s, DWORD f) { return dsound_IDirectSoundBuffer_SetFrequency(s->src, f); }
HRESULT __stdcall dsound_IDirectSoundStream_SetMixBins(Stream *s, const XMixBins *mb)
{
    set_bins(s->src, mb);
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundStream_SetMixBinVolumes_8(Stream *s, const XMixBins *mb)
{
    set_bin_volumes(s->src, mb);
    return S_OK;
}
HRESULT __stdcall dsound_IDirectSoundStream_SetFilter(Stream *s, void *f) { return S_OK; }
HRESULT __stdcall dsound_IDirectSoundStream_Pause(Stream *s, DWORD flags)
{
    MixLock lock;
    s->src->paused = (flags & 1) != 0;                  // DSSTREAMPAUSE_PAUSE
    return S_OK;
}

void __stdcall dsound_DirectSoundDoWork() {}            // the mixer runs on its own thread

}

// netplay state sync (gladiusnet.dll): no mixing while game memory is hashed or replaced
void dsound_sync_pause(bool on)
{
    mix_init_lock();
    if (on) {
        InterlockedIncrement(&g_sync_hold);
        EnterCriticalSection(&g_mix);
    } else {
        LeaveCriticalSection(&g_mix);
        InterlockedDecrement(&g_sync_hold);
    }
}

// netplay audio hunt: every stream's packet queue (buffer, size, bytes played, status word) - logged around a
// client's state-sync apply to see what the host's copy disagrees with
void dsound_dump_streams(const char *tag)
{
    mix_init_lock();
    MixLock lock;
    SourceList &sl = sources();
    for (DWORD i = 0; i < sl.count; i++) {
        Source *s = sl.list[i];
        if (!s->is_stream || !s->playing) continue;
        char line[1024];
        int n = _snprintf(line, sizeof line, "  %s src %p frame %u blk %ld q%u:", tag, s, s->frame, s->adpcm_block,
                          (unsigned)s->packets.size());
        for (Packet &pk : s->packets) {
            if (n > (int)sizeof line - 64) break;
            n += _snprintf(line + n, sizeof line - n, " [%p %u used %u st %u @%p]", pk.p.pvBuffer, pk.p.dwMaxSize, pk.used,
                           pk.p.pdwStatus ? *pk.p.pdwStatus : 99, pk.p.pdwStatus);
        }
        port_log("%s\n", line);
    }
}

// netplay state sync: the game memory the mixer writes while it plays (sources, completion words of queued packets).
// A client marks these pages changed so the host always resends them - the mixer can keep playing through a sync.
// (addr, size) pairs into `pairs` (room for `max`); returns the total (call with max 0 to size the buffer)
int dsound_sync_touched_n(uint32_t *pairs, int max)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    mix_init_lock();
    MixLock lock;
    SourceList &sl = sources();
    for (DWORD i = 0; i < sl.count; i++) {
        Source *s = sl.list[i];
        out.push_back({(uint32_t)(uintptr_t)s, (uint32_t)sizeof(Source)});
        for (Packet &pk : s->packets) {
            if (pk.p.pdwStatus) out.push_back({(uint32_t)(uintptr_t)pk.p.pdwStatus, 4u});
            if (pk.p.pdwCompletedSize) out.push_back({(uint32_t)(uintptr_t)pk.p.pdwCompletedSize, 4u});
        }
    }
    for (int i = 0; i < (int)out.size() && i < max; i++)
        pairs[2 * i] = out[i].first, pairs[2 * i + 1] = out[i].second;
    return (int)out.size();
}
