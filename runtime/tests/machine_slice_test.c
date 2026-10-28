/* Compare the real one-step and sliced scheduler with a deterministic CPU and
 * device boundary stream. These tests catch accidental new guest deadlines,
 * debugger overrun, redirect spinning, HALT handling and service-bound overflow.
 * Full built-game CPU/device/pixel/PCM qualification remains a separate gate.
 */
#include "machine.h"
#include "v810_interpreter.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

VbExecutionStats vb_execution;
static uint64_t next_event;
static uint64_t sync_trace[4096];
static unsigned sync_count, deliveries, event_count;
static int pending, scenario;

int32_t vb_devices_cycles_to_next_event(uint64_t cycle) {
    return next_event>cycle?(int32_t)(next_event-cycle):1;
}
void vb_devices_sync(uint64_t cycle) {
    assert(sync_count<4096);sync_trace[sync_count++]=cycle;
    while (cycle>=next_event) {
        next_event+=11;
        if (++event_count%7==0) pending=1;
    }
}
int vb_irq_highest_pending_level(void) {return pending?4:-1;}
int vb_irq_check_and_deliver(CPUState* cpu) {
    if (!pending || cpu->psw_id) return 0;
    pending=0;++deliveries;cpu->halted=0;cpu->pc+=8;return 1;
}
void vb_devices_end_frame(uint64_t cycle) {(void)cycle;}
void vb_game_host_end_field(CPUState* cpu) {(void)cpu;}
void vb_stub_abort(const char* what,uint32_t pc,uint32_t value) {
    (void)what;(void)pc;(void)value;abort();
}
void vb_dispatch(CPUState* cpu,uint32_t pc) {
    assert(pc==cpu->pc);
    if (scenario==3 && cpu->pc==0x100) {
        cpu->pc=0x200;cpu->yielded=1;return;
    }
    while (cpu->cycles<cpu->cycle_deadline) {
        if (scenario==2 && cpu->gpr[1]==9) {
            vb_execution.stopped=1;cpu->yielded=1;return;
        }
        ++cpu->gpr[1];cpu->pc+=2;
        cpu->cycles+=3+cpu->gpr[1]%5;
        --cpu->step_budget;
        if (scenario==1 && cpu->gpr[1]==9) {
            cpu->halted=1;return;
        }
    }
    cpu->yielded=1;
}
typedef struct Result {
    CPUState cpu;
    uint64_t trace[4096];
    unsigned sync_count,deliveries,event_count,services;
} Result;
static Result run(int which,uint32_t service_cycles,uint64_t begin,uint64_t end) {
    Result result={0};scenario=which;next_event=begin+11;
    sync_count=deliveries=event_count=0;pending=0;
    memset(&vb_execution,0,sizeof(vb_execution));
    result.cpu.pc=0x100;result.cpu.cycles=begin;
    uint32_t pc=result.cpu.pc;
    while (result.cpu.cycles<end && !vb_execution.stopped) {
        assert(++result.services<1000);
        vb_machine_run_slice(&result.cpu,&pc,end,service_cycles);
        assert(pc==result.cpu.pc);
    }
    result.sync_count=sync_count;result.deliveries=deliveries;result.event_count=event_count;
    memcpy(result.trace,sync_trace,sync_count*sizeof(*sync_trace));
    return result;
}
static void compare(int which,uint64_t begin,uint64_t end) {
    const Result reference=run(which,0,begin,end);
    const Result candidate=run(which,20,begin,end);
    assert(!memcmp(&reference.cpu,&candidate.cpu,sizeof(CPUState)));
    assert(reference.sync_count==candidate.sync_count);
    assert(reference.deliveries==candidate.deliveries);
    assert(reference.event_count==candidate.event_count);
    assert(!memcmp(reference.trace,candidate.trace,reference.sync_count*sizeof(uint64_t)));
    if (!which) assert(candidate.services<reference.services);
    const Result large=run(which,20000,begin,end);
    assert(!memcmp(&reference.cpu,&large.cpu,sizeof(CPUState)));
    assert(reference.sync_count==large.sync_count);
    assert(!memcmp(reference.trace,large.trace,reference.sync_count*sizeof(uint64_t)));
}
typedef struct FrameResult {
    CPUState cpu;
    uint64_t presents[32],snapshots[32];
    unsigned present_count;
} FrameResult;
static FrameResult frames(uint32_t service_cycles) {
    FrameResult result={0};scenario=0;next_event=11;
    sync_count=deliveries=event_count=0;pending=0;
    memset(&vb_execution,0,sizeof(vb_execution));
    result.cpu.pc=0x100;uint32_t pc=result.cpu.pc;
    uint64_t frame_start=0,last_present=0;unsigned history_frame=UINT32_MAX;
    /* Incommensurate device, instruction, frame and service periods expose
     * presentation phase drift if a slice omits the presentation boundary. */
    while (result.cpu.frame<20) {
        if (result.cpu.cycles-frame_start>=113) {
            frame_start+=113;++result.cpu.frame;
        }
        if (result.cpu.frame==20) break;
        if (history_frame!=result.cpu.frame) {
            history_frame=result.cpu.frame;
            result.snapshots[history_frame]=result.cpu.cycles;
        }
        uint32_t quota=service_cycles;
        if (quota) {
            const uint64_t elapsed=result.cpu.cycles-last_present;
            const uint64_t until=elapsed<113?113-elapsed:1;
            if (until<quota) quota=(uint32_t)until;
        }
        vb_machine_run_slice(&result.cpu,&pc,frame_start+113,quota);
        if (result.cpu.cycles-last_present>=113) {
            last_present=result.cpu.cycles;
            assert(result.present_count<32);
            result.presents[result.present_count++]=last_present;
        }
    }
    return result;
}
int main(void) {
    for (int which=0;which<4;++which) compare(which,0,1200);
    compare(0,UINT64_MAX-111,UINT64_MAX-32);
    const FrameResult reference=frames(0),candidate=frames(20),large=frames(20000);
    assert(!memcmp(&reference,&candidate,sizeof(reference)));
    assert(!memcmp(&reference,&large,sizeof(reference)));
    /* A zero-cycle redirect must return to its host, rather than spin or
     * execute the redirected target before the caller can observe it. */
    CPUState cpu={0};cpu.pc=0x100;scenario=3;next_event=11;
    sync_count=0;pending=0;memset(&vb_execution,0,sizeof(vb_execution));
    uint32_t pc=cpu.pc;
    assert(vb_machine_run_slice(&cpu,&pc,1200,20000));
    assert(cpu.pc==0x200 && pc==0x200 && cpu.cycles==0);
    return 0;
}
