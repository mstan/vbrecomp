#include "v810_interpreter.h"
#include "simulation_points.h"
#include "regions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[65536];
static uint32_t random_state=0x810;
static uint32_t next_random(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static uint8_t read8(uint32_t a){return rom[a&65535];}
static uint16_t read16(uint32_t a){return read8(a)|(uint16_t)read8(a+1)<<8;}
static uint32_t read32(uint32_t a){return read16(a)|(uint32_t)read16(a+2)<<16;}
void vb_stub_abort(const char* reason,uint32_t pc,uint32_t addr){fprintf(stderr,"%s %x %x\n",reason,pc,addr);abort();}
void vb_stub_abort_simple(const char* reason,uint32_t pc){vb_stub_abort(reason,pc,0);}
void vb_fntrace_record(uint32_t pc,uint32_t lp){(void)pc;(void)lp;}
static uint64_t hook_hash,hook_count;
void vb_cpuhook_record(const CPUState* c){hook_hash=hook_hash*0x100000001b3ull+c->pc+c->cycles+c->gpr[3];++hook_count;}
static int observe(CPUState* c,void* context){(void)context;c->gpr[3]^=(uint32_t)c->cycles;c->psw_cy^=1;return 0;}
static int redirect(CPUState* c,void* context){(void)context;c->gpr[6]++;c->pc+=2;return 1;}
static CPUState setup(unsigned variant,unsigned entry,unsigned sample){
    CPUState c;memset(&c,0,sizeof(c));
    for(unsigned i=1;i<32;++i)c.gpr[i]=next_random();
    c.read8=read8;c.read16=read16;c.read32=read32;
    c.pc=region_pcs[variant][entry];
    c.cycles=sample%7==0 ? UINT64_MAX-45 : next_random()%1000;
    unsigned headroom=sample%101;
    c.cycle_deadline=headroom>UINT64_MAX-c.cycles ? UINT64_MAX : c.cycles+headroom;
    if(sample%11==0)c.cycle_deadline=c.cycles-1;
    c.step_budget=sample%59;c.pipeline_class=(int8_t)((sample%6)-1);c.bus_tail_cycles=sample%5;
    c.psw_z=sample&1;c.psw_s=(sample>>1)&1;c.psw_ov=(sample>>2)&1;c.psw_cy=(sample>>3)&1;
    return c;
}
int main(int argc,char** argv){
    assert(argc==2);FILE* file=fopen(argv[1],"rb");assert(file);
    assert(fread(rom,1,sizeof(rom),file)==sizeof(rom));fclose(file);
    unsigned checks=0;
    for(unsigned variant=0;variant<REGION_CASES;++variant){
        for(unsigned mode=0;mode<5;++mode){
            vb_simulation_points_clear();
            if(mode==1 || mode==2){
                uint32_t pc=region_pcs[variant][6];
                assert(vb_simulation_point_register(pc,read16(pc),mode==1 ? observe:redirect,NULL));
                assert(vb_simulation_points_enable(1));
            }
            for(unsigned sample=0;sample<1500;++sample){
                unsigned entry=sample%REGION_INSTRUCTIONS;
                // Half begin at the run entry to exercise the optimized path.
                if(sample&1)entry=0;
                CPUState a=setup(variant,entry,sample),b=a,oracle=a;
                VbExecutionStats stats={0};stats.mode=VB_EXEC_HYBRID;
                if(mode==3){stats.breakpoint_enabled=1;stats.breakpoint_pc=region_pcs[variant][(entry+3)%REGION_INSTRUCTIONS];}
                if(mode==4){stats.stepping=1;stats.step_remaining=sample%7;}
                vb_execution=stats;hook_hash=hook_count=0;region_old[variant](&a);
                VbExecutionStats expected=vb_execution;uint64_t hash=hook_hash,count=hook_count;
                vb_execution=stats;hook_hash=hook_count=0;region_new[variant](&b);
                if(memcmp(&a,&b,sizeof(a)) || memcmp(&expected,&vb_execution,sizeof(expected)) || hash!=hook_hash || count!=hook_count){
                    fprintf(stderr,"native region mismatch case%u mode%u sample%u entry%u pc%x/%x cycles%llu/%llu\n",variant,mode,sample,entry,a.pc,b.pc,(unsigned long long)a.cycles,(unsigned long long)b.cycles);abort();
                }
                // Independent instruction implementation, with the same outer
                // budget/deadline scheduling; compare complete architectural state.
                vb_execution=stats;
                while(oracle.pc<region_end[variant]){
                    if(!oracle.step_budget || oracle.cycles>=oracle.cycle_deadline){oracle.yielded=1;break;}
                    --oracle.step_budget;vb_interpreter_step(&oracle);
                    if(oracle.yielded || vb_execution.stopped)break;
                }
                if(memcmp(&a,&oracle,sizeof(a))){
                    fprintf(stderr,"interpreter region mismatch case%u mode%u sample%u entry%u pc%x/%x\n",variant,mode,sample,entry,a.pc,oracle.pc);abort();
                }
                ++checks;
            }
        }
    }
    printf("%u native/original/interpreter region cases passed\n",checks);
}
