/* Explicit guest C environment. Android's environ/getenv are never consulted. */
#include "win64_environment.h"
#include "win64.h"
#include "win64_stdio.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_COUNT 64u
#define ENV_NAME 255u
#define ENV_POOL 32768u
#define CRT_MAX_ENV 32767u
typedef struct {char name[ENV_NAME+1];size_t narrow,wide;} environment_variable;
typedef struct {
    environment_variable variables[ENV_COUNT];size_t count,narrow_used,wide_used;
    char narrow[ENV_POOL];uint16_t wide[ENV_POOL];
} environment_snapshot;
static environment_snapshot environment;
static pthread_mutex_t environment_mutex=PTHREAD_MUTEX_INITIALIZER;
static int configured,published,registered;
static _Noreturn void environment_stop(const char *reason) {
    fprintf(stderr,"STOP Win64 environment %s\n",reason);fflush(stderr);_Exit(73);
}
static unsigned fold(unsigned c){return c>='A'&&c<='Z'?c+('a'-'A'):c;}
static int same_name(const char *a,const char *b) {
    while(*a&&*b){if(fold((unsigned char)*a++)!=fold((unsigned char)*b++))return 0;}
    return *a==*b;
}
static size_t bounded_length(const char *text,size_t bound) {
    size_t n=0;while(n<bound&&text[n])++n;return n;
}
static int append_wide(environment_snapshot *out,const char *value,size_t bytes) {
    size_t at=0;
    while(at<bytes){unsigned char first=(unsigned char)value[at++];uint32_t cp;unsigned following;
        if(first<0x80){cp=first;following=0;}
        else if(first>=0xc2&&first<=0xdf){cp=first&31u;following=1;}
        else if(first>=0xe0&&first<=0xef){cp=first&15u;following=2;}
        else if(first>=0xf0&&first<=0xf4){cp=first&7u;following=3;}
        else return 0;
        if(following>bytes-at)return 0;
        for(unsigned i=0;i<following;++i){unsigned char c=(unsigned char)value[at++];if((c&0xc0)!=0x80)return 0;cp=(cp<<6)|(c&63u);}
        if((following==1&&cp<0x80)||(following==2&&cp<0x800)||(following==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
        unsigned units=cp>0xffff?2:1;if(out->wide_used+units>=ENV_POOL)return 0;
        if(units==2){cp-=0x10000;out->wide[out->wide_used++]=(uint16_t)(0xd800+(cp>>10));out->wide[out->wide_used++]=(uint16_t)(0xdc00+(cp&1023));}
        else out->wide[out->wide_used++]=(uint16_t)cp;
    }
    if(out->wide_used==ENV_POOL)return 0;out->wide[out->wide_used++]=0;return 1;
}
static int prepare(environment_snapshot *out,const win64_environment_entry *entries,size_t count) {
    if(count>ENV_COUNT||(!entries&&count))return 0;
    memset(out,0,sizeof(*out));
    for(size_t i=0;i<count;++i){const char *name=entries[i].name,*value=entries[i].value;
        if(!name||!value)return 0;size_t n=bounded_length(name,ENV_NAME+1),bytes=bounded_length(value,CRT_MAX_ENV);
        if(!n||n>ENV_NAME||bytes>=CRT_MAX_ENV||bytes+1>ENV_POOL-out->narrow_used)return 0;
        for(size_t j=0;j<n;++j)if((unsigned char)name[j]<32||(unsigned char)name[j]>=127||name[j]=='=')return 0;
        for(size_t j=0;j<i;++j)if(same_name(name,out->variables[j].name))return 0;
        environment_variable *slot=&out->variables[i];memcpy(slot->name,name,n+1);slot->narrow=out->narrow_used;slot->wide=out->wide_used;
        memcpy(out->narrow+out->narrow_used,value,bytes+1);out->narrow_used+=bytes+1;
        if(!append_wide(out,value,bytes))return 0;
    }
    out->count=count;return 1;
}
int win64_environment_configure(const win64_environment_entry *entries,size_t count) {
    environment_snapshot candidate;
    if(!prepare(&candidate,entries,count))return 0;
    pthread_mutex_lock(&environment_mutex);
    if(published){pthread_mutex_unlock(&environment_mutex);return 0;}
    environment=candidate;configured=1;pthread_mutex_unlock(&environment_mutex);return 1;
}
static void publish(void) {
    pthread_mutex_lock(&environment_mutex);
    if(!configured)environment_stop("guest process environment was not configured");
    if(!registered){
        win64_region_add((uintptr_t)environment.narrow,sizeof(environment.narrow),0);
        win64_region_add((uintptr_t)environment.wide,sizeof(environment.wide),0);registered=1;
    }
    published=1;pthread_mutex_unlock(&environment_mutex);
}
static int query_name(uint64_t address,int wide,char out[ENV_NAME+1]) {
    if(!address){win64_stdio_set_errno(22);environment_stop("invalid-parameter callback required for NULL name");}
    int possible=1;
    for(size_t i=0;i<CRT_MAX_ENV;++i){uint16_t ch;
        if(wide)memcpy(&ch,win64_pointer(address+i*2,2,0),2);
        else ch=*(const unsigned char *)win64_pointer(address+i,1,0);
        if(!ch){if(possible)out[i]=0;return possible;}
        if(i>=ENV_NAME||ch>=128)possible=0;
        if(possible)out[i]=(char)ch;
    }
    win64_stdio_set_errno(22);environment_stop("invalid-parameter callback required for overlong name");
}
int win64_environment_call(CPU *cpu,const char *name) {
    int wide;if(!strcmp(name,"getenv"))wide=0;else if(!strcmp(name,"_wgetenv"))wide=1;else return 0;
    publish();char key[ENV_NAME+1];cpu->rax=0;
    if(!query_name(cpu->rcx,wide,key))return 1;
    for(size_t i=0;i<environment.count;++i)if(same_name(key,environment.variables[i].name)){
        cpu->rax=wide?(uintptr_t)(environment.wide+environment.variables[i].wide):(uintptr_t)(environment.narrow+environment.variables[i].narrow);break;
    }
    return 1;
}

/* Win32 A queries use the same immutable UTF-8 snapshot as CRT getenv.
 * Only printable-ASCII configured names exist; no Android environ is read.
 * Measured Win32 success/short-buffer preserves LastError; missing sets203.
 * shortcut: Names without NUL in32767 bytes reuse the bounded CRT reader:
 * guest errno22+STOP73 with CRT-callback wording; native Win32 parity for
 * that unsupported name-length boundary is not claimed. SDL names are short. */
int win64_environment_windows_call(CPU *cpu,const char *name) {
    if(strcmp(name,"GetEnvironmentVariableA"))return 0;
    publish();char key[ENV_NAME+1];const char *value=NULL;
    if(cpu->rcx && query_name(cpu->rcx,0,key))
        for(size_t i=0;i<environment.count;++i)if(same_name(key,environment.variables[i].name)){
            value=environment.narrow+environment.variables[i].narrow;break;
        }
    if(!value){win64_set_error(203);cpu->rax=0;return 1;}
    size_t bytes=strlen(value)+1;uint32_t capacity=(uint32_t)cpu->r8;
    if(capacity<bytes){cpu->rax=(uint32_t)bytes;return 1;}
    memcpy(win64_pointer(cpu->rdx,bytes,1),value,bytes);cpu->rax=(uint32_t)(bytes-1);return 1;
}

/* The virtual logon identity matches C:/Users/Android in the private profile
 * aliases. No host account, environment, token, or impersonation is consulted.
 * Native GetUserNameA on the measured Windows ASCII profile reports UTF16
 * bytes (2*(name length+1)) on insufficient space, but CHAR count on success.
 * Short queries do not touch lpBuffer; success writes the name then pcbBuffer,
 * including when those two outputs overlap. Invalid output pointers remain
 * checked-buffer STOPs rather than reproducing native access violations.
 * shortcut: only the fixed ASCII account is supported; no account/token API
 * or Unicode/ACP/impersonation parity is implied by this identity query. */
int win64_identity_call(CPU *cpu,const char *name) {
    if(strcmp(name,"GetUserNameA"))return 0;
    static const char identity[]="Android";
    int saved_errno=errno;uint32_t capacity;
    void *size=win64_pointer(cpu->rdx,4,1);memcpy(&capacity,size,4);
    uint32_t required=(uint32_t)sizeof(identity);
    if(capacity<required){
        required*=2;memcpy(size,&required,4);win64_set_error(122);cpu->rax=0;
    }else{
        memcpy(win64_pointer(cpu->rcx,sizeof(identity),1),identity,sizeof(identity));
        memcpy(size,&required,4);cpu->rax=1;
    }
    errno=saved_errno;return 1;
}

void win64_environment_self_test(void) {
    /* Validate configuration/conversion without publishing or changing the
     * process snapshot. Actual getters/concurrent reads have a device fixture. */
    environment_snapshot candidate;
    const win64_environment_entry values[]={{"Temp","C:\\tmp\\"},{"EMPTY",""},{"UTF8","\xc3\xa9\xf0\x9f\x8c\xa8"}};
    if(!prepare(&candidate,values,3)||!same_name("tEmP",candidate.variables[0].name)||candidate.narrow[candidate.variables[1].narrow]||
        candidate.wide[candidate.variables[2].wide]!=0xe9||candidate.wide[candidate.variables[2].wide+1]!=0xd83c||candidate.wide[candidate.variables[2].wide+2]!=0xdf28)environment_stop("snapshot/UTF8 self-test");
    const win64_environment_entry duplicate[]={{"TEMP","a"},{"temp","b"}},invalid[]={{"A=B","x"}},bad_utf8[]={{"A","\xc0\x80"}};
    if(prepare(&candidate,duplicate,2)||prepare(&candidate,invalid,1)||prepare(&candidate,bad_utf8,1)||!prepare(&candidate,NULL,0))environment_stop("configuration validation self-test");
    CPU cpu={0},before;cpu.rsp=123;cpu.rax=456;before=cpu;
    if(win64_environment_call(&cpu,"unknown_environment")||memcmp(&cpu,&before,sizeof(cpu)))environment_stop("unknown API self-test");
    fputs("WIN64_ENVIRONMENT_SELF_TEST explicit_snapshot=1 UTF8_UTF16=1 case_insensitive_ASCII_names=1 empty_value=1 no_host_environ=1 passed\n",stderr);
}
