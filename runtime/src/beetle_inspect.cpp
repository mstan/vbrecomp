/* Oracle inspection adapter; compiled exclusively into vb-beetle. */
#define INLINE inline
#include "mednafen/hw_cpu/v810/v810_cpu.h"
#include <stdint.h>
#include "mednafen/vb/vip.h"
extern V810* VB_V810;
extern "C" uint8_t vb_beetle_vram_byte(uint32_t address) { return VIP_Read8(0,address); }
extern "C" int vb_beetle_cpu_state(uint32_t* pc, uint32_t* gpr, uint32_t* sysreg) {
    if (!VB_V810) return 0;
    *pc=VB_V810->GetPC();
    for (unsigned i=0;i<32;++i) { gpr[i]=VB_V810->GetPR(i); sysreg[i]=VB_V810->GetSR(i); }
    return 1;
}
