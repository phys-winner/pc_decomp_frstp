/* Bounded English Win32 diagnostic provider and its owned LocalFree buffers. */
#include "win64_error_message.h"
#include "win64.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char unsupported_request[]="The request is not supported.\r\n";
static const char module_not_found[]="The specified module could not be found.\r\n";
_Static_assert(sizeof(module_not_found)==43,"English resource126 including terminator");
typedef struct message_allocation {uint64_t address;struct message_allocation *next;} message_allocation;
static message_allocation *allocations;
static pthread_mutex_t message_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Noreturn void message_stop(const char *reason){fprintf(stderr,"STOP Win64 error message %s\n",reason);fflush(stderr);_Exit(73);}
static uint64_t allocate_message(void){
    message_allocation *record=calloc(1,sizeof(*record));if(!record)return 0;
    uint64_t address=win64_alloc(sizeof(unsupported_request),0);if(!address){free(record);return 0;}
    memcpy(win64_pointer(address,sizeof(unsupported_request),1),unsupported_request,sizeof(unsupported_request));
    record->address=address;pthread_mutex_lock(&message_mutex);record->next=allocations;allocations=record;pthread_mutex_unlock(&message_mutex);return address;
}
static uint64_t release_message(uint64_t address){
    if(!address)return 0;
    pthread_mutex_lock(&message_mutex);message_allocation **at=&allocations;
    while(*at&&(*at)->address!=address)at=&(*at)->next;
    if(!*at){pthread_mutex_unlock(&message_mutex);win64_set_error(6);return address;}
    message_allocation *record=*at;*at=record->next;win64_free(address);free(record);pthread_mutex_unlock(&message_mutex);return 0;
}
static uint32_t format_message_w(CPU *cpu){
    uint32_t flags=(uint32_t)cpu->rcx,identifier=(uint32_t)cpu->r8,language=(uint32_t)cpu->r9;
    uint64_t destination=win64_argument(cpu,4);uint32_t capacity=(uint32_t)win64_argument(cpu,5);uint64_t arguments=win64_argument(cpu,6);
    /* shortcut: the proven direct FROM_SYSTEM English resources50/126;
     * add resources/contracts when another original caller requires them. */
    if(flags!=0x1000||cpu->rdx||arguments)message_stop("unsupported FormatMessageW flags/source/inserts");
    if(!destination){win64_set_error(87);return 0;}
    if(language!=0&&language!=0x400&&language!=0x409)message_stop("unsupported wide diagnostic language (English provider)");
    const char *message=identifier==50?unsupported_request:identifier==126?module_not_found:NULL;
    if(!message){win64_set_error(317);return 0;}
    size_t bytes=strlen(message)+1;
    uint32_t units=capacity<bytes?capacity:bytes;
    if(units){unsigned char *out=win64_pointer(destination,(size_t)units*2,1);
        for(uint32_t i=0;i<units;++i){uint16_t unit=i+1<units?(unsigned char)message[i]:0;memcpy(out+i*2,&unit,2);}}
    if(capacity<bytes){win64_set_error(122);return 0;}
    return bytes-1;
}
int win64_error_message_call(CPU *cpu,const char *name){
    if(!strcmp(name,"LocalFree")){cpu->rax=release_message(cpu->rcx);return 1;}
    if(!strcmp(name,"FormatMessageW")){cpu->rax=format_message_w(cpu);return 1;}
    if(strcmp(name,"FormatMessageA"))return 0;
    uint32_t flags=(uint32_t)cpu->rcx,identifier=(uint32_t)cpu->r8,language=(uint32_t)cpu->r9;
    uint64_t destination=win64_argument(cpu,4);uint32_t minimum=(uint32_t)win64_argument(cpu,5);uint64_t arguments=win64_argument(cpu,6);
    /* shortcut: measured original ALLOCATE_BUFFER|FROM_SYSTEM|IGNORE_INSERTS only. */
    if(flags!=0x1300||cpu->rdx||arguments)message_stop("unsupported flags/source/inserts contract");
    if(!destination){win64_set_error(8);cpu->rax=0;return 1;}
    uint64_t *out=win64_pointer(destination,8,1);uint64_t zero=0;memcpy(out,&zero,8);
    if(language!=0&&language!=0x400&&language!=0x409)message_stop("unsupported diagnostic language (English provider)");
    if(identifier!=50){win64_set_error(317);cpu->rax=0;return 1;}
    /* Native ANSI ALLOCATE output is the final 32-byte string for nSize0..4096. */
    if(minimum>4096)message_stop("untested FormatMessageA minimum allocation size");
    uint64_t address=allocate_message();if(!address){win64_set_error(8);cpu->rax=0;return 1;}
    memcpy(out,&address,8);cpu->rax=sizeof(unsupported_request)-1;return 1;
}
void win64_error_message_self_test(void){
    if(sizeof(unsupported_request)!=32||unsupported_request[29]!='\r'||unsupported_request[30]!='\n')message_stop("message resource self-test");
    uint64_t address=allocate_message();if(!address||memcmp(win64_pointer(address,32,0),unsupported_request,32)||release_message(address)||release_message(0))message_stop("owned allocation self-test");
    uint64_t stack=win64_alloc(64,1),out=win64_alloc(64,1);if(!stack||!out)message_stop("wide self-test allocation");
    uint64_t arguments[]={out,32,0};memcpy(win64_pointer(stack+40,sizeof(arguments),1),arguments,sizeof(arguments));
    CPU cpu={0};cpu.rsp=stack;cpu.rcx=0x1000;cpu.r8=50;uint32_t saved_error=win64_get_error();
    if(!win64_error_message_call(&cpu,"FormatMessageW")||cpu.rax!=31||cpu.rsp!=stack)message_stop("wide direct-buffer self-test");
    for(unsigned i=0;i<32;++i){uint16_t unit;memcpy(&unit,win64_pointer(out+i*2,2,0),2);if(unit!=(unsigned char)unsupported_request[i])message_stop("wide UTF16 resource self-test");}
    win64_free(out);win64_free(stack);win64_set_error(saved_error);
}
