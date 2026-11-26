#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int vb_diagnostics_install(const char* root,const char* game,const char* sha);
int vb_diagnostics_enabled(void);
void vb_diagnostics_record(uint32_t frame,uint16_t pad);
void vb_diagnostics_shutdown(void);
#ifdef __cplusplus
}
#endif
