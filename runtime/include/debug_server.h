/* debug_server.h — TCP JSON command server.
 *
 * Per CLAUDE.md Rule 3: this is the ONLY sanctioned debugging interface.
 * Default port: VB_DEFAULT_DEBUG_PORT (compile-time, 4390 for the
 * runtime, 4391 for the oracle).
 */
#ifndef VB_DEBUG_SERVER_H
#define VB_DEBUG_SERVER_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the listener on `port`. Returns 0 on success.
 * `cpu` is borrowed for the lifetime of the server (used by handlers
 * that inspect register state). */
int vb_debug_server_start(int port, CPUState* cpu);

void vb_debug_server_stop(void);

/* Service pending TCP work (accept + read + dispatch). Non-blocking.
 * Returns 1 if a "quit" command was received, 0 otherwise. */
int vb_debug_server_poll(void);
int vb_debug_server_is_paused(void);
void vb_debug_server_set_paused(int paused);
/* Optional host presentation capture, including UI. Callback returns 0 on success. */
typedef int (*VbDebugHostCapture)(const char* path, void* context);
void vb_debug_server_set_host_capture(VbDebugHostCapture capture, void* context);

#ifdef __cplusplus
}
#endif

#endif /* VB_DEBUG_SERVER_H */
