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

PGO remains unevaluated and must not be presented as tested by this control.
