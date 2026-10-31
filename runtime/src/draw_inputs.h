#ifndef VB_DRAW_INPUTS_H
#define VB_DRAW_INPUTS_H
#include <stdint.h>
#include "savestate.h"
/* Borrowed immutable eight-row capture. Owned by the viewport slot/row until
 * its next complete replacement. Native lazy jobs and expanded replay share
 * these exact inputs; no second DRAM/CHR copy or guest memory substitution. */
typedef struct VbVipDrawInputs {
    const uint16_t *dram,*chars,*spt,*suppress;
    const uint8_t (*gplt)[4],(*jplt)[4];
    unsigned backdrop;
} VbVipDrawInputs;
#ifdef __cplusplus
extern "C" {
#endif
/* Guest-thread-owned retention bits. A captured bank is borrowed until its
 * native row is materialized or its replacement event completes. */
extern uint32_t vb_vip_native_rows[2];
int vb_viewport_draw_inputs(int slot,int block,VbVipDrawInputs* out);
/* Host-derived revision metadata, never guest/save-state content. Only the
 * canonical VIP DRAM/CHR pointers qualify; arbitrary capture inputs fall back. */
#define VB_CAPTURE_REVISION_PAGES 40
void vb_vip_track_capture_revisions(void);
const uint64_t* vb_vip_capture_revisions(const uint16_t* dram,const uint16_t* chars);
void vb_viewport_invalidate_capture_revisions(void);
int vb_viewport_tile_hash(const uint16_t* chars,unsigned tile,uint32_t* hash);
/* Capture only. Tokens belong to stable canonical frozen-bank addresses and
 * are not serialized. A normal visitor remains the complete fallback. */
typedef void (*VbVersionedStateVisit)(void*,const void*,size_t,uint64_t);
void vb_viewport_state_visit_pages(VbStateVisit,VbVersionedStateVisit,void*);
#ifdef __cplusplus
}
#endif
#endif
