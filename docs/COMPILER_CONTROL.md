# Built-runtime compiler control

VB08 is an experimental comparison, separate from architectural packages.
`VBRECOMP_LTO` defaults off. When enabled, CMake first checks C and C++ IPO
support. Unsupported toolchains retain the ordinary portable build. Only the
runtime executable receives IPO; unrelated test targets and dependencies keep
their normal configuration. GNU links use one LTO backend worker so other
campaigns are not displaced by automatic all-core linking.

The actual IPO selection enters the strict build identity. No fast-math,
host-specific instruction set or guest-semantics relaxation is enabled. Source,
compiler, settings and binary hashes must accompany the matched game results.
Compilation or native coverage is not a runtime performance result. No LTO gain
or cross-build simulation compatibility is claimed before qualification.

`VBRECOMP_PGO_MODE=GENERATE` instruments the runtime with GCC11+ atomic counters.
It defaults to `NONE`; unsupported C/C++ toolchains retain the ordinary build.
The default relative `VBRECOMP_PGO_DATA=profile-data` writes inside the launched
process's working directory, including on a training host. Keep one isolated
training workload and retain its exact source, compiler, generated inputs,
features, commands and raw files. Relative object output paths avoid a MinGW
drive-letter path-construction failure found by the compiler probe.

Copy the complete trained folder to the same build tree's `profile-data` and
configure `USE` without changing sources, generated input or build path. Use
mode checks for data, rejects missing per-unit profiles, and includes sorted
profile filenames/content hashes in strict state/build identity. Do not delete
or edit trained data while building. No cross-build simulation compatibility is
inferred. `-fprofile-partial-training` leaves unexecuted functions optimized
normally instead of aggressively optimizing all unseen functions for size.
GCC's normal profile mismatch errors remain enabled; no fast-math, machine ISA
specialization or profile-correction relaxation is introduced.

References: [GCC15.2 instrumentation](https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gcc/Instrumentation-Options.html)
and [optimization options](https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gcc/Optimize-Options.html).
Compiler mechanism probes pass; game PGO performance remains unqualified.
