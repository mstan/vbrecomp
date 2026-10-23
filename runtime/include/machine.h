#ifndef VB_MACHINE_H
#define VB_MACHINE_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One dispatch/idle quantum, bounded by the next device/frame deadline.
 * Every caller (interactive, headless and rollback) uses this execution path. */
int vb_machine_step(CPUState* cpu, uint32_t* dispatch_pc, uint64_t frame_end);
int vb_machine_run_frame(CPUState* cpu, uint32_t* dispatch_pc, uint64_t* frame_cycles);
#ifdef __cplusplus
}
#endif
#endif
