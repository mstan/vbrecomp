#include "machine.h"
#include "memory.h"
#include "interrupts.h"
#include "stub_abort.h"
#include "game_host.h"

int vb_machine_step(CPUState* cpu, uint32_t* dispatch_pc, uint64_t frame_end) {
    /* The debugger may have changed architectural PC while paused. Direct
     * replay must honor the same edit as the normal host loop. */
    *dispatch_pc=cpu->pc;
    if (vb_irq_check_and_deliver(cpu)) *dispatch_pc=cpu->pc;
    if (cpu->halted) {
        while(cpu->cycles<frame_end) {
            int32_t step=vb_devices_cycles_to_next_event(cpu->cycles);
            if(step<1)step=1;
            if((uint64_t)step>frame_end-cpu->cycles)step=(int32_t)(frame_end-cpu->cycles);
            cpu->cycles+=(uint64_t)step;vb_devices_sync(cpu->cycles);
            int level=vb_irq_highest_pending_level();
            if(level>=0 && !cpu->psw_np && !cpu->psw_ep && !cpu->psw_id
               && level>=(int)cpu->psw_int_level)break;
        }
        return 0;
    }
    int32_t next=vb_devices_cycles_to_next_event(cpu->cycles);
    if(next<1)next=1;
    cpu->cycle_deadline=cpu->cycles+(uint64_t)next;
    if(cpu->cycle_deadline>frame_end)cpu->cycle_deadline=frame_end;
    cpu->step_budget=250000;cpu->yielded=0;
    vb_dispatch(cpu,*dispatch_pc);vb_devices_sync(cpu->cycles);
    if(!cpu->yielded && !cpu->halted)
        vb_stub_abort("dispatcher returned without yielding or halting",cpu->pc,0);
    *dispatch_pc=cpu->pc;
    return 1;
}
int vb_machine_run_frame(CPUState* cpu, uint32_t* dispatch_pc, uint64_t* frame_cycles) {
    const uint64_t end=*frame_cycles+397824u;
    unsigned redirects=0;
    while(cpu->cycles<end) {
        uint64_t before=cpu->cycles;uint32_t previous_pc=cpu->pc;
        vb_machine_step(cpu,dispatch_pc,end);
        if(cpu->cycles==before) {
            // A simulation point may redirect before charging an instruction.
            // A debugger stop retains PC. Bound malformed redirect cycles.
            if(cpu->pc==previous_pc || ++redirects>64)return 0;
        } else redirects=0;
    }
    *frame_cycles=end;vb_devices_end_frame(cpu->cycles);++cpu->frame;
    vb_game_host_end_field(cpu);
    return 1;
}
