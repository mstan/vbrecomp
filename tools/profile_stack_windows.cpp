/* Optional x64 Windows caller-stack helper for profile_native_windows.py.
 * Build outside the runtime: g++ -shared -O2 -static-libgcc -static-libstdc++
 *   tools/profile_stack_windows.cpp -ldbghelp -o profile_stack_windows.dll
 * DbgHelp is used only by the profiler process. The caller owns suspension
 * and always resumes its child in a finally block. No code is injected.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdint>

#if !defined(_WIN64) || !defined(__x86_64__)
#error This optional profiler helper requires Windows x64.
#endif

extern "C" __declspec(dllexport) int vb_profile_begin(HANDLE process) {
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    /* Empty local search path: do not fetch symbols over the network. */
    return SymInitialize(process, "", TRUE) != FALSE;
}
extern "C" __declspec(dllexport) void vb_profile_refresh(HANDLE process) {
    SymRefreshModuleList(process);
}
extern "C" __declspec(dllexport) void vb_profile_end(HANDLE process) {
    SymCleanup(process);
}
extern "C" __declspec(dllexport) unsigned vb_profile_walk(
    HANDLE process, HANDLE thread, const CONTEXT* captured,
    uint64_t* addresses, unsigned capacity) {
    if (!captured || !addresses || !capacity) return 0;
    CONTEXT context = *captured;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
    unsigned count = 0;
    addresses[count++] = context.Rip;
    DWORD64 last_stack = 0;
    for (unsigned tries = 0; count < capacity && tries < capacity; ++tries) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                         &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset) break;
        if (frame.AddrPC.Offset == addresses[count - 1] &&
            frame.AddrStack.Offset == last_stack) break;
        /* StackWalk64 can first repeat the captured instruction. */
        if (count == 1 && frame.AddrPC.Offset == addresses[0] && !last_stack) {
            last_stack = frame.AddrStack.Offset;
            continue;
        }
        addresses[count++] = frame.AddrPC.Offset;
        last_stack = frame.AddrStack.Offset;
    }
    return count;
}
