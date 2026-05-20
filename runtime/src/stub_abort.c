/* stub_abort.c — the only LOUD failure floor in vbrecomp.
 *
 * Per CLAUDE.md Rule 3, this is the ONLY whitelisted place where
 * fprintf(stderr, ...) is allowed: a crash banner immediately before
 * abort(). Every other "I want to log something" use case must go
 * through the TCP debug server.
 */
#include "stub_abort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Phase 2+ crash_trace.c will provide a strong definition. For the
 * Phase 1 skeleton we ship a weak (or, on MSVC, simply strong) fallback
 * so the binary links without crash_trace.c present. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
void vb_crash_trace_dump_json(const char* path) { (void)path; }
#else
void vb_crash_trace_dump_json(const char* path) { (void)path; }
#endif

static void vb_print_banner(const char* what, uint32_t pc, uint32_t addr) {
    fprintf(stderr,
        "\n"
        "########################################################################\n"
        "##  vb_stub_abort: %s\n"
        "##  pc   = 0x%08X\n"
        "##  addr = 0x%08X (UINT32_MAX = 'n/a')\n"
        "##\n"
        "##  This is a HARD STUB ABORT. The runtime hit an unimplemented or\n"
        "##  unreachable path. Per CLAUDE.md Rules 0 and 3 this is fatal.\n"
        "##  See vb_last_run_report.json for the rings at time of abort.\n"
        "########################################################################\n"
        "\n",
        what ? what : "(no description)", pc, addr);
    fflush(stderr);
}

void vb_stub_abort(const char* what, uint32_t pc, uint32_t addr) {
    vb_print_banner(what, pc, addr);
    /* Dump rings if the crash module is linked. The Phase 1 skeleton's
     * weak stub is a no-op; Phase 2 implements the JSON dump. */
    vb_crash_trace_dump_json("vb_last_run_report.json");
    abort();
}

void vb_stub_abort_simple(const char* what, uint32_t pc) {
    vb_stub_abort(what, pc, 0xFFFFFFFFu);
}
