#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int vb_stereo_anaglyph_enabled(void);
/* Red lens on the left, cyan lens on the right. The two inputs must be
 * independently rendered eye images of the same size. Left is replaced. */
void vb_stereo_anaglyph(uint32_t* left, const uint32_t* right, size_t count);
#ifdef __cplusplus
}
#endif
