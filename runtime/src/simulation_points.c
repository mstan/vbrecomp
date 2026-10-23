#include "simulation_points.h"
#include "stub_abort.h"
#include <string.h>

int vb_simulation_points_enabled;
const VbSimulationPoint* vb_simulation_points[256];
static VbSimulationPoint points[32];
static unsigned count;
static int applying;
int vb_simulation_point_register(uint32_t pc,uint16_t word,VbSimulationCallback callback,void* context) {
    const unsigned bucket=(pc>>1)&255;
    const VbSimulationPoint* previous;
    if(vb_simulation_points_enabled || applying || (pc&1)
       || (pc&0x07000000u)!=0x07000000u || !callback || count==32)return 0;
    for(previous=vb_simulation_points[bucket];previous;previous=previous->next)
        if(previous->pc==pc)return 0;
    points[count].pc=pc;points[count].word=word;points[count].callback=callback;
    points[count].context=context;points[count].next=vb_simulation_points[bucket];
    vb_simulation_points[bucket]=&points[count++];return 1;
}
int vb_simulation_points_enable(int enabled) {
    if(applying || (enabled && !count))return 0;
    vb_simulation_points_enabled=enabled!=0;return 1;
}
void vb_simulation_points_clear(void) {
    if(applying)return;
    vb_simulation_points_enabled=0;count=0;
    memset(points,0,sizeof(points));memset(vb_simulation_points,0,sizeof(vb_simulation_points));
}
int vb_simulation_point_apply(CPUState* cpu,const VbSimulationPoint* point) {
    uint32_t before=cpu->pc;int redirected;
    if(applying)return 0; // bounded ROM helpers cannot recursively re-enter the adapter
    if(!cpu->read16 || cpu->read16(before)!=point->word)
        vb_stub_abort("game simulation opcode mismatch",before,point->word);
    applying=1;redirected=point->callback(cpu,point->context);applying=0;
    if((redirected!=0 && redirected!=1) || ((cpu->pc!=before)!=(redirected!=0)) || (cpu->pc&1))
        vb_stub_abort("invalid game simulation redirect",before,cpu->pc);
    if(redirected)cpu->yielded=1;
    return redirected;
}
