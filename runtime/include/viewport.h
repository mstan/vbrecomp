#ifndef VB_VIEWPORT_H
#define VB_VIEWPORT_H
#include "renderer.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Presentation-only VIP replay. Inputs are frozen at each native drawing
 * block, and follow the displayed framebuffer slot. No guest stores occur.
 * Native 384x224 framebuffer/timing APIs retain their original contract. */
#define VB_VIEWPORT_MAX_WIDTH 2048
#define VB_VIEWPORT_MAX_EXTRA 16384
#define VB_VIEWPORT_MAX_SPRITES 96
typedef struct VbViewportScene {
    const uint16_t* worlds; /* 32 records of 16 words, indexed by world number */
    const uint16_t* objects; /* 1024 records of four words */
    const uint16_t* chars; /* 2048 tiles of eight words */
    const uint16_t* dram; /* Read-only captured background maps and parameters. */
    const uint16_t* spt;
    const uint8_t* extra;
    unsigned extra_size;
} VbViewportScene;

/* Map a sample outside a BG world's submitted horizontal bounds back to native
 * source artwork. Return zero for transparent space. Coordinates are unwrapped
 * source-map texels; only an explicitly registered world uses this extension. */
typedef int (*VbViewportOutside)(int x, int y, int* source_x, int* source_y, void*);

typedef struct VbViewportWorldLayout {
    int top, bottom; /* half-open destination y range; empty means no transform */
    int left_end, right_start;
    int left_shift, center_shift, right_shift;
    VbViewportOutside outside;
    void* outside_context;
} VbViewportWorldLayout;
typedef struct VbViewportSprite {
    int world, eye, x, y;
    unsigned width, height, char_count; /* Dimensions in tiles. */
    const uint16_t* cells;
    const uint16_t* chars;
} VbViewportSprite;
typedef struct VbViewportLayout {
    VbViewportWorldLayout worlds[32];
    int object_shift[1024]; /* shifts base x, preserving native eye parallax */
    VbViewportSprite sprites[VB_VIEWPORT_MAX_SPRITES];
    unsigned sprite_count;
} VbViewportLayout;
typedef int (*VbViewportClassify)(const VbViewportScene*, void*);
typedef void (*VbViewportArrange)(const VbViewportScene*, int scene,
                                 int width, VbViewportLayout*, void*);

void vb_viewport_track(void); /* Startup declaration permits paused activation. */
/* Optional title-owned read-only metadata, latched alongside VIP inputs. */
typedef void (*VbViewportCaptureExtra)(uint8_t* destination, unsigned size);
int vb_viewport_track_extra(unsigned size, VbViewportCaptureExtra capture);
int vb_viewport_register(const char* id, unsigned numerator, unsigned denominator,
                         int adaptive, VbViewportClassify classify,
                         VbViewportArrange arrange, void* context);
void vb_viewport_reset(void); /* Selection only; keep already captured frames. */
int vb_viewport_active(void);
const char* vb_viewport_id(void);
void vb_viewport_window(int width, int per_eye_height);
int vb_viewport_width(void); /* Native width for unclassified/title/menu scenes. */

/* Shared rasterizer/debug contract; caller allocates width*224 elements.
 * width=384 bypasses layout transforms and proves native snapshot fidelity. */
int vb_viewport_render(int slot, int eye, int width, uint32_t* argb,
                       uint8_t* levels, uint16_t* worlds, VbSourceTexel* sources);

/* VIP-owned observer entry points. Palette entries are decoded levels 0..3. */
void vb_viewport_clear_frames(void);
void vb_viewport_capture(int slot, int block, const uint16_t* dram,
                         const uint16_t* chars, const uint16_t spt[4],
                         const uint8_t gplt[4][4], const uint8_t jplt[4][4],
                         unsigned backdrop, const uint16_t suppress[1024]);
#ifdef __cplusplus
}
#endif
#endif
