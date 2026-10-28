#ifndef VB_DRAW_INPUTS_H
#define VB_DRAW_INPUTS_H
#include <stdint.h>
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
#ifdef __cplusplus
}
#endif
#endif
