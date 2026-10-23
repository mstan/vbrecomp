#ifndef VB_STATE_UI_H
#define VB_STATE_UI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-owned modal UI, following the PSX/SNES slot browser and filmstrip.
 * Browsing never restores a snapshot. Only explicit Load/Select commits it. */
enum VbStateUiAction {
    VB_STATE_UI_SLOTS, VB_STATE_UI_REWIND, VB_STATE_UI_BACK,
    VB_STATE_UI_LEFT, VB_STATE_UI_RIGHT, VB_STATE_UI_UP, VB_STATE_UI_DOWN,
    VB_STATE_UI_LOAD, VB_STATE_UI_SAVE
};
void vb_state_ui_init(const char* quick_path);
void vb_state_ui_shutdown(void);
int vb_state_ui_mode(void); /* 0 closed, 1 slots, 2 rewind */
int vb_state_ui_action(int action);
void vb_state_ui_select_slot(int slot);
int vb_state_ui_slot(void);
int vb_state_ui_result(void); /* 0 idle, 1 success, -1 rejected */
unsigned vb_state_ui_history_count(void);
uint32_t vb_state_ui_selected_frame(void);
void vb_state_ui_record(uint32_t frame);
void vb_state_ui_sync(void);
const uint32_t* vb_state_ui_render(int* width, int* height);
#ifdef __cplusplus
}
#endif
#endif
