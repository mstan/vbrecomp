# Oracle (beetle-vb) RB_CPUHOOK patch — reproducibility record

`beetle-vb/` is vendored at clean upstream HEAD and is **not** a git repo,
so the oracle half of the per-instruction CPU-hook harness (Axes 1/2/3/6,
`tools/cpuhook_compare.py`) is recorded here for reapplication after a
re-clone. Observability ONLY — it does not change emulation, so the oracle
stays bit-identical. After applying, rebuild the archive:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
Set-Location beetle-vb
& "C:\msys64\mingw64\bin\mingw32-make.exe" platform=win STATIC_LINKING=1 -j8
```

The recomp half is in-tree: `runtime/src/cpuhook.c`, `runtime/include/cpuhook.h`,
the `VB_CPUHOOK` macro in `runtime/include/cpu_state.h`, the emitter's
per-instruction `VB_CPUHOOK(cpu);` emit, and the `cpuhook` TCP command in
both `debug_server.c` (4390) and `beetle_debug_server.c` (4391). The record
format (`vb_cpuhook_rec`, 24 bytes: `pc, psw, fnv, pad : u32; cycle : u64`)
and the FNV-1a over registers **r1..r30** (r0 hardwired 0; r31/lp excluded —
the recomp seeds it to the 0xDEAD0000 sentinel) MUST match on both sides.

---

## 1. `beetle-vb/libretro.cpp`

### 1a. Recorder + first-N-from-boot ring — insert after `V810 *VB_V810 = NULL;`

```cpp
#include <stdint.h>
typedef struct {
   uint32_t pc;
   uint32_t psw;     /* S_REG[PSW] (CY=0x8 OV=0x4 S=0x2 Z=0x1, + ID/EP/NP/...) */
   uint32_t fnv;     /* FNV-1a over PR1..PR30 (r0 hardwired 0; r31/lp excluded) */
   uint32_t pad;
   uint64_t cycle;   /* cumulative guest cycle at this instruction */
} vb_cpuhook_rec;

/* FIRST-N-from-boot capture: fills from instruction 0, FREEZES when full
 * (does not wrap), so the boot window [0, capacity) is never evicted. */
#define VB_CPUHOOK_RING (1u << 23)            /* 8M entries * 24B = 192 MiB */
static vb_cpuhook_rec s_vb_ch_ring[VB_CPUHOOK_RING];
static uint64_t       s_vb_ch_total;
uint64_t              g_vb_cpuhook_frame_base; /* cumulative cycles before current frame */

extern "C" void vb_oracle_cpuhook(uint32_t pc, const uint32_t *p_reg,
                                  uint32_t psw, uint64_t cycle_rel)
{
   if (s_vb_ch_total >= VB_CPUHOOK_RING) return;   /* frozen: boot window full */
   uint32_t h = 2166136261u;
   for (int i = 1; i < 31; ++i) { h = (h ^ p_reg[i]) * 16777619u; }
   vb_cpuhook_rec *e = &s_vb_ch_ring[s_vb_ch_total];
   e->pc = pc; e->psw = psw; e->fnv = h; e->pad = 0;
   e->cycle = g_vb_cpuhook_frame_base + cycle_rel;
   s_vb_ch_total++;
}
extern "C" uint64_t vb_oracle_cpuhook_head(void) { return s_vb_ch_total; }
extern "C" uint32_t vb_oracle_cpuhook_query(uint64_t from, uint32_t max,
                                            vb_cpuhook_rec *out,
                                            uint64_t *out_resident_lo)
{
   const uint64_t head = s_vb_ch_total;
   if (out_resident_lo) *out_resident_lo = 0;   /* first-N never evicts */
   uint64_t cur = from;
   uint32_t n = 0;
   while (n < max && cur < head) { out[n++] = s_vb_ch_ring[cur]; cur++; }
   return n;
}
```

### 1b. Cumulative frame-base — in `Emulate()`, after `v810_timestamp = VB_V810->Run(EventHandler);`

```cpp
   g_vb_cpuhook_frame_base += (uint64_t)(uint32_t)v810_timestamp;
```
(`v810_timestamp` is frame-relative — RebaseTS/ResetTS each frame — so adding
each frame's total after Run() recovers a monotonic cumulative cycle; the
records made DURING the frame used the pre-frame base.)

---

## 2. `beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp`

Declare the recorder once (before `V810::Run_Accurate`):

```cpp
extern "C" void vb_oracle_cpuhook(uint32 pc, const uint32 *p_reg,
                                  uint32 psw, uint64 cycle_rel);
```

Then fill the `RB_CPUHOOK(n)` macro in **both** run paths (it is `#define`'d
empty in `Run_Accurate` ~line 504 and `Run_Fast` ~line 536):

```cpp
 #define RB_CPUHOOK(n) vb_oracle_cpuhook((n), P_REG, S_REG[PSW], (uint64)(uint32)timestamp_rl);
```

The hook fires once per retired instruction at `v810_oploop.inc:54` with
`P_REG`/`S_REG`/`timestamp_rl` in scope, BEFORE the instruction executes —
so it captures pre-instruction state, matching the recomp's `VB_CPUHOOK`.
