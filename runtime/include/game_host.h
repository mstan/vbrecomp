#ifndef VB_GAME_HOST_H
#define VB_GAME_HOST_H
#include "cpu_state.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VB_GAME_HOST_SEATS 8
typedef struct VbGameInputs {
    uint16_t buttons[VB_GAME_HOST_SEATS];
    uint16_t commands[VB_GAME_HOST_SEATS];
} VbGameInputs;
// Optional trusted game adapter. Core startup has verified the cartridge before
// initialize runs, and canonical spans must be registered there, before bind.
// end_field is the same deterministic boundary in interactive/headless/replay.
// It receives sealed inputs, never reads physical devices or a wall clock.
typedef struct VbGameHost {
    const char* id;
    void* context;
    int (*option)(int argc,char** argv,int* index,void* context); // 0 unknown, 1 accepted, -1 invalid
    const char* (*error)(void* context);
    int (*initialize)(CPUState*,void* context);
    void (*end_field)(CPUState*,const VbGameInputs*,void* context);
    int (*can_pause)(void* context); // also gates the interactive state browsers
    int (*debug)(const char* command,long long request_id,char* out,size_t capacity,void* context);
    // Read-only presentation of a frozen session. seat=-1 is the local canvas;
    // nonnegative seats request independent native views for diagnostics/online.
    int (*video_size)(int seat,int* width,int* height,void* context);
    int (*video_render)(int seat,int eye,uint32_t* out,int width,int height,void* context);
    unsigned netplay_players; // 0 unsupported; game-owned deterministic session ceiling
} VbGameHost;
int vb_game_host_register(const VbGameHost*);
int vb_game_host_option(int argc,char** argv,int* index);
const char* vb_game_host_error(void);
int vb_game_host_initialize(CPUState*);
void vb_game_host_seal_inputs(const VbGameInputs*);
// Physical sampling is presentation-side; explicit debug/network bundles take
// priority until cleared. No physical device reads occur during a replay tick.
void vb_game_host_physical_inputs(const VbGameInputs*);
void vb_game_host_end_field(CPUState*);
int vb_game_host_can_pause(void);
unsigned vb_game_host_netplay_players(void);
int vb_game_host_debug(const char*,long long,char*,size_t);
int vb_game_host_video_size(int seat,int* width,int* height);
int vb_game_host_video_render(int seat,int eye,uint32_t*,int width,int height);
#ifdef __cplusplus
}
#endif
#endif
