/* stub_abort.h — the single LOUD failure floor for the vbrecomp runtime.
 *
 * Per CLAUDE.md Rules 0 and 3: a stub is fatal. Every unmapped read,
 * unrecognised MMIO register, untranslated opcode, or "shouldn't be
 * reachable" path routes through vb_stub_abort(). It prints a banner
 * (the one whitelisted printf in the whole project), dumps any ring
 * buffers to vb_last_run_report.json, and calls abort().
 */
#ifndef VB_STUB_ABORT_H
#define VB_STUB_ABORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define VB_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#  define VB_NORETURN __declspec(noreturn)
#else
#  define VB_NORETURN
#endif

VB_NORETURN void vb_stub_abort(const char* what,
                               uint32_t pc,
                               uint32_t addr);

/* Convenience: abort with no addr (use UINT32_MAX as a sentinel). */
VB_NORETURN void vb_stub_abort_simple(const char* what, uint32_t pc);

#ifdef __cplusplus
}
#endif

#endif /* VB_STUB_ABORT_H */
