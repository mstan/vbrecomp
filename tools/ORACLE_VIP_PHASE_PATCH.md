# Oracle (beetle-vb) VIP draw-timing phase patch — reproducibility record

`beetle-vb/` is vendored at clean upstream HEAD and is **not** a git repo, so
the oracle half of the Axis-5a draw-timing phase gate
(`tools/vipphase_compare.py`) is recorded here for reapplication after a
re-clone. Observability ONLY — it computes DPSTTS/XPSTTS via the existing
`ReadRegister` (pure reads) at points where `CheckIRQ()` already ran, so the
oracle stays bit-identical (verified: `audio_compare` STRONG MATCH unchanged,
NCC 0.9806). After applying, rebuild the archive:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
Set-Location beetle-vb
& "C:\msys64\mingw64\bin\mingw32-make.exe" platform=win STATIC_LINKING=1 -j8
```
Then relink `vb-beetle` (`cmake --build build --target vb-beetle`).

The recomp half is in-tree: `runtime/src/vip_phase.c`,
`runtime/include/vip_phase.h`, the 4 `vb_vip_phase_record(...)` calls in
`runtime/src/vip.c`, and the `vip_phase` TCP command in both
`debug_server.c` (4390) and `beetle_debug_server.c` (4391). The 16-byte
record `{seq:u64, event:u16, dpstts:u16, xpstts:u16, reserved:u16}` MUST match
on both sides.

---

## 1. `beetle-vb/libretro.cpp` — ring + recorder + query

Insert after the `vb_oracle_cpuhook_query` block (the cpuhook ring):

```cpp
typedef struct {
   uint64_t seq;
   uint16_t event, dpstts, xpstts, reserved;
} vb_vipphase_rec;
#define VB_VIPPHASE_RING (1u << 16)
static vb_vipphase_rec s_vb_vp_ring[VB_VIPPHASE_RING];
static uint64_t        s_vb_vp_total;

extern "C" void vb_oracle_vipphase(uint16_t event, uint16_t dpstts, uint16_t xpstts)
{
   if (s_vb_vp_total >= VB_VIPPHASE_RING) return;   /* frozen: boot window full */
   vb_vipphase_rec *e = &s_vb_vp_ring[s_vb_vp_total];
   e->seq = s_vb_vp_total;
   e->event = event; e->dpstts = dpstts; e->xpstts = xpstts; e->reserved = 0;
   s_vb_vp_total++;
}
extern "C" uint64_t vb_oracle_vipphase_head(void) { return s_vb_vp_total; }
extern "C" uint32_t vb_oracle_vipphase_query(uint64_t from, uint32_t max,
                                             vb_vipphase_rec *out,
                                             uint64_t *out_resident_lo)
{
   const uint64_t head = s_vb_vp_total;
   if (out_resident_lo) *out_resident_lo = 0;   /* first-N never evicts */
   uint64_t cur = from; uint32_t n = 0;
   while (n < max && cur < head) { out[n++] = s_vb_vp_ring[cur]; cur++; }
   return n;
}
```

## 2. `beetle-vb/mednafen/vb/vip.c` — record at each INTPND-raise

Just before `VIP_Update(...)`, declare the recorder + a snapshot macro that
computes DPSTTS (reg 0x20) and XPSTTS (reg 0x40) via the file-local
`ReadRegister`:

```c
extern void vb_oracle_vipphase(uint16_t event, uint16_t dpstts, uint16_t xpstts);
#define VBB_VIPPHASE(ev, ts) \
   vb_oracle_vipphase((ev), ReadRegister((ts), 0x20), ReadRegister((ts), 0x40))
```

Then, inside `VIP_Update`, add one call immediately after each
`InterruptPending |= INT_*; CheckIRQ();` (with `running_timestamp` in scope):

- after `INT_XP_END`   → `VBB_VIPPHASE(INT_XP_END, running_timestamp);`
- after the L/R FB-end → `VBB_VIPPHASE((DisplayRegion & 2) ? INT_RFB_END : INT_LFB_END, running_timestamp);`
- after `INT_FRAME_START` → `VBB_VIPPHASE(INT_FRAME_START, running_timestamp);`
- after `INT_GAME_START`  → `VBB_VIPPHASE(INT_GAME_START, running_timestamp);`

These four are structurally identical to the recomp's `vb_vip_phase_record`
points in `runtime/src/vip.c` (snapshots taken pre-FB-flip, pre-region-bump),
so the event sequences align index-for-index.

---

## 3. `beetle-vb/libretro.cpp` — WRAM fingerprint ring (Axis-6 fidelity)

Folded into the same `vb_oracle_vipphase` recorder: at `event == GAME_START`
(0x0008), hash WRAM into a parallel first-N ring of 64 per-1KiB-region FNV-1a
values (so one volatile cell doesn't avalanche the whole frame). `WRAM` is the
file-scope `static uint8 *WRAM` already in libretro.cpp. Record layout matches
`runtime/include/wram_hash.h` (264 bytes: `seq:u64 + fnv[64]:u32`).

```cpp
#define VB_WRAMHASH_REGIONS 64
typedef struct { uint64_t seq; uint32_t fnv[VB_WRAMHASH_REGIONS]; } vb_wramhash_rec;
#define VB_WRAMHASH_RING (1u << 15)
static vb_wramhash_rec s_vb_wh_ring[VB_WRAMHASH_RING];
static uint64_t        s_vb_wh_total;
/* inside vb_oracle_vipphase(), before the vipphase record: */
if (event == 0x0008 && WRAM && s_vb_wh_total < VB_WRAMHASH_RING) {
   vb_wramhash_rec *w = &s_vb_wh_ring[s_vb_wh_total];
   w->seq = s_vb_wh_total;
   for (int r = 0; r < VB_WRAMHASH_REGIONS; ++r) {
      const uint8_t *p = WRAM + r * 1024;
      uint32_t h = 2166136261u;
      for (int i = 0; i < 1024; ++i) { h ^= p[i]; h *= 16777619u; }
      w->fnv[r] = h;
   }
   s_vb_wh_total++;
}
/* + extern "C" vb_oracle_wramhash_head()/_query() (mirror the vipphase ones). */
```
Recomp half: `runtime/src/wram_hash.c` + `.h`, `vb_wram_region_fnv` in
memory.c, the GAME_START call in vip.c, and the `wram_hash` TCP command on
both servers. Compared by `tools/wramhash_compare.py`.

## Result

`tools/vipphase_compare.py --rom roms/marios_tennis.vb`: **PHASE MATCH** —
all draw-timing events (≈17k over ~3.4k frames, and ≈27k over ~5.4k frames in
longer runs) identical in event type, DPSTTS, AND XPSTTS from boot. The VIP
draw-timing state machine is oracle-exact at every event boundary; the
cpuhook's earlier sub-frame DPSTTS-read divergence is the ≤95-cycle Axis-2
timing jitter sampled mid-column, not a draw-timing-model difference.
