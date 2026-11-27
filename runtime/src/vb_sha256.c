#include "vb_sha256.h"
#include <recomp_mod/sha256.h>
void vb_sha256_compute(const uint8_t *data, size_t len, uint8_t out[32]) {
    rmod_sha256_compute(data, len, out);
}
