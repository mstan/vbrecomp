#ifndef VB_VIP_RASTER_H
#define VB_VIP_RASTER_H
#include "draw_inputs.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Display-only descriptions: never serialized or consumed by guest reads.
 * All coordinates are native, independent of output density. A bank is the
 * already-owned eight-row capture; its pointers remain borrowed until the
 * guest thread resumes. Zero/absent page identities require a normal upload. */
typedef struct VbVipRasterBank {
    VbVipDrawInputs input;
    const uint64_t* pages;
} VbVipRasterBank;
typedef struct VbVipRasterJob {
    int32_t bounds[4]; /* half-open native x0,y0,x1,y1 */
    int32_t data[4];   /* kind(-1 backdrop,-2 native bitmap,0..3 VIP), world,OBJ,bank */
} VbVipRasterJob;
#define VB_VIP_RASTER_JOB_LIMIT 65536
void vb_vip_raster_set_active(int active); /* Qualified display request only. */
void vb_vip_set_capture_revision_tracking(int active);
void vb_viewport_raster_enable(int active);
void vb_viewport_raster_track(void); /* Display capture only; never adds state fields. */
void vb_viewport_raster_loaded(void);
int vb_viewport_raster_bank(int slot,int row,VbVipRasterBank*);
int vb_vip_raster_prepare(const VbVipRasterBank banks[28],int eye,uint32_t bitmap_rows,
                         VbVipRasterJob*,unsigned capacity,unsigned* count);
/* Portable native-density consumer of the very same prepared jobs. */
int vb_vip_raster_cpu(const VbVipRasterBank banks[28],const VbVipRasterJob*,unsigned,
                      int eye,const uint8_t* native_bitmap,uint8_t* levels);
/* Guest writer/lifecycle-derived presentation metadata; not snapshot content. */
uint32_t vb_vip_raster_bitmap_rows(int slot);
const uint8_t* vb_vip_raster_bitmap(int slot,int eye,uint64_t* revision);
#ifdef __cplusplus
}
#endif
#endif
