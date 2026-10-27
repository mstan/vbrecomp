/* Compare direct reads with the original live bus boundary, including MMIO
 * synchronization and deadlines. All bytes are synthetic test fixtures. */
#define VBRECOMP_NATIVE_READS 1
#include "../src/memory.c"
#include <assert.h>

static uint64_t ticks[4];
static unsigned reads;
#define DEVICE(name, number) \
void name##_init(void) {} \
uint8_t name##_read8(uint32_t a) {++reads;return (uint8_t)(a^(number));} \
uint16_t name##_read16(uint32_t a) {++reads;return (uint16_t)(a^(number));} \
uint32_t name##_read32(uint32_t a) {++reads;return a^(number);} \
void name##_write8(uint32_t a,uint8_t v) {(void)a;(void)v;abort();} \
void name##_write16(uint32_t a,uint16_t v) {(void)a;(void)v;abort();} \
void name##_write32(uint32_t a,uint32_t v) {(void)a;(void)v;abort();}
DEVICE(vb_vip,0x12345678u)
DEVICE(vb_vsu,0x89abcdefu)
DEVICE(vb_timer,0x24578136u)
DEVICE(vb_input,0x57281364u)
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
void vb_wtrace_record(uint32_t a,uint32_t v,uint8_t w) {(void)a;(void)v;(void)w;abort();}
void vb_vip_record_cpu_write(uint32_t a,uint32_t v,unsigned w,uint32_t tag) {(void)a;(void)v;(void)w;(void)tag;abort();}
int vb_rom_patch_attach(uint8_t* p,uint32_t n) {(void)p;(void)n;return 1;}
VB_NORETURN void vb_stub_abort(const char* s,uint32_t pc,uint32_t a) {(void)s;(void)pc;(void)a;abort();}

static uint32_t legacy(uint32_t a,unsigned width) {
    bus_begin(a,0);
    uint32_t v=width==1?raw_read8(a):width==2?raw_read16(a):raw_read32(a);
    bus_end(a);return v;
}
static CPUState cpu;
static void reset_boundary(void) {
    memset(&cpu,0,sizeof(cpu));cpu.cycles=100003;cpu.cycle_deadline=100100;cpu.bus_tail_cycles=3;
    s_timer_cycle=s_vip_cycle=s_vsu_cycle=s_input_cycle=0;
    memset(ticks,0,sizeof(ticks));reads=0;vb_memory_set_cpu(&cpu);
}
static void compare(uint32_t a,unsigned width) {
    reset_boundary();uint32_t expected=legacy(a,width);
    CPUState expected_cpu=cpu;
    uint64_t expected_ticks[4];memcpy(expected_ticks,ticks,sizeof(ticks));
    uint64_t expected_clocks[4]={s_timer_cycle,s_vip_cycle,s_vsu_cycle,s_input_cycle};
    unsigned expected_reads=reads;
    reset_boundary();uint32_t actual=width==1?vb_read8(a):width==2?vb_read16(a):vb_read32(a);
    uint64_t clocks[4]={s_timer_cycle,s_vip_cycle,s_vsu_cycle,s_input_cycle};
    assert(actual==expected && !memcmp(&cpu,&expected_cpu,sizeof(cpu)));
    assert(!memcmp(ticks,expected_ticks,sizeof(ticks)) && !memcmp(clocks,expected_clocks,sizeof(clocks)));
    assert(reads==expected_reads);
}
int main(int argc,char** argv) {
    assert(argc==2);unsigned comparisons=0;
    assert(ordinary_rom_read_mask(1)==0 && ordinary_rom_read_mask(3)==0);
    assert(ordinary_rom_read_mask(4)==3 && ordinary_rom_read_mask(8192)==8191);
    assert(ordinary_rom_read_mask(0x01000000)==0x00ffffff);
    assert(ordinary_rom_read_mask(0x02000000)==0x00ffffff);
    assert(ordinary_rom_read_mask(0x80000000)==0x00ffffff);
    for(unsigned size=8192;size<=12288;size+=4096) {
        FILE* f=fopen(argv[1],"wb");assert(f);
        for(unsigned i=0;i<size;++i)assert(fputc((i*37+size/256)&255,f)!=EOF);
        assert(!fclose(f));assert(!vb_memory_init(argv[1]));
        assert(s_rom_read_mask==(size==8192?8191u:0u));
        for(unsigned i=0;i<65536;++i) {s_wram[i]=(uint8_t)(i*13);s_cart_ram[i]=(uint8_t)(i*29);}
        for(unsigned i=0;i<65536;++i) {
            uint32_t a=(i*2654435761u)&0x00ffffff;
            for(unsigned region=5;region<=7;++region)for(unsigned w=1;w<=4;w*=2) {
                compare((region<<24)|a|((i&15)<<28),w);++comparisons;
            }
        }
        /* Both direct and fallback paths see live mutation and slot reuse. */
        s_rom[3]^=0xff;s_wram[65535]^=0x7f;s_cart_ram[0]^=0x42;
        compare(0xfffffff3u,4);compare(0xfdffffffu,2);compare(0xfe010000u,1);
        for(unsigned region=0;region<3;++region)for(unsigned i=0;i<256;++i)for(unsigned w=1;w<=4;w*=2) {
            compare((region<<24)|i|0xf8000000u,w);++comparisons;
        }
        vb_memory_shutdown();assert(!s_rom_read_mask);
    }
    /* Oversized images are accepted by the original loader: only the first
     * 16 MiB is addressable. Distinct halves catch retaining bank bits. */
    s_rom_size=0x02000000;s_rom=(uint8_t*)malloc(s_rom_size);assert(s_rom);
    s_rom_read_mask=ordinary_rom_read_mask(s_rom_size);
    memset(s_rom,0x3c,0x01000000);memset(s_rom+0x01000000,0xc3,0x01000000);
    for(unsigned i=0;i<4096;++i)for(unsigned w=1;w<=4;w*=2) {
        compare(0x07000000u|((i*2654435761u)&0x00ffffffu)|((i&31u)<<27),w);++comparisons;
    }
    vb_memory_shutdown();
    assert(!remove(argv[1]));printf("PASS: %u native/legacy reads, aliases, alignment, live bytes, oversized/non-power-of-two ROM and MMIO timing\n",comparisons);
    return 0;
}
