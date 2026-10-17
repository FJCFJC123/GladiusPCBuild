// Engine shader programs on the D3D11 side: LoadVertexShaderProgram puts a tagged program at a program-memory address,
// SelectVertexShaderDirect picks one with a vertex attribute format, SetPixelShaderProgram picks a tagged pixel shader.
#include "d3d.h"
#include "container.h"
#include <map>

// Program memory (address -> source hash). In game memory (shared page +0x800, 136 NV2A slots) when the container is
// on: after a state sync the engine assumes the host's programs are loaded, so this table must be the host's too.
static const DWORD PROG_SLOTS = 136;
static DWORD g_prog_local[PROG_SLOTS];
static std::map<DWORD, DWORD> g_prog_extra;            // addresses past the NV2A range (not expected)
static DWORD &prog_slot(DWORD address)
{
    char *page = (char *)container_shared_page();
    DWORD *t = page ? (DWORD *)(page + 0x800) : g_prog_local;
    return address < PROG_SLOTS ? t[address] : g_prog_extra[address];
}
DWORD g_cur_vs = 0xFFFFFFFF, g_cur_ps = 0xFFFFFFFF;
const BYTE *g_cur_vaf;

static void dump_vaf(const BYTE *vaf)
{
    static std::map<const BYTE *, bool> seen;
    if (seen[vaf])
        return;
    seen[vaf] = true;
    port_log("VAF %p:", vaf);
    for (int s = 0; s < 16; s++) {
        const DWORD *slot = (const DWORD *)(vaf + s * 16);
        if ((slot[2] & 0xFF) != 0x02)
            port_log(" v%d{stream %u off %u fmt %02X}", s, slot[0], slot[1], slot[2]);
    }
    port_log("\n");
}

extern "C" {

void __stdcall d3d_D3DDevice_LoadVertexShaderProgram(const BYTE *microcode, DWORD address)
{
    D3DLock lock;
    d3d_drain();
    DWORD hash = xg_tag_vs_hash(microcode);
    d3d_trace("LoadVertexShaderProgram(%p, addr %u) -> shader %08X", microcode, address, hash);
    if (hash) {
        DWORD &slot = prog_slot(address);
        if (slot != hash) {
            static int logged;
            if (logged++ < 50)
                port_log("vertex program at %u = shader %08X (local #%u)\n", address, hash, xg_hash_id(hash));
        }
        slot = hash;
    } else {
        port_log("LoadVertexShaderProgram: untagged program %p (header %08X)\n", microcode, microcode ? *(DWORD *)microcode : 0);
    }
}

void __stdcall d3d_D3DDevice_SelectVertexShaderDirect(const BYTE *vaf, DWORD address)
{
    D3DLock lock;
    d3d_drain();
    DWORD hash = prog_slot(address);
    g_cur_vs = hash ? xg_hash_id(hash) : 0xFFFFFFFF;
    g_cur_vaf = vaf;
    d3d_trace("SelectVertexShaderDirect(vaf %p, addr %u) -> shader %d", vaf, address, (int)g_cur_vs);
    if (vaf)
        dump_vaf(vaf);
}

void __stdcall d3d_D3DDevice_SetPixelShaderProgram(const DWORD *psdef)
{
    D3DLock lock;
    d3d_drain();
    g_cur_ps = xg_tag_ps(psdef);
    d3d_trace("SetPixelShaderProgram(%p) -> shader %d", psdef, (int)g_cur_ps);
}

}
