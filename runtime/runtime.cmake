# runtime.cmake — vb_add_runtime_target()
#
# Aggregates the runtime sources and emits one executable. Modelled on
# psxrecomp/psxrecomp/runtime/runtime.cmake.
#
# Usage:
#   vb_add_runtime_target(target_name
#       DEBUG_PORT <int>
#       WINDOW_TITLE <string>
#       [NO_GAME_LINKED]        # link the no_game_linked.c placeholder
#       [GENERATED_SOURCES ...] # full / dispatch C from the recompiler
#   )

set(VB_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(vb_add_runtime_target TARGET)
    set(options NO_GAME_LINKED)
    set(oneValue DEBUG_PORT WINDOW_TITLE GENERATED_DIR GENERATED_MODULE)
    set(multiValue GENERATED_SOURCES)
    cmake_parse_arguments(VB "${options}" "${oneValue}" "${multiValue}" ${ARGN})

    if(NOT VB_DEBUG_PORT)
        set(VB_DEBUG_PORT 4390)
    endif()
    if(NOT VB_WINDOW_TITLE)
        set(VB_WINDOW_TITLE "vbrecomp")
    endif()

    set(_runtime_sources
        ${VB_RUNTIME_DIR}/src/main.cpp
        ${VB_RUNTIME_DIR}/src/memory.c
        ${VB_RUNTIME_DIR}/src/vip.c
        # VIP draw-timing phase event ring (Axis-5a phase gate; always-on,
        # observability-only — does not touch emulation).
        ${VB_RUNTIME_DIR}/src/vip_phase.c
        # Per-game-frame WRAM fingerprint ring (Axis-6 whole-session fidelity).
        ${VB_RUNTIME_DIR}/src/wram_hash.c
        # Runtime graphics-capture + override experiment (opt-in, default OFF;
        # byte-identical with VBRECOMP_CAPTURE / VBRECOMP_OVERRIDES unset).
        ${VB_RUNTIME_DIR}/src/vip_capture.c
        ${VB_RUNTIME_DIR}/src/asset_pack.c
        ${VB_RUNTIME_DIR}/src/png_read.c
        ${VB_RUNTIME_DIR}/src/recolor.c
        ${VB_RUNTIME_DIR}/src/vsu.c
        # Vendored band-limited synthesis buffer (Mednafen Blip_Buffer 0.4.1,
        # from beetle-vb/mednafen). Used by the VSU output stage (Axis-5b).
        ${VB_RUNTIME_DIR}/src/Blip_Buffer.c
        # Verified-enhancement shadow QoL layer (opt-in, default OFF;
        # byte-identical with VBRECOMP_AUDIO_SHADOW / VBRECOMP_SCREEN unset).
        ${VB_RUNTIME_DIR}/src/audio_shadow.c
        ${VB_RUNTIME_DIR}/src/vsu_shadow.c
        ${VB_RUNTIME_DIR}/src/red_lut.c
        ${VB_RUNTIME_DIR}/src/input.c
        ${VB_RUNTIME_DIR}/src/interrupts.c
        ${VB_RUNTIME_DIR}/src/timer.c
        ${VB_RUNTIME_DIR}/src/png_write.c
        ${VB_RUNTIME_DIR}/src/ring_frame.c
        ${VB_RUNTIME_DIR}/src/stub_abort.c
        ${VB_RUNTIME_DIR}/src/watchdog.cpp
    )

    # Prod-vs-debug TCP strip. VBRECOMP_DEBUG_TOOLS (option in vbrecomp/CMakeLists.txt,
    # default ON) keeps the always-on dev tooling: the TCP debug server, the 1M-entry
    # write-trace ring (wtrace), and the 256K function-entry ring (fntrace). OFF
    # (release: tools/build-linux.sh --config prod) compiles vb_trace_stub.c instead —
    # no-op definitions of the same symbols — so the runtime links but opens no port
    # and carries no rings.
    if(VBRECOMP_DEBUG_TOOLS)
        list(APPEND _runtime_sources
            ${VB_RUNTIME_DIR}/src/debug_server.c
            ${VB_RUNTIME_DIR}/src/wtrace.c
            ${VB_RUNTIME_DIR}/src/fntrace.c
            ${VB_RUNTIME_DIR}/src/cpuhook.c)
    else()
        list(APPEND _runtime_sources ${VB_RUNTIME_DIR}/src/vb_trace_stub.c)
    endif()

    # Pick exactly one source of dispatch — never both. The build
    # fails loudly if a caller asks for both NO_GAME_LINKED and a
    # generated module so the choice is explicit at site rather than
    # masked by include order.
    set(_dispatch_count 0)
    if(VB_NO_GAME_LINKED)
        math(EXPR _dispatch_count "${_dispatch_count} + 1")
    endif()
    if(VB_GENERATED_DIR AND VB_GENERATED_MODULE)
        math(EXPR _dispatch_count "${_dispatch_count} + 1")
    endif()
    if(VB_GENERATED_SOURCES)
        math(EXPR _dispatch_count "${_dispatch_count} + 1")
    endif()
    if(_dispatch_count GREATER 1)
        message(FATAL_ERROR
            "vb_add_runtime_target(${TARGET}): more than one dispatch "
            "source given (NO_GAME_LINKED / GENERATED_DIR+MODULE / "
            "GENERATED_SOURCES are mutually exclusive)")
    endif()

    if(VB_NO_GAME_LINKED)
        list(APPEND _runtime_sources ${VB_RUNTIME_DIR}/src/no_game_linked.c)
    endif()

    set(_generated_include "")
    if(VB_GENERATED_DIR AND VB_GENERATED_MODULE)
        set(_full     "${VB_GENERATED_DIR}/${VB_GENERATED_MODULE}_full.c")
        set(_dispatch "${VB_GENERATED_DIR}/${VB_GENERATED_MODULE}_dispatch.c")
        set(_header   "${VB_GENERATED_DIR}/${VB_GENERATED_MODULE}.h")
        foreach(_f IN ITEMS "${_full}" "${_dispatch}" "${_header}")
            if(NOT EXISTS "${_f}")
                message(FATAL_ERROR
                    "vb_add_runtime_target(${TARGET}): generated file "
                    "missing: ${_f}. Run "
                    "`python -m recompiler.cli.vbrecomp_codegen "
                    "--rom <rom> --module ${VB_GENERATED_MODULE} "
                    "--out ${VB_GENERATED_DIR}` first.")
            endif()
        endforeach()
        list(APPEND _runtime_sources "${_full}" "${_dispatch}")
        set(_generated_include "${VB_GENERATED_DIR}")
    endif()

    if(VB_GENERATED_SOURCES)
        list(APPEND _runtime_sources ${VB_GENERATED_SOURCES})
    endif()

    add_executable(${TARGET} ${_runtime_sources})

    target_include_directories(${TARGET} PRIVATE
        ${VB_RUNTIME_DIR}/include
    )
    if(_generated_include)
        target_include_directories(${TARGET} PRIVATE ${_generated_include})
    endif()

    target_compile_definitions(${TARGET} PRIVATE
        VB_DEFAULT_DEBUG_PORT=${VB_DEBUG_PORT}
        VB_DEFAULT_WINDOW_TITLE="${VB_WINDOW_TITLE}"
    )

    if(VBRECOMP_DEBUG_TOOLS)
        target_compile_definitions(${TARGET} PRIVATE VBRECOMP_DEBUG_TOOLS=1)
        # Opt-in per-instruction CPU-hook ring (cpuhook.c, default OFF).
        # Enables the generated VB_CPUHOOK(cpu) calls for the recomp-vs-
        # oracle divergence/cycle harness. Requires the debug tooling
        # (cpuhook.c is only compiled in this block). Default build is
        # byte-identical in behavior with VBRECOMP_CPUHOOK unset.
        if(VBRECOMP_CPUHOOK)
            target_compile_definitions(${TARGET} PRIVATE VB_CPUHOOK_ENABLE=1)
        endif()
    endif()

    # Warnings — match psxrecomp's discipline.
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    # Winsock on Windows; dbghelp lets the watchdog symbolize the main thread's
    # stack on a freeze (StackWalk64 / SymFromAddr). Game-controller support is
    # cross-platform via SDL_GameController (see main.cpp), so no XInput link.
    if(WIN32)
        target_link_libraries(${TARGET} PRIVATE ws2_32 dbghelp)
        # When building with MinGW (the project's primary toolchain),
        # statically link the gcc/stdc++/winpthread runtimes so the
        # release zip ships just vb-runtime.exe + SDL2.dll without
        # the libgcc_s_seh-1 / libstdc++-6 / libwinpthread-1 DLLs.
        if(MINGW)
            target_link_options(${TARGET} PRIVATE
                -static-libgcc -static-libstdc++
                -Wl,-Bstatic,--whole-archive -lwinpthread
                -Wl,--no-whole-archive)
        endif()
    endif()
    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(${TARGET} PRIVATE Threads::Threads)
    endif()

    # SDL2 for the live window. If unavailable, compile in --headless-only
    # mode so the runtime still builds for TCP-only setups.
    # For MSVC release builds we ship a vendored SDL2 dev pack at
    # runtime/external/SDL2/; on MSYS2/mingw local builds the system
    # find_package finds the toolchain's SDL2 directly. On macOS/Linux use
    # the system/Homebrew SDL2 only — the vendored pack is Windows-shaped
    # (no .dylib/.a) and would shadow the real one.
    if(WIN32)
        list(APPEND CMAKE_PREFIX_PATH "${VB_RUNTIME_DIR}/external/SDL2/cmake")
    endif()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        target_compile_definitions(${TARGET} PRIVATE VB_RUNTIME_HAVE_SDL=1)
        target_include_directories(${TARGET} PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PRIVATE ${SDL2_LIBRARIES})
        message(STATUS "${TARGET}: SDL2 ${SDL2_VERSION_STRING} — live window enabled")
    else()
        message(STATUS "${TARGET}: SDL2 not found — TCP-only build")
    endif()
endfunction()
