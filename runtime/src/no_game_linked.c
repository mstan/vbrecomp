/* Generic ROM host: interpret until a generated cartridge is linked. */
#include "cpu_state.h"
#include "v810_interpreter.h"
static void run(CPUState* cpu,uint64_t return_pc) {
    while(cpu->pc!=return_pc && !cpu->halted && !cpu->yielded) {
        if(!cpu->step_budget || cpu->cycles>=cpu->cycle_deadline) { cpu->yielded=1;return; }
        if(vb_execution.mode==VB_EXEC_INTERPRETER) {
            --cpu->step_budget;vb_interpreter_step(cpu);
        } else vb_interpreter_fallback(cpu);
    }
}
void vb_dispatch(CPUState* cpu,uint32_t pc) { cpu->pc=pc;run(cpu,UINT64_MAX); }
void vb_dispatch_call(CPUState* cpu,uint32_t pc,uint32_t lp) { cpu->pc=pc;cpu->gpr[31]=lp;run(cpu,lp); }
uint32_t vb_game_expected_crc32(void) { return 0; }
