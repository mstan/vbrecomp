#ifndef V810_BITSTRING_H
#define V810_BITSTRING_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Build-fixed implementation. HLE logical operations complete atomically;
 * LLE retains destination-word resume points and instruction-level scheduling. */
void vb_bitstring_execute(CPUState *cpu, unsigned operation);
const char *vb_bitstring_implementation(void);
typedef struct { uint64_t logical_calls, logical_bits, logical_completed; } VbBitstringStats;
extern VbBitstringStats vb_bitstring_stats;
int vb_bitstring_diagnostics_enabled(void);
#ifdef __cplusplus
}
#endif
#endif
