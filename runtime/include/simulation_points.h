#ifndef VB_SIMULATION_POINTS_H
#define VB_SIMULATION_POINTS_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
// Trusted, statically linked game simulation adapters. Register while disabled;
// enable only for the explicit game variant whose build/profile includes them.
// Callbacks run on the guest thread during live execution AND resimulation.
// They must not sample host input, present, perform I/O, or throw exceptions.
// Return 1 after redirecting PC, or 0 to execute the original instruction.
typedef int (*VbSimulationCallback)(CPUState*,void* context);
typedef struct VbSimulationPoint {
    uint32_t pc;
    uint16_t word;
    VbSimulationCallback callback;
    void* context;
    const struct VbSimulationPoint* next;
} VbSimulationPoint;
int vb_simulation_point_register(uint32_t pc,uint16_t word,VbSimulationCallback,void*);
int vb_simulation_points_enable(int enabled);
void vb_simulation_points_clear(void);
extern int vb_simulation_points_enabled;
extern const VbSimulationPoint* vb_simulation_points[256];
int vb_simulation_point_apply(CPUState*,const VbSimulationPoint*);
static inline int vb_simulation_before(CPUState* cpu) {
    if(vb_simulation_points_enabled) {
        const VbSimulationPoint* point=vb_simulation_points[(cpu->pc>>1)&255];
        for(;point;point=point->next)
            if(point->pc==cpu->pc)return vb_simulation_point_apply(cpu,point);
    }
    return 0;
}
#ifdef __cplusplus
}
#endif
#endif
