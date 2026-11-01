/* Include the shipping body unchanged; private seeding exists only in tests. */
#include "../src/v810_interpreter.c"
void vb_test_coverage_clear(void) {
    memset(fallback_coverage,0,sizeof(fallback_coverage));fallback_coverage_overflow=0;
#if VB_INTERPRETER_BATCH
    coverage_cache_clear();
#endif
}
void vb_test_coverage_seed(unsigned slot,uint64_t hits) { fallback_coverage[slot].hits=hits; }
void vb_test_coverage_seed_overflow(uint64_t hits) { fallback_coverage_overflow=hits; }
