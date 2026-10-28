#ifndef VB_MACHINE_H
#define VB_MACHINE_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* One dispatch/idle quantum, bounded by the next device/frame deadline.
 * Every caller (interactive, headless and rollback) uses this execution path. */
int vb_machine_step(CPUState* cpu, uint32_t* dispatch_pc, uint64_t frame_end);
/* Service several exact device quanta before returning to the desktop host.
 * service_cycles is a host-service bound, never a new guest device deadline.
 * HALT, a debugger stop or a zero-cycle redirect returns immediately. Zero
 * retains one-step behavior. Return has the same active/idle meaning as step.
 */
int vb_machine_run_slice(CPUState* cpu, uint32_t* dispatch_pc,
                         uint64_t frame_end, uint32_t service_cycles);
int vb_machine_run_frame(CPUState* cpu, uint32_t* dispatch_pc, uint64_t* frame_cycles);
#ifdef __cplusplus
}
#endif
#endif
