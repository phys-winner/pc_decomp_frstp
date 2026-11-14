/* The host self-test exercises its own synthetic NX image, never a game body. */
#include "recomp_dispatch.h"
#include "recomp_supplemental.h"
const recomp_entry_t recomp_dispatch_table[]={{0,0}};
const uint32_t recomp_dispatch_count=0;
const hle_import_t recomp_imports[HLE_COUNT]={{0}};
frostpunk_supplemental_fn frostpunk_supplemental_lookup(uint64_t target) {
    (void)target;return 0;
}
