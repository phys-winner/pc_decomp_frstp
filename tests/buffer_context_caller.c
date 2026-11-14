/* A separate native DSO gives the failure trace a real module boundary. */
#include <stddef.h>
#include <stdint.h>
typedef void *(*checked_pointer)(uint64_t,size_t,int);
static volatile unsigned completed;
__attribute__((noinline,visibility("default")))
void *buffer_context_dso_entry(checked_pointer pointer,uint64_t address,size_t bytes,int writing,unsigned depth) {
    void *result=depth?buffer_context_dso_entry(pointer,address,bytes,writing,depth-1):pointer(address,bytes,writing);
    ++completed;
    return result;
}
