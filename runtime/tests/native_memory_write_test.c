/* Differential contract against the original bus writer, including complete
 * pre-store CPU/trace/observer state, virtual aliases and device ordering. */
#define VBRECOMP_NATIVE_WRITES 1
#define VB_TRACE_RECORDING 1
#include "../src/memory.c"
#include <assert.h>

typedef struct Event {unsigned kind;uint32_t addr,value,width;CPUState cpu;uint8_t old[4];} Event;
static Event events[16];static unsigned count;static CPUState cpu;
static uint64_t ticks[4];
static void record(unsigned kind,uint32_t a,uint32_t v,unsigned w) {
    assert(count<16);Event* e=&events[count++];memset(e,0,sizeof(*e));
    e->kind=kind;e->addr=a;e->value=v;e->width=w;
    if(s_bus_cpu)e->cpu=*s_bus_cpu;
    unsigned r=(a>>24)&7u;
    if(r==5 || r==6)memcpy(e->old,(r==5?s_wram:s_cart_ram)+(a&65535u),w);
}
#define DEVICE(name, number) \
void name##_init(void) {} \
uint8_t name##_read8(uint32_t a) {(void)a;return 0;} \
uint16_t name##_read16(uint32_t a) {(void)a;return 0;} \
uint32_t name##_read32(uint32_t a) {(void)a;return 0;} \
void name##_write8(uint32_t a,uint8_t v) {record(number,a,v,1);} \
void name##_write16(uint32_t a,uint16_t v) {record(number,a,v,2);} \
void name##_write32(uint32_t a,uint32_t v) {record(number,a,v,4);}
DEVICE(vb_vip,3) DEVICE(vb_vsu,4) DEVICE(vb_timer,5) DEVICE(vb_input,6)
void vb_timer_tick(uint32_t n) {ticks[0]+=n;}
void vb_input_tick(uint32_t n) {ticks[1]+=n;}
void vb_vip_tick(uint64_t n) {ticks[2]+=n;}
void vb_vsu_tick(uint64_t n) {ticks[3]+=n;}
void vb_vsu_end_frame(void) {}
int32_t vb_timer_cycles_to_next_event(void) {return 17;}
int32_t vb_input_cycles_to_next_event(void) {return 31;}
int32_t vb_vip_cycles_to_next_event(void) {return 47;}
void vb_irq_init(void) {}
int vb_irq_highest_pending_level(void) {return 3;}
void vb_wtrace_record(uint32_t a,uint32_t v,uint8_t w) {record(1,a,v,w);}
void vb_vip_record_cpu_write(uint32_t a,uint32_t v,unsigned w,uint32_t tag) {assert(tag==0xcafeu || !s_write_observer || !s_bus_cpu);record(7,a,v,w);}
uint32_t vb_vip_native_pending;
void vb_vip_materialize_framebuffer(uint32_t a,unsigned w) {record(8,a,0,w);}
int vb_rom_patch_attach(uint8_t* p,uint32_t n) {(void)p;(void)n;return 1;}
VB_NORETURN void vb_stub_abort(const char* s,uint32_t pc,uint32_t a) {(void)s;(void)pc;(void)a;abort();}
static uint32_t observer(const CPUState* c,uint32_t a,uint32_t v,unsigned w) {assert(c==&cpu);record(2,a,v,w);return 0xcafeu;}
static void reset(unsigned attached,unsigned observing) {
    memset(&cpu,0,sizeof(cpu));cpu.pc=0x07012344;cpu.cycles=100003;cpu.cycle_deadline=100100;cpu.bus_tail_cycles=3;
    for(unsigned i=0;i<32;++i)cpu.gpr[i]=i*12345u;
    s_timer_cycle=s_vip_cycle=s_vsu_cycle=s_input_cycle=0;
    memset(ticks,0,sizeof(ticks));memset(events,0,sizeof(events));count=0;
    vb_memory_set_cpu(attached?&cpu:NULL);s_write_observer=observing?observer:NULL;
}
static void compare(uint32_t a,uint32_t v,unsigned w,unsigned attached,unsigned observing) {
    static uint8_t expected_wram[65536],expected_sram[65536];
    for(unsigned i=0;i<65536;++i){s_wram[i]=(uint8_t)(i*13u);s_cart_ram[i]=(uint8_t)(i*29u);}
    reset(attached,observing);vb_vip_native_pending=1;
    bus_begin(a,1);if(w==1)raw_write8(a,(uint8_t)v);else if(w==2)raw_write16(a,(uint16_t)v);else raw_write32(a,v);bus_end(a);
    Event expected[16];memcpy(expected,events,sizeof(expected));unsigned n=count;
    CPUState expected_cpu=cpu;uint64_t clocks[]={s_timer_cycle,s_vip_cycle,s_vsu_cycle,s_input_cycle};
    uint64_t expected_ticks[4];memcpy(expected_ticks,ticks,sizeof(ticks));
    memcpy(expected_wram,s_wram,sizeof(s_wram));memcpy(expected_sram,s_cart_ram,sizeof(s_cart_ram));uint8_t misc=s_misc_wcr;
    for(unsigned i=0;i<65536;++i){s_wram[i]=(uint8_t)(i*13u);s_cart_ram[i]=(uint8_t)(i*29u);}
    s_misc_wcr=0;reset(attached,observing);vb_vip_native_pending=1;
    if(w==1)vb_write8(a,(uint8_t)v);else if(w==2)vb_write16(a,(uint16_t)v);else vb_write32(a,v);
    uint64_t actual_clocks[]={s_timer_cycle,s_vip_cycle,s_vsu_cycle,s_input_cycle};
    assert(count==n && !memcmp(expected,events,sizeof(events)));
    assert(!memcmp(&cpu,&expected_cpu,sizeof(cpu)) && !memcmp(clocks,actual_clocks,sizeof(clocks)));
    assert(!memcmp(expected_ticks,ticks,sizeof(ticks)) && !memcmp(expected_wram,s_wram,sizeof(s_wram)) && !memcmp(expected_sram,s_cart_ram,sizeof(s_cart_ram)) && misc==s_misc_wcr);
    s_misc_wcr=0;
}
int main(void) {
    unsigned comparisons=0;
    for(unsigned mode=0;mode<4;++mode)for(unsigned i=0;i<2048;++i)for(unsigned w=1;w<=4;w*=2) {
        uint32_t a=i*2654435761u;
        for(unsigned r=5;r<=6;++r){compare((a&0xf8ffffffu)|(r<<24),a^0xabcdef01u,w,mode&1,mode&2);++comparisons;}
    }
    for(unsigned r=0;r<3;++r)for(unsigned a=0;a<128;++a)for(unsigned w=1;w<=4;w*=2) {
        compare(0xf8000000u|(r<<24)|a,0x12345678u,w,1,1);++comparisons;
    }
    puts("PASS: ordinary store aliases/alignment, full pre-store trace/observer CPU and bytes, SRAM, MMIO ticks and deadlines");
    printf("%u differential stores\n",comparisons);return 0;
}
