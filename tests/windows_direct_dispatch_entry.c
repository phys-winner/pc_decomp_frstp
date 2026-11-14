/* Separate TU: retain actual Win64 ABI calls into the tested host draft. */
#include "../build/windows-direct-dispatch-candidate/generated-overlay/aot_dispatch.h"
__declspec(noinline) uint64_t test_main_GVA_probe(uint64_t value) {return GVA(value);}
void dispatch(CPU *c,uint64_t target);
__declspec(noinline) void test_direct_entry(CPU *c,uint64_t target,void (*linked)(CPU *)) {
    fp_dispatch_main(c,target,linked);
}
__declspec(noinline) void test_normal_entry(CPU *c,uint64_t target) {
    dispatch(c,target);
}
