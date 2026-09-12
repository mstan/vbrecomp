# SD Gundam framework integration

The SD Gundam game pins the shared runtime that adds captured VIP viewport
presentation. The stack over master `137ae3b` also contains the previously
validated Zero Racers hybrid execution/provenance/controller changes, title
region metadata, indirect callback discovery and guarded reversible ROM data
plans. It fast-forwards master; it does not replace another integration branch.

Validation before publication on 2026-09-12:

- Python recompiler suite: 85 tests run, 80 passed and five optional tests
  skipped. No failures.
- Runtime CTest: all six tests passed, including interpreter transitions,
  controllers, guarded data plans and both-eye VIP source/replay checks.
- SD Gundam: Japanese and English oracle evidence in the game repository;
  the final viewport runtime matches the committed native baseline across
  nine planes at 25 checkpoints through frame 34,790, with zero fallback or
  interpreter instructions. See `VIEWPORT.md` and `viewport-validation.json`.
- Real SDL launcher exposes English, reference color and adaptive widescreen;
  window/stereo, controller input, pause and exact battery-save publication
  are covered by the game's recorded validation.
- Reachable blob audit found no credential signatures or tracked ROMs, saves,
  generated cartridge code or private analysis databases. Local artifact and
  credential ignore rules were expanded before publishing.

The game repository is `mstan/SDGundamDimensionWarVirtualBoyRecomp`, following
the pinned-submodule layout of `mstan/ZeroRacersVirtualBoyRecomp`.
Central integration issue: `beads-vb.1.7`.
