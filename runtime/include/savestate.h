#ifndef VB_SAVESTATE_H
#define VB_SAVESTATE_H
#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Build-specific, pointer-free execution snapshots. Call only on the guest
 * thread between dispatches. Audio exclusion surrounds all state mutation. */
typedef void (*VbStateVisit)(void* context, void* bytes, size_t size);
/* Trusted plugins may register pointer-free POD presentation residue before
 * the host binds. Version the ID when its layout/meaning changes. Immutable
 * configuration, assets and host pointers do not belong in this span. */
int vb_state_register_presentation(const char* id, void* bytes, size_t size);
void vb_memory_state_visit(VbStateVisit visit, void* context);
void vb_vip_state_visit(VbStateVisit visit, void* context);
void vb_vip_presentation_visit(VbStateVisit visit, void* context);
void vb_vsu_state_visit(VbStateVisit visit, void* context);
void vb_timer_state_visit(VbStateVisit visit, void* context);
void vb_input_state_visit(VbStateVisit visit, void* context);
void vb_irq_state_visit(VbStateVisit visit, void* context);
void vb_viewport_state_visit(VbStateVisit visit, void* context);
int vb_vip_state_valid(void);
int vb_vsu_state_valid(void);
int vb_timer_state_valid(void);
int vb_input_state_valid(void);
int vb_memory_state_valid(void);
int vb_irq_state_valid(void);
void vb_vsu_state_loaded(void);
void vb_vsu_discard_playback(void);
void vb_vsu_shadow_state_visit(VbStateVisit visit, void* context);
int vb_vsu_shadow_state_valid(void);
void vb_vip_state_loaded(void);
void vb_state_bind(CPUState* cpu, uint32_t* dispatch_pc, uint64_t* frame_cycles,
                   void (*audio_lock)(void*), void (*audio_unlock)(void*), void* audio);
/* include_presentation=0 is the canonical simulation digest, never a loadable
 * state. Files and rewind preserve captured per-eye presentation history. */
int vb_state_capture(uint8_t** data, size_t* size, int include_presentation);
int vb_state_restore(const uint8_t* data, size_t size);
int vb_state_save_file(const char* path);
int vb_state_load_file(const char* path);
int vb_state_reset(void); /* reset execution, preserving live cartridge SRAM */
int vb_state_replay_frames(unsigned frames);
void vb_state_shutdown(void);
uint32_t vb_state_digest(void);
uint64_t vb_state_generation(void);
const char* vb_state_error(void);
const char* vb_state_build_id(void);
const uint8_t* vb_state_content_id(void);
/* Local history uses rbengine's owned, tick-keyed snapshot ring. */
int vb_rewind_enable(unsigned depth);
void vb_rewind_shutdown(void);
int vb_rewind_record(uint32_t tick);
int vb_rewind_to(uint32_t tick);
unsigned vb_rewind_count(void);
#ifdef __cplusplus
}
#endif
#endif
