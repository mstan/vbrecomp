/* vb_runtime.h — convenience macros used by recompiled C and the runtime. */
#ifndef VB_RUNTIME_H
#define VB_RUNTIME_H

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void vb_call_by_address(CPUState* cpu, uint32_t addr) {
    vb_dispatch_call(cpu, addr, cpu->gpr[31]);
}

#ifdef __cplusplus
}
#endif

#endif /* VB_RUNTIME_H */
