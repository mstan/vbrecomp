/* Compile the scalar sampling path independently against the same snapshots. */
#define VB_VIEWPORT_TILE_JOBS 0
#define vb_viewport_state_visit ref_viewport_state_visit
#define vb_viewport_track ref_viewport_track
#define vb_viewport_track_extra ref_viewport_track_extra
#define vb_viewport_clear_frames ref_viewport_clear_frames
#define vb_viewport_reset ref_viewport_reset
#define vb_viewport_register ref_viewport_register
#define vb_viewport_active ref_viewport_active
#define vb_viewport_id ref_viewport_id
#define vb_viewport_window ref_viewport_window
#define vb_viewport_width ref_viewport_width
#define vb_viewport_capture ref_viewport_capture
#define vb_viewport_render ref_viewport_render
#include "../src/viewport.c"
