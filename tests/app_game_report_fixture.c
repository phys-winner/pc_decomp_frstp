/* Exact production report() is included; only its platform/FILE boundary is mocked. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <inttypes.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define FP_GAME_RUNTIME 1
#define FIXTURE_NAME "static-recompiled-original-game-worker"
#define FIXTURE_TITLE "Frostpunk static recompilation"
#define CHECK(value) do{if(!(value)){fprintf(stderr,"APP_REPORT_FAIL line=%d expression=%s\n",__LINE__,#value);exit(1);}}while(0)
typedef struct {
    int mutex,worker_started,worker_finished,worker_result,runtime_loaded,overflow;
    void *window;uint64_t generation;
    char error[512],private_path[PATH_MAX],reported_game_state[FP_REPORT_CACHE_BYTES];
    unsigned char canary[16];
} app_context;
enum {FAIL_OPEN=1,FAIL_PUTS,FAIL_CLOSE,FAIL_RENAME};
static struct {
    unsigned opens,puts,closes,renames,commits,statuses,locks,unlocks;
    int open,failure,format_failure;
    char temporary[512],committed[512],status[1024];
    app_context *context;
} io;
static unsigned checks,changed_fields,failure_cases,format_cases;
static FILE *token(void){return(FILE*)(uintptr_t)0x12340;}
static int fail(int stage){if(io.failure!=stage)return 0;io.failure=0;return 1;}
static int fixture_lock(int *mutex){CHECK(!*mutex);*mutex=1;++io.locks;return 0;}
static int fixture_unlock(int *mutex){CHECK(*mutex);*mutex=0;++io.unlocks;return 0;}
static FILE *fixture_fopen(const char *path,const char *mode){
    CHECK(!io.context->mutex&&!strcmp(mode,"w")&&!strcmp(path,"/app/files/game-state.tmp"));
    ++io.opens;if(fail(FAIL_OPEN))return NULL;
    CHECK(!io.open);io.open=1;io.temporary[0]=0;return token();
}
static int fixture_fputs(const char *text,FILE *file){
    CHECK(file==token()&&io.open&&!io.context->mutex);++io.puts;
    if(fail(FAIL_PUTS)){strcpy(io.temporary,"partial");return EOF;}
    CHECK(strlen(text)<sizeof(io.temporary));strcpy(io.temporary,text);return 1;
}
static int fixture_fclose(FILE *file){
    CHECK(file==token()&&io.open&&!io.context->mutex);io.open=0;++io.closes;
    return fail(FAIL_CLOSE)?EOF:0;
}
static int fixture_rename(const char *from,const char *to){
    CHECK(!io.open&&!io.context->mutex&&!strcmp(from,"/app/files/game-state.tmp")&&!strcmp(to,"/app/files/game-state.json"));
    ++io.renames;if(fail(FAIL_RENAME))return -1;
    strcpy(io.committed,io.temporary);++io.commits;return 0;
}
static int fixture_snprintf(char *output,size_t size,const char *format,...){
    if(format[0]=='{'&&io.format_failure){int result=io.format_failure;io.format_failure=0;return result==1?-1:(int)size;}
    va_list args;va_start(args,format);int result=vsnprintf(output,size,format,args);va_end(args);return result;
}
static void set_status(void *opaque,const char *text){
    CHECK(opaque==io.context&&!io.context->mutex&&strlen(text)<sizeof(io.status));++io.statuses;strcpy(io.status,text);
}
#define pthread_mutex_lock fixture_lock
#define pthread_mutex_unlock fixture_unlock
#define fopen fixture_fopen
#define fputs fixture_fputs
#define fclose fixture_fclose
#define rename fixture_rename
#define snprintf fixture_snprintf
#include "app_report.inc"
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef fopen
#undef fputs
#undef fclose
#undef rename
#undef snprintf

static void initialize(app_context *c){
    memset(c,0,sizeof(*c));memset(c->canary,0xa5,sizeof(c->canary));strcpy(c->private_path,"/app/files");io.context=c;
}
static void check_cache(app_context *c){
    CHECK(!c->mutex&&!io.open&&io.locks==io.unlocks);
    for(unsigned i=0;i<sizeof(c->canary);++i)CHECK(c->canary[i]==0xa5);
    if(c->reported_game_state[0])CHECK(!strcmp(c->reported_game_state,io.committed));++checks;
}
static void changed(app_context *c,const char *fragment){
    unsigned old=io.commits;report(c);CHECK(io.commits==old+1&&strstr(io.committed,fragment));check_cache(c);++changed_fields;
}
int main(void){
    _Static_assert(FP_REPORT_CACHE_BYTES==512,"actual production report cache capacity");
    app_context c;initialize(&c);report(&c);check_cache(&c);
    CHECK(io.commits==1&&io.opens==1&&io.puts==1&&io.closes==1&&io.renames==1&&io.statuses==1);
    unsigned opens=io.opens,statuses=io.statuses;
    for(unsigned i=0;i<10000;++i){report(&c);check_cache(&c);}
    CHECK(io.opens==opens&&io.commits==1&&io.statuses==statuses+10000);
    c.worker_started=1;changed(&c,"\"worker_started\":true");
    c.worker_finished=1;changed(&c,"\"worker_finished\":true");
    c.worker_result=INT_MIN;changed(&c,"\"worker_result\":-2147483648");
    c.runtime_loaded=1;changed(&c,"\"runtime_loaded\":true");
    c.window=(void*)(uintptr_t)1;changed(&c,"\"real_window\":true");
    c.generation=UINT64_MAX;changed(&c,"\"window_generation\":18446744073709551615");
    c.overflow=1;changed(&c,"\"queue_overflow\":true");
    /* Error-only changes must update UI without rewriting identical JSON. */
    opens=io.opens;statuses=io.statuses;strcpy(c.error,"first error");report(&c);
    CHECK(io.opens==opens&&io.statuses==statuses+1&&!strcmp(io.status,"first error"));
    strcpy(c.error,"second error");report(&c);CHECK(io.opens==opens&&!strcmp(io.status,"second error"));
    memset(c.error,'x',sizeof(c.error)-1);c.error[sizeof(c.error)-1]=0;report(&c);
    CHECK(io.opens==opens&&strlen(io.status)==511);check_cache(&c);c.error[0]=0;
    for(int stage=FAIL_OPEN;stage<=FAIL_RENAME;++stage){
        char old_cache[512],old_file[512];memcpy(old_cache,c.reported_game_state,512);memcpy(old_file,io.committed,512);
        ++c.generation;unsigned commits=io.commits,puts=io.puts,closes=io.closes,renames=io.renames;
        io.failure=stage;report(&c);
        CHECK(!memcmp(old_cache,c.reported_game_state,512)&&!memcmp(old_file,io.committed,512)&&io.commits==commits);
        CHECK(io.puts==puts+(stage!=FAIL_OPEN)&&io.closes==closes+(stage!=FAIL_OPEN));
        CHECK(io.renames==renames+(stage==FAIL_RENAME));check_cache(&c);
        report(&c);CHECK(io.commits==commits+1);check_cache(&c);
        opens=io.opens;report(&c);CHECK(io.opens==opens);++failure_cases;
    }
    for(int failure=1;failure<=2;++failure){
        char old[512];memcpy(old,c.reported_game_state,512);++c.generation;opens=io.opens;io.format_failure=failure;
        report(&c);CHECK(io.opens==opens&&!memcmp(old,c.reported_game_state,512));
        report(&c);CHECK(io.opens==opens+1);check_cache(&c);++format_cases;
    }
    c.worker_started=c.worker_finished=c.runtime_loaded=c.overflow=0;c.window=NULL;c.worker_result=INT_MIN;c.generation=UINT64_MAX;
    changed(&c,"\"worker_started\":false");
    size_t maximum_length=strlen(c.reported_game_state);CHECK(maximum_length<512);
    char maximum[512];strcpy(maximum,c.reported_game_state);
    /* A new Activity context must publish once even if an old file remains. */
    initialize(&c);unsigned commits=io.commits;report(&c);CHECK(io.commits==commits+1);check_cache(&c);
    printf("{\"success\":true,\"duplicate_reports\":10000,\"duplicate_new_writes\":0,\"changed_fields\":%u,\"IO_failures_then_retry\":%u,\"format_failures_then_retry\":%u,\"error_UI_without_JSON_change\":true,\"closed_after_puts_failure\":true,\"cache_only_after_commit\":true,\"new_Activity_republishes\":true,\"cache_capacity\":512,\"maximum_JSON_bytes\":%zu,\"maximum_state\":%s,\"checks\":%u,\"platform_IO_mocked\":true,\"game_executed\":false}\n",
        changed_fields,failure_cases,format_cases,maximum_length,maximum,checks);
    return 0;
}
