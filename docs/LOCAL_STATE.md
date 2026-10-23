# Local execution snapshots and rbengine history

This work uses the standalone snapshot-ring backend from the pinned
`lib/retcomm-rbengine` submodule. It does not link recomp-net, open a transport,
provide shared-controller play, or emulate a Virtual Boy link cable. The owner
explicitly deferred netplay on 2026-10-22. Local replay is not network testing.

Snapshots are disposable and specific to the engine source, generated code,
compiler/options, ABI and cartridge content. No old-state migration is intended.
Native SRAM remains a separate persistence contract. Before a clean exit after
restoring, resetting or replaying execution history, an existing native save is
copied to a unique `.pre-state-<timestamp>.bak` beside it. Failure to preserve
that file prevents replacement. Native writes and snapshot-file writes use a
temporary file and atomic replacement.

## Controls

`--state-file PATH` selects the quick snapshot file (default `quick.state` in
the working directory). F5 captures it; F8 restores it. `--rewind-frames N`
enables a local history of 2..512 frame slots, subject to a 512 MiB memory
limit. Hold Backspace to step backward and release it to resume. Rewind is off
by default. Captures and restores occur between guest dispatches; they never
serialize a host call stack or a bus function pointer.

The development TCP interface supports `save_state` / `load_state` with a
`path`, `rewind_enable` with a frame count in `value`, `rewind` with the target
guest frame in `value`, `reset`, `replay_frames` with `frames=1..10000`, and
`state_digest`. Mutating lifecycle commands
require a paused guest. Reset preserves live cartridge SRAM. Qualification
always supplies `--no-save` and isolated config/mod/snapshot paths, protecting
the owner's actual native saves.

## Captured state

The CPU's architectural and execution residue, dispatch resume PC and frame
clock are included, together with WRAM/SRAM, bus synchronization clocks,
interrupt lines, timer divider/reload latches, controller transfer/press state,
VIP memory/registers/palette caches/scan and drawing counters/framebuffer
ownership, and VSU channel/divider/noise state. Blip phase, filter accumulator,
complete impulse buffers and last per-channel amplitudes are retained; its
configuration and coefficient kernels are immutable. Pointers are retained from
the live runtime and never read from a file.

Presentation snapshots retain captured per-eye attribution/source buffers and
adaptive replay blocks when their tracking features are enabled. Row scratch
that is completely overwritten on the next drawing block is derived. Live
window geometry, UI settings and host output queues stay local. Successful
restore drops queued audio from the abandoned future; subsequent emulated
audio is regenerated. PCM capture advertises that older output before the
restore point is no longer resident.

The canonical simulation digest excludes host presentation history, pointers,
diagnostic rings and audio playback cursors. File input is length/build/content
and CRC checked before mutation, then device/scheduler ranges are validated
transactionally with restoration of the previous state on failure.

Private rewind entries are immutable allocations owned by rbengine's ring.
They use a distinct internal flag and omit the file CRC pass; the public
file/blob loader always rejects that flag. Direct snapshot writing counts the
layout once and writes into one allocation. It avoids vector growth and a
second complete copy without changing the captured state. Read-only capture
does not lock the SDL audio consumer, which owns only the excluded playback
cursor. Restores and direct replay lock audio while changing simulation state.

Custom renderers register pointer-free mutable presentation spans before host
binding, with versioned IDs. Their IDs/sizes and renderer/viewport identity gate
loads; canonical simulation digests exclude these host presentation bytes.
Consumers call `vb_state_add_identity(target sources...)` after adding custom
sources so their implementation participates in the build identity. Zero
Racers uses this for edge/line material provenance. Loaded files do not supply
callbacks, allocations, pointers, or plugin configuration.

## Qualification

`tools/qualify_lifecycle.py --runtime ... --rom ... --route
<existing-race-driving-route.json> --out <non-C evidence directory>` uses the
existing controller route without guest-success flags. It snapshots at frame
2600, compares main execution and replay through frame4000, and checks actual
CPU/device/memory/both-eye/presented bytes and stereo PCM. It exercises direct
WRAM/SRAM, aliased character RAM and both eyes in both framebuffer slots;
an instruction-stepped mid-frame restore/replay; repeated
presentations; malformed files, wrong build/content and valid-CRC invalid CPU
state; rewind branching; and reset with SRAM retained. The independent Beetle
route remains the hardware reference. Passing local replay does not establish
netplay, link-cable or cross-build compatibility.

`--sources` additionally compares both eyes' native and presented source data.
`--mod-archive` and `--enable-mod package:feature` exercise an installed mod in
the isolated profile. `replay_frames` is a paused diagnostic operation using
the same frame scheduler, not a rollback session or network protocol.
