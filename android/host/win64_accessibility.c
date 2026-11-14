/* Guest-only Windows accessibility setting storage. Initial features, shortcut
 * flags and FILTERKEYS timings are disabled/zero; Android settings are neither
 * queried nor changed. No settings persistence or system notification occurs. */
#include "win64_accessibility.h"
#include "win64.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint32_t cbSize,dwFlags;} sticky_toggle_keys;
typedef struct {uint32_t cbSize,dwFlags,iWaitMSec,iDelayMSec,iRepeatMSec,iBounceMSec;} filter_keys;
_Static_assert(sizeof(sticky_toggle_keys)==8,"Win64 STICKYKEYS/TOGGLEKEYS layout");
_Static_assert(sizeof(filter_keys)==24,"Win64 FILTERKEYS layout");
static pthread_mutex_t settings_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t settings[3][6]={{8,0,0,0,0,0},{8,0,0,0,0,0},{24,0,0,0,0,0}};
static const uint32_t flags_masks[3]={UINT32_C(0xffff01ff),UINT32_C(0x3f),UINT32_C(0x7f)};
static _Noreturn void accessibility_stop(const char *reason){
    fprintf(stderr,"STOP Win64 accessibility %s\n",reason);fflush(stderr);_Exit(73);
}
static uint32_t validate_set(unsigned kind,uint32_t ui,uint32_t flags,const uint32_t *record){
    uint32_t size=kind==2?24:8;
    if(record[0]!=size || (ui&&ui!=size) || (record[1]&~flags_masks[kind]))return 87;
    /* shortcut: only disabled accessibility setting storage is implemented;
     * actual sticky/toggle/filter input transforms need a separate provider. */
    if(flags || (record[1]&1u))return 50;
    return 0;
}
int win64_accessibility_call(CPU *cpu,const char *name){
    if(strcmp(name,"SystemParametersInfoA"))return 0;
    uint32_t action=(uint32_t)cpu->rcx,ui=(uint32_t)cpu->rdx,flags=(uint32_t)cpu->r9;
    unsigned kind;int setter;
    switch(action){
        case 0x3a:kind=0;setter=0;break;case 0x3b:kind=0;setter=1;break;
        case 0x34:kind=1;setter=0;break;case 0x35:kind=1;setter=1;break;
        case 0x32:kind=2;setter=0;break;case 0x33:kind=2;setter=1;break;
        default:return 0;
    }
    int saved_errno=errno;uint32_t size=kind==2?24:8,cb;
    memcpy(&cb,win64_pointer(cpu->r8,4,0),4);
    if(!setter){
        /* Native USER32 A getters leave cbSize intact and zero the complete
         * fixed-size body for any nonzero short cbSize or bad uiParam. Zero
         * and oversized cbSize leave the entire record unchanged. */
        if(!cb || cb>size){cpu->rax=0;errno=saved_errno;return 1;}
        if(cpu->r8&3u){win64_set_error(998);cpu->rax=0;errno=saved_errno;return 1;}
        unsigned char *out=win64_pointer(cpu->r8,size,1);uint32_t copy[6]={0};
        if(cb==size && (!ui||ui==size)){
            pthread_mutex_lock(&settings_mutex);memcpy(copy,settings[kind],size);pthread_mutex_unlock(&settings_mutex);
            memcpy(out+4,(unsigned char*)copy+4,size-4);cpu->rax=1;
        }else{memset(out+4,0,size-4);cpu->rax=0;}
    }else{
        uint32_t copy[6]={0};
        if(cb!=size){win64_set_error(87);cpu->rax=0;errno=saved_errno;return 1;}
        if(cpu->r8&3u){win64_set_error(998);cpu->rax=0;errno=saved_errno;return 1;}
        memcpy(copy,win64_pointer(cpu->r8,size,0),size);
        uint32_t error=validate_set(kind,ui,flags,copy);
        if(error){win64_set_error(error);cpu->rax=0;}else{
            pthread_mutex_lock(&settings_mutex);memcpy(settings[kind],copy,size);pthread_mutex_unlock(&settings_mutex);cpu->rax=1;
        }
    }
    errno=saved_errno;return 1;
}
void win64_accessibility_self_test(void){
    CPU cpu={0},before;cpu.rsp=123;cpu.rcx=0x9999;before=cpu;
    if(win64_accessibility_call(&cpu,"SystemParametersInfoA")||memcmp(&cpu,&before,sizeof(cpu)))accessibility_stop("unknown action changed CPU");
    if(win64_accessibility_call(&cpu,"SystemParametersInfoW")||memcmp(&cpu,&before,sizeof(cpu)))accessibility_stop("unknown API changed CPU");
    uint32_t record[6]={24,0x7e,1,2,UINT32_MAX,4};
    if(validate_set(2,24,0,record)||validate_set(2,0,0,record)||validate_set(2,23,0,record)!=87||
       validate_set(2,24,1,record)!=50)accessibility_stop("private settings validation");
    record[1]=1;if(validate_set(2,24,0,record)!=50)accessibility_stop("unsupported feature enabled");
    record[1]=0x80;if(validate_set(2,24,0,record)!=87)accessibility_stop("reserved flags accepted");
    fputs("WIN64_ACCESSIBILITY_SELF_TEST guest_only_settings=1 disabled_initial_policy=1 no_OS_setters=1 feature_enable_unsupported=1 no_rsp_pop=1 passed\n",stderr);
}
