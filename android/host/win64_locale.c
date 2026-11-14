/* UCRT data ABI at the Win64 boundary, independently implemented from SDK
 * layouts and development-oracle observations. Android locale_t never escapes. */
#include "win64_locale.h"
#include "win64.h"
#include "win64_stdio.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct {
    uint64_t narrow[10];
    uint8_t monetary[8];
    uint64_t wide[8];
} guest_lconv;
_Static_assert(sizeof(guest_lconv)==152 && offsetof(guest_lconv,monetary)==80 &&
               offsetof(guest_lconv,wide)==88,"Win64 UCRT lconv ABI");
static guest_lconv c_conventions;
static char empty_a[]="",point_a[]=".",c_name[]="C",utf8_name[]="en-US.UTF8";
static char mixed_name[]="LC_COLLATE=C;LC_CTYPE=en-US.UTF8;LC_MONETARY=C;LC_NUMERIC=C;LC_TIME=C";
static uint16_t empty_w[]={0},point_w[]={'.',0},c_name_w[2],utf8_name_w[11],mixed_name_w[80];
static uint16_t en_us_w[]={'e','n','-','U','S',0};
static uint16_t ctype_tables[2][384];
static uint64_t locale_names[2][6];
/* Only CTYPE differs: monetary/numeric/collation/time remain the C locale. */
static int global_utf8,global_private;
static _Thread_local int attached,private_mode,local_utf8,lock_depth;

static _Noreturn void locale_stop(const char *reason) {
    fprintf(stderr,"STOP Win64 locale %s\n",reason);exit(73);
}
static void register_data(void *p,size_t bytes) { win64_region_add((uintptr_t)p,bytes,0); }
static void wide_ascii(uint16_t *out,const char *in) { do { *out++=(unsigned char)*in; } while (*in++); }
static void initialize_data(void) {
    for (unsigned i=0;i<10;++i) c_conventions.narrow[i]=(uintptr_t)(i?empty_a:point_a);
    memset(c_conventions.monetary,127,sizeof(c_conventions.monetary));
    for (unsigned i=0;i<8;++i) c_conventions.wide[i]=(uintptr_t)(i?empty_w:point_w);
    wide_ascii(c_name_w,c_name);wide_ascii(utf8_name_w,utf8_name);wide_ascii(mixed_name_w,mixed_name);
    for (unsigned i=0;i<128;++i) {
        unsigned f=0;
        if (i<32 || i==127) f|=0x20;
        if ((i>=9 && i<=13) || i==32) f|=8;
        if (i==32) f|=0x40;
        if (i>='0' && i<='9') f|=4|0x80;
        if (i>='A' && i<='Z') f|=1;
        if (i>='a' && i<='z') f|=2;
        if ((i>='A' && i<='F') || (i>='a' && i<='f')) f|=0x80;
        if (i>=33 && i<=126 && !(f&7)) f|=0x10;
        ctype_tables[0][128+i]=(uint16_t)f;
        ctype_tables[1][128+i]=(uint16_t)(f|0x200|((f&3)?0x100:0)|(i==9?0x40:0));
    }
    for (unsigned i=128;i<256;++i) ctype_tables[1][128+i]=(i>=0xc2 && i<0xf5)?0x8000:0;
    for (unsigned i=0;i<127;++i) ctype_tables[1][i]=ctype_tables[1][256+i];
    locale_names[1][2]=(uintptr_t)en_us_w;
    register_data(&c_conventions,sizeof(c_conventions));
    register_data(empty_a,sizeof(empty_a));register_data(point_a,sizeof(point_a));
    register_data(empty_w,sizeof(empty_w));register_data(point_w,sizeof(point_w));
    register_data(c_name,sizeof(c_name));register_data(utf8_name,sizeof(utf8_name));register_data(mixed_name,sizeof(mixed_name));
    register_data(c_name_w,sizeof(c_name_w));register_data(utf8_name_w,sizeof(utf8_name_w));register_data(mixed_name_w,sizeof(mixed_name_w));
    register_data(en_us_w,sizeof(en_us_w));register_data(ctype_tables,sizeof(ctype_tables));register_data(locale_names,sizeof(locale_names));
}
#if defined(_MSC_VER)
static INIT_ONCE locale_once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION locale_mutex;
static BOOL CALLBACK initialize_locale(PINIT_ONCE once,PVOID arg,PVOID *context) {
    (void)once;(void)arg;(void)context;InitializeCriticalSection(&locale_mutex);initialize_data();return TRUE;
}
static void locale_lock(void) { InitOnceExecuteOnce(&locale_once,initialize_locale,NULL,NULL);EnterCriticalSection(&locale_mutex);++lock_depth; }
static void locale_unlock(void) { if (!lock_depth) locale_stop("unbalanced locale unlock");--lock_depth;LeaveCriticalSection(&locale_mutex); }
#else
static pthread_once_t locale_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t locale_mutex;
static void initialize_locale(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&locale_mutex,&attr) || pthread_mutexattr_destroy(&attr)) locale_stop("mutex initialization");
    initialize_data();
}
static void locale_lock(void) {
    if (pthread_once(&locale_once,initialize_locale) || pthread_mutex_lock(&locale_mutex)) locale_stop("mutex lock");
    ++lock_depth;
}
static void locale_unlock(void) {
    if (!lock_depth) locale_stop("unbalanced locale unlock");
    --lock_depth;if (pthread_mutex_unlock(&locale_mutex)) locale_stop("mutex unlock");
}
#endif
static void attach_locked(void) { if (!attached) { attached=1;private_mode=0;local_utf8=global_utf8; } }
void win64_locale_thread_attach(void) {
    locale_lock();attached=1;private_mode=0;local_utf8=global_utf8;locale_unlock();
}
static int current_locked(void) {
    attach_locked();if (!private_mode && !global_private) local_utf8=global_utf8;return local_utf8;
}
static int config_locked(int flag) {
    attach_locked();int previous=private_mode?1:2;
    switch (flag) {
        case 0:break;
        case 1:private_mode=1;break;
        case 2:private_mode=0;break;
        /* The UCRT link option disables global resynchronization; mode query
         * still returns2. It does not enable the per-thread-mode flag. */
        case -1:global_private=1;break;
        default:win64_stdio_set_errno(EINVAL);return -1;
    }
    return previous;
}
static uint64_t locale_name_locked(int category,int wide) {
    int utf8=current_locked();
    if (utf8 && category==0) return (uintptr_t)(wide?(void*)mixed_name_w:(void*)mixed_name);
    if (utf8 && category==2) return (uintptr_t)(wide?(void*)utf8_name_w:(void*)utf8_name);
    return (uintptr_t)(wide?(void*)c_name_w:(void*)c_name);
}
static uint64_t set_locale_locked(int category,uint64_t name,int wide) {
    if (category<0 || category>5) { win64_stdio_set_errno(EINVAL);return 0; }
    if (!name) return locale_name_locked(category,wide);
    char text[128];const char *requested;
    if (wide) {
        unsigned i;
        for (i=0;i<sizeof(text);++i) {
            uint16_t ch;memcpy(&ch,win64_pointer(name+i*2,2,0),2);
            if (ch>127) { win64_stdio_set_errno(ENOENT);return 0; }
            text[i]=(char)ch;if (!ch) break;
        }
        if (i==sizeof(text)) { win64_stdio_set_errno(ENOENT);return 0; }
        requested=text;
    } else requested=win64_string_a(name);
    int utf8;
    if (!strcmp(requested,"C")) utf8=0;
    else if ((category==2 && !strcmp(requested,utf8_name)) || (category==0 && !strcmp(requested,mixed_name))) utf8=1;
    else { win64_stdio_set_errno(ENOENT);return 0; }
    current_locked();
    if (category==0 || category==2) {
        local_utf8=utf8;if (!private_mode && !global_private) global_utf8=utf8;
    }
    return locale_name_locked(category,wide);
}

int win64_locale_call(CPU *c,const char *name) {
    if (!strcmp(name,"_lock_locales")) { locale_lock();return 1; }
    if (!strcmp(name,"_unlock_locales")) { locale_unlock();return 1; }
    if (!strcmp(name,"_configthreadlocale")) {
        locale_lock();c->rax=(uint32_t)config_locked((int)win64_argument(c,0));locale_unlock();return 1;
    }
    if (!strcmp(name,"setlocale") || !strcmp(name,"_wsetlocale")) {
        locale_lock();c->rax=set_locale_locked((int)win64_argument(c,0),win64_argument(c,1),name[0]=='_');locale_unlock();return 1;
    }
    if (!strcmp(name,"localeconv") || !strcmp(name,"___lc_locale_name_func") ||
        !strcmp(name,"___mb_cur_max_func") || !strcmp(name,"___lc_codepage_func") ||
        !strcmp(name,"___lc_collate_cp_func") || !strcmp(name,"__pctype_func")) {
        locale_lock();int utf8=current_locked();
        if (!strcmp(name,"localeconv")) c->rax=(uintptr_t)&c_conventions;
        else if (!strcmp(name,"___lc_locale_name_func")) c->rax=(uintptr_t)locale_names[utf8];
        else if (!strcmp(name,"___mb_cur_max_func")) c->rax=utf8?4:1;
        else if (!strcmp(name,"___lc_codepage_func")) c->rax=utf8?65001:0;
        else if (!strcmp(name,"___lc_collate_cp_func")) c->rax=0;
        else c->rax=(uintptr_t)&ctype_tables[utf8][128];
        locale_unlock();return 1;
    }
    if (!strcmp(name,"tolower") || !strcmp(name,"toupper")) {
        int value=(int)win64_argument(c,0);
        if (name[2]=='l' && value>='A' && value<='Z') value+='a'-'A';
        if (name[2]=='u' && value>='a' && value<='z') value-='a'-'A';
        c->rax=(uint32_t)value;return 1;
    }
    unsigned mask=!strcmp(name,"isupper")?1:!strcmp(name,"islower")?2:
        !strcmp(name,"isalpha")?0x103:!strcmp(name,"isalnum")?0x107:
        !strcmp(name,"iscntrl")?0x20:!strcmp(name,"isspace")?8:
        !strcmp(name,"isdigit")?4:!strcmp(name,"isxdigit")?0x80:!strcmp(name,"ispunct")?0x10:0;
    if (mask) {
        int value=(int)win64_argument(c,0);
        if(value < -1 || value > 255) { c->rax=0;return 1; }
        int saved_errno=errno;
        locale_lock();int utf8=current_locked();c->rax=ctype_tables[utf8][128+value]&mask;locale_unlock();errno=saved_errno;return 1;
    }
    if (!strcmp(name,"strcoll")) {
        int result=strcmp(win64_string_a(win64_argument(c,0)),win64_string_a(win64_argument(c,1)));
        c->rax=(uint32_t)((result>0)-(result<0));return 1;
    }
    return 0;
}

static void locale_check(int condition,const char *reason) { if (!condition) locale_stop(reason); }
void win64_locale_self_test(void) {
    CPU c={0};c.rsp=UINT64_C(0x12345678);locale_lock();locale_lock();
    int saved_global=global_utf8,saved_private=global_private,saved_attached=attached;
    int saved_mode=private_mode,saved_local=local_utf8;
    global_utf8=global_private=private_mode=local_utf8=0;attached=1;
    locale_check(config_locked(0)==2,"initial mode");
    locale_check(set_locale_locked(0,(uintptr_t)c_name,0)==(uintptr_t)c_name,"C setlocale");
    locale_check(c_conventions.monetary[0]==127 && c_conventions.narrow[0]==(uintptr_t)point_a &&
                 c_conventions.wide[0]==(uintptr_t)point_w,"lconv data");
    locale_check(ctype_tables[0][128+'A']==0x81 && ctype_tables[0][127]==0 &&
                 ctype_tables[1][128+'A']==0x381 && ctype_tables[1][128+0xc2]==0x8000 &&
                 ctype_tables[1][128+0xc1]==0 && ctype_tables[1][128+0xf5]==0,"ctype tables");
    locale_check(config_locked(1)==2,"enable private mode");
    locale_check(set_locale_locked(2,(uintptr_t)utf8_name,0)==(uintptr_t)utf8_name && global_utf8==0,"private UTF8 state");
    locale_check(locale_name_locked(0,1)==(uintptr_t)mixed_name_w && locale_names[1][2]==(uintptr_t)en_us_w &&
                 locale_names[1][1]==0,"UTF8 category data");
    c.rcx='A';locale_check(win64_locale_call(&c,"isalpha") && c.rax==0x101,"UTF8 alpha result mask");
    c.rcx=UINT64_C(0x1234567800000030);locale_check(win64_locale_call(&c,"isdigit") && c.rax==4,"UTF8 digit mask and low32 argument");
    c.rcx='f';locale_check(win64_locale_call(&c,"isxdigit") && c.rax==0x80,"UTF8 hex digit mask");
    c.rcx='_';locale_check(win64_locale_call(&c,"ispunct") && c.rax==0x10,"UTF8 punctuation mask");
    c.rcx=UINT32_MAX;locale_check(win64_locale_call(&c,"isspace") && !c.rax,"ctype EOF");
    c.rcx=UINT32_MAX-1;locale_check(win64_locale_call(&c,"isupper") && !c.rax,"ctype input bounds");
    locale_check(config_locked(2)==1 && current_locked()==0,"restore global state");
    c.rcx='A';locale_check(win64_locale_call(&c,"isalpha") && c.rax==1,"C alpha result mask");
    c.rcx='9';locale_check(win64_locale_call(&c,"isdigit") && c.rax==4,"C digit mask");
    c.rcx='G';locale_check(win64_locale_call(&c,"isxdigit") && !c.rax,"C nonhex digit");
    c.rcx=0xc2;locale_check(win64_locale_call(&c,"ispunct") && !c.rax,"C high byte punctuation");
    c.rcx='A';locale_check(win64_locale_call(&c,"tolower") && c.rax=='a' && c.rsp==UINT64_C(0x12345678),"dispatch preserves RSP");
    locale_check(!win64_locale_call(&c,"unknown_locale_function"),"unknown import");
    global_utf8=saved_global;global_private=saved_private;attached=saved_attached;
    private_mode=saved_mode;local_utf8=saved_local;locale_unlock();locale_unlock();
    fprintf(stderr,"WIN64_LOCALE_SELF_TEST lconv_bytes=152 ctype_entries=768 ctype_functions=9 C=1 UTF8_CTYPE=1 recursive_lock=1 no_rsp_pop=1 passed\n");
}
