// gladiusnet.dll interface (online versus, shipped prebuilt). gladiusport.dll loads it at startup when [net]
// Netplay=1 (gn_hooks.cpp) and calls it through this table; without it the game runs offline. gladiusnet.dll in
// turn links against gladiusport.dll's exports (gp_api.h).
#pragma once
#include "gn.h"

#define GLADIUSNET_ABI 2                 // bump on any change to GnNetApi or to gp_api.h

struct GnNetApi {
    uint32_t abi;                        // GLADIUSNET_ABI
    void (*install)();                   // startup, before the game thread runs: code hooks (AI Simulator)
    void (*init)(GnCtx *c);              // first frame, game thread
    void (*remote_log)(const char *line);    // client: forward a diagnostic line to the host
    // Per frame after the pads are sampled: bot input, lobby screens, hand-off syncs, save pick, lockstep exchange.
    // true = a lobby, browser or session is up.
    bool (*frame)(GnCtx *c, int stateId, int sub, bool harness);
    // After the harness and the save screen: AI-vs-AI timer, session trace, lobby pad blanking.
    void (*after)(GnCtx *c, int stateId, int sub);
    bool (*running)();                   // lockstep session active
};

typedef const GnNetApi *(__cdecl *GladiusNetEntry)(uint32_t abi);   // export "gladiusnet_api"

const GnNetApi *gn_net();                // gn_hooks.cpp: the loaded table, NULL = offline
inline bool gn_net_running() { const GnNetApi *n = gn_net(); return n && n->running(); }
