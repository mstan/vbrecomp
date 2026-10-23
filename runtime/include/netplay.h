#ifndef VB_NETPLAY_H
#define VB_NETPLAY_H
#include "cpu_state.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional shared RNet host. The portable offline build needs no RNet. */
int vb_netplay_option(int argc,char** argv,int* index);
int vb_netplay_enabled(void);
int vb_netplay_players(void);
int vb_netplay_view_seat(void); /* -1 offline, owned racer online */
int vb_netplay_authority_seat(void);
int vb_netplay_from_lobby(void);
int vb_netplay_native_setup(void);
struct RecompLauncherCGameInfo;
struct RecompLauncherCNetplayLaunch;
void vb_netplay_configure_launcher(struct RecompLauncherCGameInfo*);
int vb_netplay_apply_launch(const struct RecompLauncherCNetplayLaunch*);
int vb_netplay_auto_requested(void);
int vb_netplay_auto_launch(const char* config_path);
void vb_netplay_resume_launcher(struct RecompLauncherCGameInfo*);
int vb_netplay_start(CPUState*,uint32_t* dispatch_pc,uint64_t* frame_cycles);
/* Main-thread only: 1 live field, 0 held, -1 refused/left, -2 agreed return. */
int vb_netplay_poll(void);
void vb_netplay_physical_input(uint16_t);
void vb_netplay_override_input(uint16_t,int enabled);
void vb_netplay_stop(void);
const char* vb_netplay_error(void);
int vb_netplay_in_resim(void); /* safe on the audio thread */
int vb_netplay_debug(const char*,long long,char*,size_t);
int vb_netplay_hold(uint32_t tick);
void vb_netplay_release(void);
/* Only authority may open F7, and only between races. All operations use the
 * shared STATE channel and a settled all-seat barrier; online rewind is absent. */
int vb_netplay_state_can_open(void);
int vb_netplay_state_request(const char* path,int save,int ui_slot);
int vb_netplay_state_busy(void);
int vb_netplay_state_result(void); /* 0 pending, 1 complete, -1 refused */
const char* vb_netplay_state_message(void);
#ifdef __cplusplus
}
#endif
#endif
