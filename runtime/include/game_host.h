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
} VbGameHost;
int vb_game_host_register(const VbGameHost*);
int vb_game_host_option(int argc,char** argv,int* index);
const char* vb_game_host_error(void);
int vb_game_host_initialize(CPUState*);
void vb_game_host_seal_inputs(const VbGameInputs*);
void vb_game_host_end_field(CPUState*);
int vb_game_host_can_pause(void);
int vb_game_host_debug(const char*,long long,char*,size_t);
#ifdef __cplusplus
}
#endif
#endif
