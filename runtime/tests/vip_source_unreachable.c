/* Link guards for unused peripheral entry points retained by PE/COFF ld.
 * This test calls only vip_draw_block_into and source-buffer accessors.
 * A call into any peripheral is a test failure, regardless of its arguments.
 * Keep this translation unit independent of production interface headers.
 */
#include <stdlib.h>
#define UNREACHABLE(name) void name(void) { abort(); }
UNREACHABLE(vb_capture_active)
UNREACHABLE(vb_capture_begin_frame)
UNREACHABLE(vb_capture_end_frame)
UNREACHABLE(vb_capture_init)
UNREACHABLE(vb_capture_tile)
UNREACHABLE(vb_capture_use)
UNREACHABLE(vb_input_frame_advance)
UNREACHABLE(vb_irq_assert)
UNREACHABLE(vb_memory_dump)
UNREACHABLE(vb_mod_runtime_frame_tick_c)
UNREACHABLE(vb_overlay_add)
UNREACHABLE(vb_overlay_reset)
UNREACHABLE(vb_overrides_active)
UNREACHABLE(vb_overrides_init)
UNREACHABLE(vb_overrides_lookup_tile)
UNREACHABLE(vb_recolor_active)
UNREACHABLE(vb_recolor_init)
UNREACHABLE(vb_recolor_select_scene)
UNREACHABLE(vb_recolor_world_pixel)
UNREACHABLE(vb_red_lut_map)
UNREACHABLE(vb_vip_phase_record)
UNREACHABLE(vb_vip_phase_reset)
UNREACHABLE(vb_wram_hash_record)
UNREACHABLE(vb_wram_hash_reset)
UNREACHABLE(vb_wram_region_fnv)
