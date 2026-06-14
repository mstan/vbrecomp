/*
 * vb_trace_stub.c — production no-op stand-in for the runtime's dev tooling
 * (debug_server.c + wtrace.c + fntrace.c).
 *
 * Compiled instead of those three when VBRECOMP_DEBUG_TOOLS is OFF (release:
 * tools/build-linux.sh --config prod). It satisfies every public symbol that
 * the always-compiled runtime (main.cpp, memory.c) references, so the runtime
 * links while carrying NO TCP debug server, NO 1M-entry write-trace ring, and
 * NO 256K function-entry ring. vb_debug_server_start() returning 0 without
 * opening a socket is the point: a release build never listens on a port.
 *
 * Get the real tooling back with -DVBRECOMP_DEBUG_TOOLS=ON, which compiles
 * debug_server.c/wtrace.c/fntrace.c in this file's place. Signatures here mirror
 * wtrace.h / fntrace.h / debug_server.h exactly.
 */
#include "wtrace.h"
#include "fntrace.h"
#include "debug_server.h"
#include "cpu_state.h"
#include <stddef.h>
#include <stdint.h>

/* ---- write-trace ring (wtrace.h) ---- */
void   vb_wtrace_init(void) {}
void   vb_wtrace_shutdown(void) {}
void   vb_wtrace_set_active_cpu(struct CPUState* cpu) { (void)cpu; }
void   vb_wtrace_record(uint32_t addr, uint32_t value, uint8_t width) { (void)addr; (void)value; (void)width; }
void   vb_wtrace_reset(void) {}
uint64_t vb_wtrace_seq(void) { return 0; }
size_t vb_wtrace_capacity(void) { return 0; }
size_t vb_wtrace_query(const VBWTraceFilter* f, VBWTraceEntry* out, size_t max) {
    (void)f; (void)out; (void)max; return 0;
}

/* ---- function-entry ring (fntrace.h) ---- */
void   vb_fntrace_init(void) {}
void   vb_fntrace_shutdown(void) {}
void   vb_fntrace_set_active_cpu(struct CPUState* cpu) { (void)cpu; }
void   vb_fntrace_record(uint32_t pc, uint32_t lp) { (void)pc; (void)lp; }
void   vb_fntrace_reset(void) {}
uint64_t vb_fntrace_seq(void) { return 0; }
size_t vb_fntrace_capacity(void) { return 0; }
size_t vb_fntrace_query(const VBFnTraceFilter* f, VBFnTraceEntry* out, size_t max) {
    (void)f; (void)out; (void)max; return 0;
}

/* ---- TCP debug server (debug_server.h) ---- */
int  vb_debug_server_start(int port, CPUState* cpu) { (void)port; (void)cpu; return 0; }
void vb_debug_server_stop(void) {}
int  vb_debug_server_poll(void) { return 0; }  /* 0 = keep running, never quits via TCP */
