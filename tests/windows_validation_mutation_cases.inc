/* Included after the exact selected runtime, only in the validation test TU. */
#include <assert.h>
static _Thread_local unsigned fp_test_mutation_checked;
static uint64_t fp_test_mutation_random(uint64_t *s){*s^=*s<<13;*s^=*s>>7;return *s^=*s<<17;}
static void fp_test_mutation_check(uint64_t target) {
    int expected=0;for(unsigned i=0;i<mutation_count;++i)if(mutation_targets[i]==target){expected=1;break;}
    DWORD saved=GetLastError();unsigned csr=_mm_getcsr();SetLastError(0xe5ef1234);
    assert(fp_native_memory_mutation(target)==expected && GetLastError()==0xe5ef1234 && _mm_getcsr()==csr);
    SetLastError(saved);++fp_test_mutation_checked;
}
static void fp_test_mutation_fill(unsigned count,uint64_t base) {
    memset(mutation_targets,255,sizeof(mutation_targets));mutation_count=count;
    for(unsigned i=0;i<count;++i)mutation_targets[i]=base+(i/3)*32;
}
static DWORD WINAPI fp_test_mutation_thread(void *arg) {
    uint64_t state=(uintptr_t)arg+1;unsigned csr=_mm_getcsr();_mm_setcsr(0x5fc3);
    for(unsigned i=0;i<4096;++i)fp_test_mutation_check(mutation_targets[fp_test_mutation_random(&state)%mutation_count]+(i&1));
    assert(fp_test_mutation_checked==4096);_mm_setcsr(csr);return 0;
}
unsigned fp_test_mutation_cases(void) {
    uint64_t saved[128];memcpy(saved,mutation_targets,sizeof(saved));unsigned saved_count=mutation_count,csr=_mm_getcsr();
    _mm_setcsr(0x5fc3);
    for(unsigned n=0;n<=128;++n){fp_test_mutation_fill(n,0x100000);for(unsigned i=0;i<n;++i){fp_test_mutation_check(mutation_targets[i]);fp_test_mutation_check(mutation_targets[i]+1);}fp_test_mutation_check(0);fp_test_mutation_check(UINT64_MAX);}
    uint64_t state=UINT64_C(0x8394a57bef001c29);
    for(unsigned turn=0;turn<512;++turn){fp_test_mutation_fill((unsigned)(fp_test_mutation_random(&state)%129),fp_test_mutation_random(&state)&UINT64_C(0xffff00000000));for(unsigned q=0;q<8;++q)fp_test_mutation_check(mutation_count?mutation_targets[fp_test_mutation_random(&state)%mutation_count]+(q&1):fp_test_mutation_random(&state));}
    mutation_count=2;mutation_targets[0]=0;mutation_targets[1]=UINT64_MAX;
    fp_test_mutation_check(0);fp_test_mutation_check(1);fp_test_mutation_check(UINT64_MAX);
    mutation_count=8;for(unsigned i=0;i<8;++i)mutation_targets[i]=i*32;
    fp_test_mutation_check(128);mutation_targets[3]=128;fp_test_mutation_check(128);
    fp_test_mutation_check(129);mutation_targets[5]=129;fp_test_mutation_check(129);
    mutation_count=128;for(unsigned i=0;i<128;++i)mutation_targets[i]=UINT64_C(0x7ffb00000000)+i*16;
    for(unsigned i=0;i<128;++i)fp_test_mutation_check(mutation_targets[(i&1)*64]);
    uint64_t stale=mutation_targets[127];fp_test_mutation_check(stale);mutation_count=0;fp_test_mutation_check(stale);
    mutation_count=1;fp_test_mutation_check(stale);mutation_count=128;fp_test_mutation_check(stale);
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,fp_test_mutation_thread,(void *)(uintptr_t)i,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i){DWORD code;assert(GetExitCodeThread(threads[i],&code)&&!code);assert(CloseHandle(threads[i]));}
    memcpy(mutation_targets,saved,sizeof(saved));mutation_count=saved_count;_mm_setcsr(csr);
    assert(fp_test_mutation_checked==21005);return fp_test_mutation_checked;
}
