/* Include after initialize_static_tls in bootstrap.c. Original PE data remains
 * NX; each guest entry and TLS callback goes through the static dispatcher.
 * guest_peb/process_parameters are process-shared, while TEB, static_tls_vector,
 * stack_lo/hi and the dispatcher/EH depths remain native thread-local.
 */
#include "win64_threads.h"
#include <pthread.h>

extern void es3_eh_thread_reset(void);
extern void win64_thread_crt_attach(void);
extern void win64_thread_crt_detach(void);
static pthread_mutex_t bootstrap_loader_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t image,first,bytes,callbacks;size_t image_bytes,zero;
    int installed,tls;
} bootstrap_thread_image;
static _Thread_local int bootstrap_thread_teb_registered;
static _Thread_local int bootstrap_thread_vector_registered;
static _Thread_local int bootstrap_thread_attach_complete;
static _Thread_local int bootstrap_thread_dependency_attached;
static _Thread_local int bootstrap_thread_loader_locked;

static void *bootstrap_thread_teb(void){return guest_teb;}
static void bootstrap_thread_callbacks(CPU *cpu,unsigned reason) {
    uint64_t list=bootstrap_thread_image.callbacks;if(!list)return;
    for(unsigned i=0;i<64;++i) {
        uint64_t item=list+(uint64_t)i*8;
        if(item<bootstrap_thread_image.image || !fits(item-bootstrap_thread_image.image,8,bootstrap_thread_image.image_bytes))
            stop("thread TLS callback entry bounds");
        uint64_t target=rd64(item);if(!target)return;
        if(target<bootstrap_thread_image.image || target-bootstrap_thread_image.image>=bootstrap_thread_image.image_bytes)
            stop("thread TLS callback target bounds");
        CPU saved=*cpu;cpu->rsp=(stack_hi-64)&~UINT64_C(15);uint64_t before=cpu->rsp;
        cpu->rcx=bootstrap_thread_image.image;cpu->rdx=reason;cpu->r8=0;
        push64(cpu,UINT64_C(0xe5ef0020)+reason*16);
        fprintf(stderr,"TLS_CALLBACK thread_reason=%u target=0x%" PRIx64 "\n",reason,target);
        dispatch(cpu,target);
        if(cpu->rsp!=before)stop("thread TLS callback stack imbalance");
        *cpu=saved;
    }
    stop("unterminated thread TLS callback list");
}
static int bootstrap_thread_attach(CPU *cpu,uint64_t low,size_t bytes) {
    if(!bootstrap_thread_image.image)return 0;
    stack_lo=low;stack_hi=low+bytes;es3_eh_thread_reset();
    memset(cpu,0,sizeof(*cpu));cpu->rsp=(stack_hi-64)&~UINT64_C(15);cpu->mxcsr=0x1f80;
    /* prepare_tls ran first. Preserve its GS dynamic slots and expansion vector. */
    frostpunk_teb_write(0x08,stack_hi,8);frostpunk_teb_write(0x10,stack_lo,8);
    frostpunk_teb_write(0x30,(uintptr_t)guest_teb,8);
    frostpunk_teb_write(0x40,(uint32_t)getpid(),8);frostpunk_teb_write(0x48,(uint32_t)gettid(),8);
    frostpunk_teb_write(0x60,(uintptr_t)guest_peb,8);win64_set_error(0);
    win64_thread_crt_attach();
    win64_region_add((uintptr_t)guest_teb,sizeof(guest_teb),1);bootstrap_thread_teb_registered=1;
    if(bootstrap_thread_image.tls) {
        size_t total=(size_t)bootstrap_thread_image.bytes+bootstrap_thread_image.zero;
        uint64_t storage=win64_alloc(total?total:1,1);if(!storage)stop("thread static TLS allocation");
        memcpy((void *)(uintptr_t)storage,(void *)(uintptr_t)bootstrap_thread_image.first,(size_t)bootstrap_thread_image.bytes);
        memset(static_tls_vector,0,sizeof(static_tls_vector));static_tls_vector[0]=storage;
        frostpunk_teb_write(0x58,(uintptr_t)static_tls_vector,8);
        win64_region_add((uintptr_t)static_tls_vector,sizeof(static_tls_vector),1);bootstrap_thread_vector_registered=1;
    } else frostpunk_teb_write(0x58,0,8);
    pthread_mutex_lock(&bootstrap_loader_mutex);bootstrap_thread_loader_locked=1;
    frostpunk_dependencies_thread_notify(cpu,2);bootstrap_thread_dependency_attached=1;
    bootstrap_thread_callbacks(cpu,2);bootstrap_thread_attach_complete=1;
    bootstrap_thread_loader_locked=0;pthread_mutex_unlock(&bootstrap_loader_mutex);return 1;
}
static void bootstrap_thread_detach(CPU *cpu) {
    /* ExitThread longjmp may cross static dispatch and a loader callback. Clear
     * those native C bookkeeping chains before using a fresh cleanup context. */
    es3_eh_thread_reset();
    if(bootstrap_thread_loader_locked){bootstrap_thread_loader_locked=0;pthread_mutex_unlock(&bootstrap_loader_mutex);}
    if(bootstrap_thread_attach_complete||bootstrap_thread_dependency_attached) {
        pthread_mutex_lock(&bootstrap_loader_mutex);bootstrap_thread_loader_locked=1;
        if(bootstrap_thread_attach_complete){bootstrap_thread_attach_complete=0;bootstrap_thread_callbacks(cpu,3);}
        if(bootstrap_thread_dependency_attached){bootstrap_thread_dependency_attached=0;frostpunk_dependencies_thread_notify(cpu,3);}
        bootstrap_thread_loader_locked=0;pthread_mutex_unlock(&bootstrap_loader_mutex);
    }
    if(bootstrap_thread_vector_registered) {
        uint64_t storage=static_tls_vector[0];win64_region_remove((uintptr_t)static_tls_vector);
        bootstrap_thread_vector_registered=0;memset(static_tls_vector,0,sizeof(static_tls_vector));win64_free(storage);
    }
    if(bootstrap_thread_teb_registered){win64_region_remove((uintptr_t)guest_teb);bootstrap_thread_teb_registered=0;}
    win64_thread_crt_detach();
}
static uint64_t bootstrap_thread_call(CPU *cpu,uint64_t target,uint64_t parameter) {
    cpu->rsp=(stack_hi-64)&~UINT64_C(15);uint64_t before=cpu->rsp;
    cpu->rcx=parameter;push64(cpu,UINT64_C(0xe5ef0080));dispatch(cpu,target);
    if(cpu->rsp!=before)stop("guest thread entry stack imbalance");return cpu->rax;
}
static void bootstrap_threads_install(const pe_t *pe,unsigned char *image) {
    if(bootstrap_thread_image.installed)stop("bootstrap thread hooks installed twice");
    bootstrap_thread_image.installed=1;size_t stack=STACK_BYTES;
    if(pe) {
        bootstrap_thread_image.image=pe->base;bootstrap_thread_image.image_bytes=pe->image_size;
        uint64_t declared=u64(pe->bytes+pe->directory_table-112+72);
        if(declared>64*1024*1024)stop("thread default stack reserve unsupported");if(declared)stack=(size_t)declared;
        if(pe->directories>=10) {
            uint32_t rva=u32(pe->bytes+pe->directory_table+9*8),length=u32(pe->bytes+pe->directory_table+9*8+4);
            if(rva||length) {
                if(length<40||!fits(rva,40,pe->image_size))stop("thread static TLS directory bounds");
                const unsigned char *tls=image+rva;uint64_t first=u64(tls),last=u64(tls+8),index=u64(tls+16),list=u64(tls+24);
                uint32_t zero=u32(tls+32),alignment=(u32(tls+36)>>20)&15;
                if(first<pe->base||last<first||last-pe->base>pe->image_size||index<pe->base||!fits(index-pe->base,4,pe->image_size)||
                   last-first>16*1024*1024||zero>16*1024*1024||alignment>5)stop("thread static TLS geometry");
                if(list&&(list<pe->base||!fits(list-pe->base,8,pe->image_size)))stop("thread TLS callback list bounds");
                bootstrap_thread_image.first=first;bootstrap_thread_image.bytes=last-first;bootstrap_thread_image.zero=zero;
                bootstrap_thread_image.callbacks=list;bootstrap_thread_image.tls=1;
            }
        }
    }
    win64_thread_hooks hooks={bootstrap_thread_attach,bootstrap_thread_detach,bootstrap_thread_call,bootstrap_thread_teb};
    win64_threads_configure(&hooks,stack);
}
/* Root calls only after actual PROCESS_ATTACH, never merely --run-crt. This
 * adopts the existing main TEB/static TLS and opens the worker start gate. */
static __attribute__((unused)) void bootstrap_threads_process_attached(CPU *cpu) {
    bootstrap_thread_teb_registered=1;
    bootstrap_thread_vector_registered=bootstrap_thread_image.tls;
    bootstrap_thread_attach_complete=1;
    bootstrap_thread_dependency_attached=1;
    win64_threads_adopt_current(cpu);win64_threads_process_ready();
}
