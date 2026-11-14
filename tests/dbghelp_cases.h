#include <stdint.h>
static const uint64_t dbghelp_handles[]={0,UINT64_MAX,1,2,UINT64_C(0x100000000),UINT64_C(0x1234567812345678)};
static uint64_t dbghelp_hash(uint64_t hash,uint32_t result,uint32_t error) {
    hash=(hash^result)*UINT64_C(1099511628211);return(hash^error)*UINT64_C(1099511628211);
}
