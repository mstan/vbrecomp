/* watchdog.cpp — see watchdog.h. Observe-only main-loop hang detector.
 *
 * Adapted from psxrecomp's freeze_heartbeat.c (CLAUDE.md §7/§8: salvage proven
 * sibling infrastructure). Two outputs, both written by the watchdog thread so
 * they survive a fully-wedged main thread:
 *
 *   vb_freeze_heartbeat.json   — overwritten every poll with the live snapshot
 *                                (no log growth; CLAUDE.md §3-compliant). A
 *                                reader polls it any time to see current state.
 *   vb_freeze_dump_<wall>.json — written ONCE per stall episode (a crash-style
 *                                forensic, like vb_stub_abort): the snapshot
 *                                plus the main thread's symbolized native call
 *                                stack, captured by suspending it and walking
 *                                with StackWalk64. This names the exact C
 *                                function the runtime is wedged in.
 */
#include "watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <dbghelp.h>
#endif

namespace {

std::atomic<uint64_t> g_beats{0};
std::atomic<int>      g_phase{VB_WD_INIT};
std::atomic<uint32_t> g_pc{0};
std::atomic<uint64_t> g_cycles{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<int64_t>  g_last_change_ms{0};

std::thread           g_thread;
std::atomic<bool>     g_running{false};

#ifdef _WIN32
HANDLE s_main_thread    = NULL;   /* real (duplicated) handle to the main thread */
DWORD  s_main_thread_id = 0;
int    s_sym_initialized = 0;
#endif

constexpr int STALL_MS  = 3000;   /* Win32 flags "Not Responding" at ~5s */
constexpr int POLL_MS   = 250;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

void snapshot(int* phase, uint32_t* pc, uint64_t* cyc, uint64_t* frame) {
    *phase = g_phase.load(std::memory_order_relaxed);
    *pc    = g_pc.load(std::memory_order_relaxed);
    *cyc   = g_cycles.load(std::memory_order_relaxed);
    *frame = g_frame.load(std::memory_order_relaxed);
}

/* Continuously-overwritten live snapshot. */
void write_heartbeat(int stalled_ms) {
    FILE* f = std::fopen("vb_freeze_heartbeat.json", "wb");
    if (!f) return;
    int phase; uint32_t pc; uint64_t cyc, frame;
    snapshot(&phase, &pc, &cyc, &frame);
    std::fprintf(f,
        "{\"backend\":\"vb-runtime\",\"wall\":%lld,\"beats\":%llu,"
        "\"phase\":\"%s\",\"pc\":\"0x%08X\",\"cycles\":%llu,"
        "\"frame\":%llu,\"stalled_ms\":%d}\n",
        (long long)std::time(nullptr),
        (unsigned long long)g_beats.load(std::memory_order_relaxed),
        vb_watchdog_phase_name(phase), pc,
        (unsigned long long)cyc, (unsigned long long)frame, stalled_ms);
    std::fclose(f);
}

#ifdef _WIN32
/* Suspend the main thread, walk + symbolize its native stack into `f` as a
 * JSON array. Best-effort. Adapted from psxrecomp freeze_dump_main_stack_json. */
void dump_main_stack_json(FILE* f) {
    if (!s_main_thread) { std::fputs("[]", f); return; }
    if (GetCurrentThreadId() == s_main_thread_id) { std::fputs("[]", f); return; }

    if (!s_sym_initialized) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(GetCurrentProcess(), NULL, TRUE)) s_sym_initialized = 1;
        else { std::fputs("[]", f); return; }
    }

    if (SuspendThread(s_main_thread) == (DWORD)-1) { std::fputs("[]", f); return; }

    CONTEXT ctx; std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(s_main_thread, &ctx)) {
        ResumeThread(s_main_thread); std::fputs("[]", f); return;
    }

    STACKFRAME64 frame; std::memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip; frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#else
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip; frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    std::fputc('[', f);
    int first = 1;
    union { SYMBOL_INFO si; char buf[sizeof(SYMBOL_INFO) + 512]; } st;
    SYMBOL_INFO* sym = &st.si;
    for (int depth = 0; depth < 128; depth++) {
        if (!StackWalk64(machine, GetCurrentProcess(), s_main_thread, &frame,
                         &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL)) break;
        DWORD64 addr = frame.AddrPC.Offset;
        if (!addr) break;
        std::memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;
        DWORD64 disp = 0;
        BOOL got = SymFromAddr(GetCurrentProcess(), addr, &disp, sym);
        std::fprintf(f, "%s{\"depth\":%d,\"addr\":\"0x%016llX\"",
                     first ? "" : ",", depth, (unsigned long long)addr);
        if (got) {
            char safe[512]; size_t k = 0;
            for (size_t i = 0; sym->Name[i] && k < sizeof(safe) - 2; i++) {
                char c = sym->Name[i];
                safe[k++] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
            }
            safe[k] = 0;
            std::fprintf(f, ",\"symbol\":\"%s\",\"displacement\":%llu",
                         safe, (unsigned long long)disp);
        }
        std::fputc('}', f);
        first = 0;
    }
    std::fputc(']', f);
    ResumeThread(s_main_thread);
}
#else
void dump_main_stack_json(FILE* f) { std::fputs("[]", f); }
#endif

/* One-shot forensic dump for a stall episode. */
void write_freeze_dump(int stalled_ms) {
    char path[128];
    std::snprintf(path, sizeof(path), "vb_freeze_dump_%lld.json",
                  (long long)std::time(nullptr));
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    int phase; uint32_t pc; uint64_t cyc, frame;
    snapshot(&phase, &pc, &cyc, &frame);
    std::fprintf(f,
        "{\n  \"backend\":\"vb-runtime\",\n  \"wall\":%lld,\n"
        "  \"stalled_ms\":%d,\n  \"phase\":\"%s\",\n  \"pc\":\"0x%08X\",\n"
        "  \"cycles\":%llu,\n  \"frame\":%llu,\n  \"beats\":%llu,\n"
        "  \"main_thread_stack\":",
        (long long)std::time(nullptr), stalled_ms,
        vb_watchdog_phase_name(phase), pc,
        (unsigned long long)cyc, (unsigned long long)frame,
        (unsigned long long)g_beats.load(std::memory_order_relaxed));
    dump_main_stack_json(f);
    std::fprintf(f, "\n}\n");
    std::fclose(f);
}

void watchdog_main() {
    uint64_t last_beats = g_beats.load(std::memory_order_relaxed);
    int64_t  last_change = now_ms();
    g_last_change_ms.store(last_change, std::memory_order_relaxed);
    bool     reported = false;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        uint64_t b = g_beats.load(std::memory_order_relaxed);
        int64_t  t = now_ms();
        if (b != last_beats) {
            last_beats = b; last_change = t;
            g_last_change_ms.store(t, std::memory_order_relaxed);
            reported = false;
        }
        int stalled = (int)(t - last_change);
        write_heartbeat(stalled);
        if (stalled >= STALL_MS && !reported) {
            int phase; uint32_t pc; uint64_t cyc, frame;
            snapshot(&phase, &pc, &cyc, &frame);
            std::fprintf(stderr,
                "\n=== vb-runtime WATCHDOG: main loop STALLED %d ms in phase=%s "
                "pc=0x%08X (frame=%llu) — see vb_freeze_dump_*.json ===\n",
                stalled, vb_watchdog_phase_name(phase), pc,
                (unsigned long long)frame);
            std::fflush(stderr);
            write_freeze_dump(stalled);   /* suspends main thread, walks its stack */
            reported = true;
        }
    }
}

} // namespace

extern "C" {

void vb_watchdog_beat(int phase, uint32_t pc, uint64_t cycles, uint64_t frame) {
    g_phase.store(phase, std::memory_order_relaxed);
    g_pc.store(pc, std::memory_order_relaxed);
    g_cycles.store(cycles, std::memory_order_relaxed);
    g_frame.store(frame, std::memory_order_relaxed);
    g_beats.fetch_add(1, std::memory_order_relaxed);
}

void vb_watchdog_start(void) {
    if (g_running.exchange(true)) return;
    g_last_change_ms.store(now_ms(), std::memory_order_relaxed);
#ifdef _WIN32
    /* Duplicate the calling (main) thread's pseudo-handle into a real handle
     * so the watchdog thread can SuspendThread/StackWalk it on a freeze. */
    s_main_thread_id = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &s_main_thread,
                         THREAD_ALL_ACCESS, FALSE, 0)) {
        s_main_thread = NULL;
    }
#endif
    g_thread = std::thread(watchdog_main);
}

void vb_watchdog_stop(void) {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

void vb_watchdog_status(int* phase, uint32_t* pc, uint64_t* cycles,
                        uint64_t* frame, uint64_t* beats, int* stalled_ms) {
    if (phase)  *phase  = g_phase.load(std::memory_order_relaxed);
    if (pc)     *pc     = g_pc.load(std::memory_order_relaxed);
    if (cycles) *cycles = g_cycles.load(std::memory_order_relaxed);
    if (frame)  *frame  = g_frame.load(std::memory_order_relaxed);
    if (beats)  *beats  = g_beats.load(std::memory_order_relaxed);
    if (stalled_ms) {
        int64_t s = now_ms() - g_last_change_ms.load(std::memory_order_relaxed);
        *stalled_ms = (s < 0) ? 0 : (int)s;
    }
}

const char* vb_watchdog_phase_name(int phase) {
    switch (phase) {
        case VB_WD_INIT:     return "init";
        case VB_WD_IDLE:     return "idle";
        case VB_WD_DISPATCH: return "dispatch";
        case VB_WD_TICK:     return "tick";
        case VB_WD_PRESENT:  return "present";
        case VB_WD_POLL:     return "poll";
        default:             return "?";
    }
}

} // extern "C"
