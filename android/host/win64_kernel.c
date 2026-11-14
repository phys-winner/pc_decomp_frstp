/* POSIX-backed Win64 kernel subset. Named objects belong to this guest process;
 * absent external IPC objects stay absent. Original inputs are read-only and
 * profile operations use anchored openat traversal with no symlink following.
 * There are no executable mappings, guest thread callbacks or fake API success.
 */
#define _GNU_SOURCE
#include "win64_kernel.h"
#include "zip_files.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ERROR_FILE_NOT_FOUND=2, ERROR_PATH_NOT_FOUND=3, ERROR_ACCESS_DENIED=5,
 ERROR_INVALID_HANDLE=6, ERROR_NOT_ENOUGH_MEMORY=8, ERROR_INVALID_DATA=13,
 ERROR_SHARING_VIOLATION=32, ERROR_NOT_SUPPORTED=50, ERROR_FILE_EXISTS=80,
 ERROR_INVALID_PARAMETER=87, ERROR_DISK_FULL=112, ERROR_INVALID_NAME=123,
 ERROR_DIR_NOT_EMPTY=145, ERROR_ALREADY_EXISTS=183, ERROR_FILENAME_EXCED_RANGE=206,
 ERROR_TOO_MANY_POSTS=298, ERROR_NO_UNICODE_TRANSLATION=1113,
 ERROR_MAPPED_ALIGNMENT=1132 };
enum { OBJ_EVENT=1, OBJ_SEMAPHORE=2, OBJ_FILE=3, OBJ_MAPPING=4, OBJ_THREAD=5, OBJ_FIND=6 };
#define INVALID_HANDLE UINT64_MAX
#define WAIT_FAILED UINT32_MAX
#define WAIT_TIMEOUT 258u
#define SYNCHRONIZE 0x100000u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u

typedef struct object object;
struct object {
    int type; unsigned refs; char *name; object *next;
    union {
        struct { int manual, signaled; } event;
        struct { int32_t count, maximum; } semaphore;
        struct { int fd, readable, writable, delete_access, zipped; uint32_t share; dev_t dev; ino_t ino; fp_data_file data; uint64_t position; } file;
        struct { int fd; uint64_t bytes; uint32_t protection; object *file; } mapping;
        struct {void *payload;void (*destroy)(void *);int completed;uint32_t exit_code;} thread;
        struct {DIR *directory;char *pattern;int all,directories,sensitive,parent,ziproot,physical_done;size_t zip_cursor;} find;
    } u;
};
typedef struct handle { uint64_t id; object *obj; uint32_t access, flags; struct handle *next; } handle;
typedef struct view { void *base; size_t bytes; object *obj; struct view *next; void *native_base;size_t native_bytes; } view;
/* Every waiter sleeps on its own condition variable: a signal wakes exactly the waits it
 * satisfied. One shared variable woke every sleeping guest thread on every SetEvent and
 * ReleaseSemaphore (about ten idle threads, several dozen times per frame). */
typedef struct waiter {object **selected;unsigned count;int all,done;uint32_t result;struct waiter *next;pthread_cond_t *wake;} waiter;
static object *objects;
static handle *handles;
static view *views;
static waiter *waiters;
static uint64_t next_handle=UINT64_C(0xe5e60000);
static pthread_mutex_t kernel_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t kernel_once=PTHREAD_ONCE_INIT;
static pthread_condattr_t wait_clock;
static const char *game_root="/sdcard/Frostpunk/game";
static const char *profile_root="/sdcard/Frostpunk/profile";
extern int frostpunk_switch_ui(void) __attribute__((weak));
static int kernel_started,roots_configured;
static const char guest_game[]="C:/Program Files (x86)/Steam/steamapps/common/Frostpunk";
static const char guest_profile[]="C:/Users/Android/AppData/Roaming/11bitstudios/Frostpunk";
static const char guest_profile_root[]="C:/FrostpunkProfile";
static const char guest_documents[]="C:/Users/Android/Documents/My Games/Frostpunk";

static void initialize(void) {
    pthread_mutex_lock(&kernel_mutex);kernel_started=1;pthread_mutex_unlock(&kernel_mutex);
    pthread_condattr_init(&wait_clock);
    pthread_condattr_setclock(&wait_clock,CLOCK_MONOTONIC);
}
static uint64_t error_value(uint32_t error,uint64_t result) { win64_set_error(error); return result; }
static uint32_t posix_error(int error) {
    switch(error) {
    case ENOENT:return ERROR_FILE_NOT_FOUND; case ENOTDIR:return ERROR_PATH_NOT_FOUND;
    case EACCES:case EPERM:case ELOOP:case EROFS:case EISDIR:return ERROR_ACCESS_DENIED;
    case ENOMEM:case EMFILE:case ENFILE:return ERROR_NOT_ENOUGH_MEMORY;
    case EEXIST:return ERROR_FILE_EXISTS; case ENOTEMPTY:return ERROR_DIR_NOT_EMPTY;
    case ENOSPC:case EDQUOT:return ERROR_DISK_FULL; case ENAMETOOLONG:return ERROR_FILENAME_EXCED_RANGE;
    case EBADF:return ERROR_INVALID_HANDLE; case EINVAL:return ERROR_INVALID_PARAMETER;
    default:return ERROR_INVALID_DATA;
    }
}
static handle **handle_table;
static size_t handle_table_size;
#define HANDLE_FIRST UINT64_C(0xe5e60008)
static handle *find_handle(uint64_t id) {
    if(id<HANDLE_FIRST||(id-HANDLE_FIRST)%8)return NULL;
    uint64_t index=(id-HANDLE_FIRST)/8;
    return index<handle_table_size?handle_table[index]:NULL;
}
static int handle_table_set(uint64_t id,handle *h) {
    uint64_t index=(id-HANDLE_FIRST)/8;
    if(index>=handle_table_size) {
        size_t size=handle_table_size?handle_table_size:4096;
        while(size<=index)size*=2;
        handle **grown=realloc(handle_table,size*sizeof(*grown));if(!grown)return 0;
        memset(grown+handle_table_size,0,(size-handle_table_size)*sizeof(*grown));
        handle_table=grown;handle_table_size=size;
    }
    handle_table[index]=h;return 1;
}
static object *find_name(const char *name) {
    if(name)for(object *o=objects;o;o=o->next)if(o->name && !strcmp(o->name,name))return o;
    return NULL;
}
static object *new_object(int type,const char *name) {
    object *o=calloc(1,sizeof(*o)); if(!o)return NULL;
    if(name && !(o->name=strdup(name))) {free(o);return NULL;}
    o->type=type; o->next=objects; objects=o; return o;
}
static void release_object(object *o) {
    if(--o->refs)return;
    object **p=&objects; while(*p!=o)p=&(*p)->next; *p=o->next;
    if(o->type==OBJ_FILE){if(o->u.file.zipped)fp_data_close(&o->u.file.data);else close(o->u.file.fd);}
    if(o->type==OBJ_MAPPING) {close(o->u.mapping.fd);if(o->u.mapping.file)release_object(o->u.mapping.file);}
    if(o->type==OBJ_THREAD && o->u.thread.destroy)o->u.thread.destroy(o->u.thread.payload);
    if(o->type==OBJ_FIND){closedir(o->u.find.directory);free(o->u.find.pattern);}
    free(o->name); free(o);
}
static uint64_t new_handle(object *o,uint32_t access,int inherit) {
    handle *h=calloc(1,sizeof(*h));
    if(!h)return error_value(ERROR_NOT_ENOUGH_MEMORY,0);
    if(!handle_table_set(next_handle+8,h)){free(h);return error_value(ERROR_NOT_ENOUGH_MEMORY,0);}
    next_handle+=8; *h=(handle){next_handle,o,access,inherit?1u:0u,handles}; handles=h; ++o->refs; return h->id;
}
static int close_handle(uint64_t id) {
    handle *found=find_handle(id);
    if(!found)return (int)error_value(ERROR_INVALID_HANDLE,0);
    if(found->flags&2)return (int)error_value(ERROR_ACCESS_DENIED,0);
    handle **p=&handles; while(*p!=found)p=&(*p)->next;
    *p=found->next; handle_table[(id-HANDLE_FIRST)/8]=NULL; release_object(found->obj); free(found); return 1;
}
static void store32(uint64_t address,uint32_t value) {memcpy(win64_pointer(address,4,1),&value,4);}
static void store64(uint64_t address,uint64_t value) {memcpy(win64_pointer(address,8,1),&value,8);}

static int utf8_units(const char *text) {
    const unsigned char *p=(const unsigned char *)text;int units=0;
    while(*p) {
        uint32_t cp;unsigned continuation;
        if(*p<0x80){cp=*p++;continuation=0;}
        else if(*p>=0xc2&&*p<=0xdf){cp=*p++&31;continuation=1;}
        else if(*p>=0xe0&&*p<=0xef){cp=*p++&15;continuation=2;}
        else if(*p>=0xf0&&*p<=0xf4){cp=*p++&7;continuation=3;}
        else return -1;
        unsigned count=continuation;
        while(continuation--){if((*p&0xc0)!=0x80)return -1;cp=(cp<<6)|(*p++&63);}
        if((count==1&&cp<0x80)||(count==2&&cp<0x800)||(count==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return -1;
        units+=cp>=0x10000?2:1;
    }
    return units;
}

/* Win64 WCHAR is UTF-16, not host wchar_t. Validate complete surrogate pairs. */
static char *guest_text(uint64_t address,int wide) {
    if(!address)return NULL;
    if(!wide){const char *source=win64_string_a(address);if(utf8_units(source)<0){win64_set_error(ERROR_NO_UNICODE_TRANSLATION);return NULL;}
        char *text=strdup(source);if(!text)win64_set_error(ERROR_NOT_ENOUGH_MEMORY);return text;}
    char *out=malloc(131069); if(!out){win64_set_error(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    size_t at=0;
    for(size_t i=0;i<32768;++i) {
        uint16_t first;memcpy(&first,win64_pointer(address+i*2,2,0),2);
        if(!first){out[at]=0;return out;}
        uint32_t cp=first;
        if(first>=0xd800 && first<=0xdbff) {
            uint16_t second;
            if(++i>=32768){free(out);win64_set_error(ERROR_NO_UNICODE_TRANSLATION);return NULL;}
            memcpy(&second,win64_pointer(address+i*2,2,0),2);
            if(second<0xdc00 || second>0xdfff){free(out);win64_set_error(ERROR_NO_UNICODE_TRANSLATION);return NULL;}
            cp=0x10000+(((uint32_t)first-0xd800)<<10)+(second-0xdc00);
        } else if(first>=0xdc00 && first<=0xdfff) {free(out);win64_set_error(ERROR_NO_UNICODE_TRANSLATION);return NULL;}
        if(cp<0x80)out[at++]=(char)cp;
        else if(cp<0x800){out[at++]=(char)(0xc0|(cp>>6));out[at++]=(char)(0x80|(cp&63));}
        else if(cp<0x10000){out[at++]=(char)(0xe0|(cp>>12));out[at++]=(char)(0x80|((cp>>6)&63));out[at++]=(char)(0x80|(cp&63));}
        else {out[at++]=(char)(0xf0|(cp>>18));out[at++]=(char)(0x80|((cp>>12)&63));out[at++]=(char)(0x80|((cp>>6)&63));out[at++]=(char)(0x80|(cp&63));}
    }
    free(out);win64_set_error(ERROR_FILENAME_EXCED_RANGE);return NULL;
}
static int security(uint64_t address,int *inherit) {
    *inherit=0;if(!address)return 1;
    const unsigned char *p=win64_pointer(address,24,0);uint32_t length,flag;uint64_t descriptor;
    memcpy(&length,p,4);memcpy(&descriptor,p+8,8);memcpy(&flag,p+16,4);
    if(length!=24){win64_set_error(ERROR_INVALID_PARAMETER);return 0;}
    if(descriptor){win64_set_error(ERROR_NOT_SUPPORTED);return 0;}
    *inherit=flag!=0;return 1;
}
static int valid_object_name(const char *name) {
    if(!name)return 1;
    if(utf8_units(name)>260){win64_set_error(ERROR_FILENAME_EXCED_RANGE);return 0;}
    const char *tail=name;
    if(!strncmp(tail,"Global\\",7))tail+=7;else if(!strncmp(tail,"Local\\",6))tail+=6;
    if(strchr(tail,'\\')){win64_set_error(ERROR_INVALID_NAME);return 0;}return 1;
}
static uint64_t create_sync(int type,uint64_t sec,int32_t a,int32_t b,uint64_t name_ptr,int wide,uint32_t access) {
    int inherit;if(!security(sec,&inherit))return 0;
    char *name=guest_text(name_ptr,wide);if(name_ptr&&!name)return 0;
    if(name&&!*name){free(name);name=NULL;}
    if(!valid_object_name(name)){free(name);return 0;}
    object *o=find_name(name);int exists=o!=NULL;
    if(o && o->type!=type){free(name);return error_value(ERROR_INVALID_HANDLE,0);}
    if(!o) {
        if(type==OBJ_SEMAPHORE && (a<0 || b<=0 || a>b)){free(name);return error_value(ERROR_INVALID_PARAMETER,0);}
        o=new_object(type,name);if(!o){free(name);return error_value(ERROR_NOT_ENOUGH_MEMORY,0);}
        if(type==OBJ_EVENT){o->u.event.manual=a!=0;o->u.event.signaled=b!=0;}
        else{o->u.semaphore.count=a;o->u.semaphore.maximum=b;}
    }
    free(name);uint64_t result=new_handle(o,access,inherit);
    if(!result && !o->refs){o->refs=1;release_object(o);}
    if(result)win64_set_error(exists?ERROR_ALREADY_EXISTS:0);return result;
}
static uint64_t open_named(int type,uint32_t access,int inherit,uint64_t pointer,int wide) {
    if(!pointer)return error_value(ERROR_INVALID_PARAMETER,0);
    char *name=guest_text(pointer,wide);if(!name)return 0;
    if(!*name){free(name);return error_value(ERROR_INVALID_NAME,0);}
    if(!valid_object_name(name)){free(name);return 0;}
    object *o=find_name(name);free(name);
    if(!o)return error_value(ERROR_FILE_NOT_FOUND,0);
    if(o->type!=type)return error_value(ERROR_INVALID_HANDLE,0);
    if(type==OBJ_MAPPING && ((access&0x20) || ((access&2) && o->u.mapping.protection!=4)))
        return error_value(ERROR_ACCESS_DENIED,0);
    return new_handle(o,access,inherit);
}
static int signaled(object *o) {return o->type==OBJ_EVENT?o->u.event.signaled:o->type==OBJ_SEMAPHORE?o->u.semaphore.count>0:o->u.thread.completed;}
static void consume(object *o) {if(o->type==OBJ_EVENT){if(!o->u.event.manual)o->u.event.signaled=0;}else if(o->type==OBJ_SEMAPHORE)--o->u.semaphore.count;}
static int satisfy_waiter(waiter *w) {
    if(w->done)return 0;
    unsigned first=w->count,ready=0;
    for(unsigned i=0;i<w->count;++i)if(signaled(w->selected[i])){if(first==w->count)first=i;++ready;}
    if((w->all && ready==w->count)||(!w->all && ready)) {
        if(w->all){for(unsigned i=0;i<w->count;++i)consume(w->selected[i]);w->result=0;}
        else{consume(w->selected[first]);w->result=first;}
        w->done=1;return 1;
    }
    return 0;
}
static void notify_waiters(void) {
    /* Complete waits while holding the object lock. A following ResetEvent
     * cannot revoke a wait already satisfied by SetEvent before rescheduling. */
    for(waiter *w=waiters;w;w=w->next)if(satisfy_waiter(w))pthread_cond_signal(w->wake);
}
static uint32_t wait_objects_extra(const uint64_t *ids,unsigned count,int all,uint32_t milliseconds,object *extra) {
    if((!count&&!extra) || count>(extra?63u:64u))return (uint32_t)error_value(ERROR_INVALID_PARAMETER,WAIT_FAILED);
    object *selected[64];unsigned retained=0;uint32_t result=WAIT_FAILED;
    for(unsigned i=0;i<count;++i) {
        handle *h=find_handle(ids[i]);
        if(!h || (h->obj->type!=OBJ_EVENT && h->obj->type!=OBJ_SEMAPHORE && h->obj->type!=OBJ_THREAD)){win64_set_error(ERROR_INVALID_HANDLE);goto done;}
        if(!(h->access&SYNCHRONIZE)){win64_set_error(ERROR_ACCESS_DENIED);goto done;}
        for(unsigned j=0;j<i;++j)if(ids[j]==ids[i] || selected[j]==h->obj){win64_set_error(ERROR_INVALID_PARAMETER);goto done;}
        selected[i]=h->obj;++selected[i]->refs;++retained;
    }
    if(extra){selected[count++]=extra;++extra->refs;++retained;}
    /* One monotonic condition variable per waiting thread, created on its first blocking wait. */
    static _Thread_local pthread_cond_t thread_wake;static _Thread_local int thread_wake_ready;
    waiter registration={selected,count,all,0,WAIT_FAILED,NULL,&thread_wake};
    if(satisfy_waiter(&registration)){result=registration.result;goto done;}
    if(!milliseconds){result=WAIT_TIMEOUT;goto done;}
    if(!thread_wake_ready){
        if(pthread_cond_init(&thread_wake,&wait_clock)){win64_set_error(ERROR_NOT_ENOUGH_MEMORY);goto done;}
        thread_wake_ready=1;
    }
    registration.next=waiters;waiters=&registration;
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    deadline.tv_sec+=milliseconds/1000;deadline.tv_nsec+=(milliseconds%1000)*1000000L;
    if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
    for(;;) {
        int status=milliseconds==UINT32_MAX?pthread_cond_wait(&thread_wake,&kernel_mutex):pthread_cond_timedwait(&thread_wake,&kernel_mutex,&deadline);
        if(registration.done){result=registration.result;break;}
        if(status==ETIMEDOUT){result=WAIT_TIMEOUT;break;}
        if(status){win64_set_error(ERROR_INVALID_DATA);break;}
    }
    waiter **link=&waiters;while(*link!=&registration)link=&(*link)->next;*link=registration.next;
done:
    for(unsigned i=0;i<retained;++i)release_object(selected[i]);return result;
}
static uint32_t wait_objects(const uint64_t *ids,unsigned count,int all,uint32_t milliseconds) {
    return wait_objects_extra(ids,count,all,milliseconds,NULL);
}
void *win64_kernel_queue_event_create(void) {
    pthread_once(&kernel_once,initialize);pthread_mutex_lock(&kernel_mutex);
    object *o=new_object(OBJ_EVENT,NULL);
    if(o){o->refs=1;o->u.event.manual=1;}else win64_set_error(ERROR_NOT_ENOUGH_MEMORY);
    pthread_mutex_unlock(&kernel_mutex);return o;
}
void win64_kernel_queue_event_set(void *token,int level) {
    pthread_mutex_lock(&kernel_mutex);object *o=token;o->u.event.signaled=!!level;
    if(level)notify_waiters();pthread_mutex_unlock(&kernel_mutex);
}
void win64_kernel_queue_event_destroy(void *token) {
    pthread_mutex_lock(&kernel_mutex);release_object(token);pthread_mutex_unlock(&kernel_mutex);
}
uint32_t win64_kernel_queue_wait(const uint64_t *ids,unsigned count,void *token,uint32_t milliseconds) {
    pthread_mutex_lock(&kernel_mutex);uint32_t result=wait_objects_extra(ids,count,0,milliseconds,token);
    pthread_mutex_unlock(&kernel_mutex);return result;
}

uint64_t win64_kernel_thread_create(void *payload,void (*destroy)(void *),int inherit,void **token) {
    pthread_once(&kernel_once,initialize);pthread_mutex_lock(&kernel_mutex);
    object *o=new_object(OBJ_THREAD,NULL);uint64_t id=0;
    if(o) {
        o->refs=1;o->u.thread.payload=payload;o->u.thread.destroy=destroy;
        id=new_handle(o,0x1fffff,inherit);
        if(id)*token=o;else{o->u.thread.destroy=NULL;release_object(o);}
    }else win64_set_error(ERROR_NOT_ENOUGH_MEMORY);
    pthread_mutex_unlock(&kernel_mutex);return id;
}
void *win64_kernel_thread_acquire(uint64_t id,uint32_t any_access,void **token) {
    pthread_mutex_lock(&kernel_mutex);handle *h=find_handle(id);void *payload=NULL;
    if(!h || h->obj->type!=OBJ_THREAD)win64_set_error(ERROR_INVALID_HANDLE);
    else if(any_access && !(h->access&any_access))win64_set_error(ERROR_ACCESS_DENIED);
    else{++h->obj->refs;*token=h->obj;payload=h->obj->u.thread.payload;}
    pthread_mutex_unlock(&kernel_mutex);return payload;
}
void win64_kernel_thread_release(void *token) {pthread_mutex_lock(&kernel_mutex);release_object(token);pthread_mutex_unlock(&kernel_mutex);}
void win64_kernel_thread_complete(void *token,uint32_t exit_code) {
    pthread_mutex_lock(&kernel_mutex);object *o=token;o->u.thread.completed=1;o->u.thread.exit_code=exit_code;
    notify_waiters();release_object(o);pthread_mutex_unlock(&kernel_mutex);
}
uint32_t win64_kernel_thread_exit_code(void *token) {
    pthread_mutex_lock(&kernel_mutex);object *o=token;uint32_t value=o->u.thread.completed?o->u.thread.exit_code:259;
    pthread_mutex_unlock(&kernel_mutex);return value;
}
uint64_t win64_kernel_thread_duplicate(void *token,uint32_t access,int inherit) {
    pthread_mutex_lock(&kernel_mutex);uint64_t id=new_handle(token,access,inherit);pthread_mutex_unlock(&kernel_mutex);return id;
}

/* All host paths are relative to one of these opened directories. The final
 * component and every parent are opened without following symlinks. */
typedef struct {int profile;char relative[PATH_MAX];} path_info;
static int prefix(const char *text,const char *start,int insensitive) {
    size_t n=strlen(start);return (insensitive?!strncasecmp(text,start,n):!strncmp(text,start,n)) && (!text[n]||text[n]=='/');
}
int win64_kernel_configure_roots(const char *game,const char *profile) {
    if(!game||!profile||game[0]!='/'||profile[0]!='/')return 0;
    char *game_path=realpath(game,NULL),*profile_path=realpath(profile,NULL);
    struct stat gs,ps;
    if(!game_path||!profile_path||strlen(game_path)<=1||strlen(profile_path)<=1||
       prefix(game_path,profile_path,0)||prefix(profile_path,game_path,0)||
       stat(game_path,&gs)||stat(profile_path,&ps)||!S_ISDIR(gs.st_mode)||!S_ISDIR(ps.st_mode)) {
        free(game_path);free(profile_path);return 0;
    }
    pthread_mutex_lock(&kernel_mutex);
    int accepted=!kernel_started&&!roots_configured;
    if(accepted){game_root=game_path;profile_root=profile_path;roots_configured=1;}
    pthread_mutex_unlock(&kernel_mutex);
    if(!accepted){free(game_path);free(profile_path);}return accepted;
}
static int translate_path(const char *input,path_info *out,int writing) {
    if(!input || !*input){win64_set_error(ERROR_PATH_NOT_FOUND);return 0;}
    size_t length=strlen(input);if(length>=PATH_MAX){win64_set_error(ERROR_FILENAME_EXCED_RANGE);return 0;}
    char normalized[PATH_MAX];for(size_t i=0;i<=length;++i)normalized[i]=input[i]=='\\'?'/':input[i];
    const char *p=normalized;if(!strncmp(p,"//?/",4))p+=4;
    out->profile=0;
    /* Exact C: names this guest's fixed C-drive current directory. Other
     * drive-relative names and the drive root remain outside this namespace. */
    if(length==2&&(p[0]=='C'||p[0]=='c')&&p[1]==':')p+=2;
    else if(prefix(p,guest_game,1))p+=strlen(guest_game);
    else if(prefix(p,guest_profile,1)){p+=strlen(guest_profile);out->profile=1;}
    else if(prefix(p,guest_profile_root,1)){p+=strlen(guest_profile_root);out->profile=1;}
    else if(prefix(p,guest_documents,1)){p+=strlen(guest_documents);out->profile=1;}
    else if(prefix(p,game_root,0))p+=strlen(game_root);
    else if(prefix(p,profile_root,0)){p+=strlen(profile_root);out->profile=1;}
    else if(*p=='/' || strchr(p,':')){win64_set_error(ERROR_PATH_NOT_FOUND);return 0;}
    if(writing && !out->profile){win64_set_error(ERROR_ACCESS_DENIED);return 0;}
    while(*p=='/')++p;
    size_t used=0,depth=0,starts[PATH_MAX/2];
    while(*p) {
        const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);
        if(n && !(n==1&&p[0]=='.')) {
            if(n==2&&p[0]=='.'&&p[1]=='.') {
                if(!depth){win64_set_error(ERROR_ACCESS_DENIED);return 0;}used=starts[--depth];
            } else {
                if(memchr(p,':',n)){win64_set_error(ERROR_NOT_SUPPORTED);return 0;}
                if(used+n+1>=sizeof(out->relative)){win64_set_error(ERROR_FILENAME_EXCED_RANGE);return 0;}
                starts[depth++]=used;if(used)out->relative[used++]='/';memcpy(out->relative+used,p,n);used+=n;
            }
        }
        if(!end)break;p=end+1;
    }
    out->relative[used]=0;
    if(!out->profile&&frostpunk_switch_ui&&frostpunk_switch_ui()&&!strcasecmp(out->relative,"templates.idx"))
        strcpy(out->relative,"templates-switch.idx");
    return 1;
}
static int resolve_component(int directory,const char *wanted,char actual[NAME_MAX+1]) {
    size_t n=strlen(wanted);if(!n || n>NAME_MAX){errno=ENAMETOOLONG;return 0;}
    struct stat st;
    if(!fstatat(directory,wanted,&st,AT_SYMLINK_NOFOLLOW)){memcpy(actual,wanted,n+1);return 1;}
    DIR *entries=fdopendir(dup(directory));if(!entries)return 0;rewinddir(entries);
    struct dirent *entry;int found=0;
    while((entry=readdir(entries)))if(!strcasecmp(entry->d_name,wanted)){strcpy(actual,entry->d_name);found=1;break;}
    closedir(entries);if(!found)errno=ENOENT;return found;
}
static int path_parent(const path_info *path,char basename[NAME_MAX+1]) {
    int fd=open(path->profile?profile_root:game_root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){win64_set_error(errno==ENOENT?ERROR_PATH_NOT_FOUND:posix_error(errno));return -1;}
    char relative[PATH_MAX];strcpy(relative,path->relative);char *part=relative;
    if(!*part){strcpy(basename,".");return fd;}
    for(;;) {
        char *slash=strchr(part,'/');if(slash)*slash=0;
        char actual[NAME_MAX+1];
        if(!resolve_component(fd,part,actual)) {
            if(!slash && errno==ENOENT && strlen(part)<=NAME_MAX){strcpy(basename,part);return fd;}
            uint32_t err=slash&&errno==ENOENT?ERROR_PATH_NOT_FOUND:posix_error(errno);close(fd);win64_set_error(err);return -1;
        }
        if(!slash){strcpy(basename,actual);return fd;}
        int next=openat(fd,actual,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);int saved=errno;close(fd);
        if(next<0){win64_set_error(saved==ENOENT?ERROR_PATH_NOT_FOUND:posix_error(saved));return -1;}
        fd=next;part=slash+1;
    }
}
static int guest_path(uint64_t address,int wide,int writing,path_info *path) {
    char *text=guest_text(address,wide);if(!text){if(!address)win64_set_error(ERROR_INVALID_PARAMETER);return 0;}
    if(!strcmp(text,"C:") || !strcmp(text,"C:/") || !strcmp(text,"C:\\")) {
        int saved_errno=errno;
        fprintf(stderr,"KERNEL_PATH_OBSERVED drive_C=1 separator=%d wide=%d writing=%d\n",text[2]!=0,wide,writing);
        errno=saved_errno;
    }
    int ok=translate_path(text,path,writing);free(text);return ok;
}
static int stat_path(const path_info *path,struct stat *st) {
    if(!path->profile&&fp_data_root_matches(game_root)&&fp_data_stat(path->relative,st))return 1;
    char name[NAME_MAX+1];int dir=path_parent(path,name);if(dir<0)return 0;
    int status=fstatat(dir,name,st,AT_SYMLINK_NOFOLLOW),saved=errno;close(dir);
    if(status){win64_set_error(posix_error(saved));return 0;}
    if(S_ISLNK(st->st_mode)){win64_set_error(ERROR_ACCESS_DENIED);return 0;}return 1;
}
static uint64_t create_file(CPU *c,int wide,int regular_only) {
    uint32_t desired=(uint32_t)c->rdx,share=(uint32_t)c->r8,disposition=(uint32_t)win64_argument(c,4),flags=(uint32_t)win64_argument(c,5);
    int inherit;if(!security(c->r9,&inherit))return INVALID_HANDLE;
    if(disposition<1||disposition>5 || share&~7u)return error_value(ERROR_INVALID_PARAMETER,INVALID_HANDLE);
    /* These flags affect cache policy or directory/reparse handling. Unknown
     * attributes (e.g. encryption) are rejected instead of silently ignored. */
    if(flags&~(0x80000000u|0x10000000u|0x08000000u|0x02000000u|0x00200000u|0x80u))
        return error_value(ERROR_NOT_SUPPORTED,INVALID_HANDLE);
    int readable=(desired&(GENERIC_READ|GENERIC_ALL|1))!=0,writable=(desired&(GENERIC_WRITE|GENERIC_ALL|2|4))!=0;
    int delete_access=(desired&(GENERIC_ALL|0x10000))!=0;
    if((disposition!=3 && disposition!=4) && !writable)return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);
    if(win64_argument(c,6))return error_value(ERROR_NOT_SUPPORTED,INVALID_HANDLE);
    int changing=writable || delete_access || disposition==1 || disposition==2 || disposition==5;
    path_info path;if(!guest_path(c->rcx,wide,changing,&path))return INVALID_HANDLE;
    char name[NAME_MAX+1];int dir=path_parent(&path,name);if(dir<0)return INVALID_HANDLE;
    fp_data_file archived;
    if(!path.profile&&!changing&&fp_data_root_matches(game_root)&&fp_data_open(path.relative,&archived)) {
        close(dir);struct stat *st=&archived.stat;
        for(object *other=objects;other;other=other->next)if(other->type==OBJ_FILE&&other->u.file.dev==st->st_dev&&other->u.file.ino==st->st_ino) {
            if((readable&&!(other->u.file.share&1))||(other->u.file.readable&&!(share&1))) {
                fp_data_close(&archived);return error_value(ERROR_SHARING_VIOLATION,INVALID_HANDLE);
            }
        }
        object *o=new_object(OBJ_FILE,NULL);if(!o){fp_data_close(&archived);return error_value(ERROR_NOT_ENOUGH_MEMORY,INVALID_HANDLE);}
        o->u.file.fd=archived.fd[0];o->u.file.readable=readable;o->u.file.share=share;
        o->u.file.dev=st->st_dev;o->u.file.ino=st->st_ino;o->u.file.zipped=1;o->u.file.data=archived;
        uint64_t id=new_handle(o,desired,inherit);if(!id){o->refs=1;release_object(o);return INVALID_HANDLE;}
        if(disposition==4)win64_set_error(ERROR_ALREADY_EXISTS);return id;
    }
    struct stat old;int existed=!fstatat(dir,name,&old,AT_SYMLINK_NOFOLLOW);
    if(disposition==4 && !path.profile && !existed){close(dir);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    if(existed && S_ISLNK(old.st_mode)){close(dir);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    if(regular_only&&existed&&!S_ISREG(old.st_mode)){close(dir);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    if(existed)for(object *o=objects;o;o=o->next)if(o->type==OBJ_FILE && o->u.file.dev==old.st_dev && o->u.file.ino==old.st_ino) {
        if((readable&&!(o->u.file.share&1))||(writable&&!(o->u.file.share&2))||(delete_access&&!(o->u.file.share&4))||
           (o->u.file.readable&&!(share&1))||(o->u.file.writable&&!(share&2))||(o->u.file.delete_access&&!(share&4))) {
            close(dir);return error_value(ERROR_SHARING_VIOLATION,INVALID_HANDLE);
        }
    }
    int mode=writable?(readable?O_RDWR:O_WRONLY):O_RDONLY;
    if(regular_only)mode|=O_NONBLOCK; /* A raced-in FIFO must not hold kernel_mutex waiting for a peer. */
    if((desired&4)&&!(desired&(GENERIC_WRITE|GENERIC_ALL|2)))mode|=O_APPEND;
    if(disposition==1)mode|=O_CREAT|O_EXCL;else if(disposition==2)mode|=O_CREAT|O_TRUNC;
    else if(disposition==4 && path.profile)mode|=O_CREAT;else if(disposition==5)mode|=O_TRUNC;
    if(flags&0x80000000u)mode|=O_SYNC;
    if(existed && S_ISDIR(old.st_mode) && !(flags&0x02000000u)){close(dir);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    int fd=openat(dir,name,mode|O_CLOEXEC|O_NOFOLLOW,0600),saved=errno;close(dir);
    if(fd<0)return error_value(regular_only&&saved==ENXIO?ERROR_ACCESS_DENIED:posix_error(saved),INVALID_HANDLE);
    struct stat st;if(fstat(fd,&st)){saved=errno;close(fd);return error_value(posix_error(saved),INVALID_HANDLE);}
    if(regular_only&&!S_ISREG(st.st_mode)){close(fd);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    object *o=new_object(OBJ_FILE,NULL);if(!o){close(fd);return error_value(ERROR_NOT_ENOUGH_MEMORY,INVALID_HANDLE);}
    o->u.file.fd=fd;o->u.file.readable=readable;o->u.file.writable=writable;o->u.file.delete_access=delete_access;
    o->u.file.share=share;o->u.file.dev=st.st_dev;o->u.file.ino=st.st_ino;
    uint64_t id=new_handle(o,desired,inherit);if(!id){o->refs=1;release_object(o);return INVALID_HANDLE;}
    if(disposition==2||disposition==4)win64_set_error(existed?ERROR_ALREADY_EXISTS:0);return id;
}
static object *file_object(uint64_t id) {
    handle *h=find_handle(id);if(!h||h->obj->type!=OBJ_FILE){win64_set_error(ERROR_INVALID_HANDLE);return NULL;}return h->obj;
}
static int file_stat(object *o,struct stat *st) {
    if(o->u.file.zipped){*st=o->u.file.data.stat;return 0;}return fstat(o->u.file.fd,st);
}
FILE *win64_kernel_stream_file(int fd,void *token,const char *mode) {
    object *o=token;
    if(o&&o->u.file.zipped){FILE *stream=fp_data_stream(&o->u.file.data);if(stream)close(fd);return stream;}
    return fdopen(fd,mode);
}
int win64_kernel_stream_open(uint64_t path,int wide,int writing,int *fd,void **token) {
    /* Secure UCRT rb shares reads; wb denies all sharing. Keep the original
     * object alive even though the FILE uses a duplicate descriptor. */
    uint64_t stack[8]={0};CPU c={0};uint32_t previous=win64_get_error();int result=0;
    *fd=-1;*token=NULL;c.rcx=path;c.rdx=writing?GENERIC_WRITE:GENERIC_READ;c.r8=writing?0:1;
    c.rsp=(uintptr_t)stack;stack[5]=writing?2:3;stack[6]=0x80;
    win64_region_add((uintptr_t)stack,sizeof(stack),0);
    pthread_once(&kernel_once,initialize);pthread_mutex_lock(&kernel_mutex);
    uint64_t id=create_file(&c,wide,1);
    if(id!=INVALID_HANDLE){
        object *o=file_object(id);int duplicate=fcntl(o->u.file.fd,F_DUPFD_CLOEXEC,0);
        if(duplicate>=0){++o->refs;*token=o;*fd=duplicate;}else result=errno==EMFILE||errno==ENFILE?24:12;
        close_handle(id);
    }else{
        switch(win64_get_error()){
        case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:result=2;break;
        case ERROR_ACCESS_DENIED:case ERROR_SHARING_VIOLATION:result=13;break;
        case ERROR_NOT_ENOUGH_MEMORY:result=12;break;
        case ERROR_FILENAME_EXCED_RANGE:result=38;break;
        case ERROR_NO_UNICODE_TRANSLATION:result=42;break;
        case ERROR_FILE_EXISTS:case ERROR_ALREADY_EXISTS:result=17;break;
        case ERROR_DISK_FULL:result=28;break;
        default:result=22;break;
        }
    }
    pthread_mutex_unlock(&kernel_mutex);win64_region_remove((uintptr_t)stack);win64_set_error(previous);return result;
}
void win64_kernel_stream_close(void *token) {
    if(!token)return;pthread_mutex_lock(&kernel_mutex);release_object(token);pthread_mutex_unlock(&kernel_mutex);
}
static uint32_t file_attributes(const struct stat *st) {
    uint32_t a=S_ISDIR(st->st_mode)?0x10u:0;if(!(st->st_mode&S_IWUSR))a|=1;return a?a:0x80u;
}
static uint64_t file_time(struct timespec t) {return UINT64_C(116444736000000000)+(uint64_t)t.tv_sec*10000000u+(uint64_t)t.tv_nsec/100;}
static void put32(unsigned char *p,size_t off,uint32_t value){memcpy(p+off,&value,4);}
static void put64(unsigned char *p,size_t off,uint64_t value){memcpy(p+off,&value,8);}
static void stat_times(unsigned char *p,size_t off,const struct stat *st) {
    /* Filesystems need not supply a creation time. Zero represents unavailable
     * birth time; POSIX metadata-change time is not mislabeled as creation. */
    put64(p,off,0);put64(p,off+8,file_time(st->st_atim));put64(p,off+16,file_time(st->st_mtim));
}
/* Windows SDK 10.0.26100.0 minwinbase.h: DWORD-aligned, no host wchar_t.
 * The obsolete Mac-only fields shown in some web renderings are absent. */
typedef struct {uint32_t fields[11];uint16_t filename[260],alternate[14];} find_data_w;
typedef struct {uint32_t fields[11];char filename[260],alternate[14];} find_data_a;
_Static_assert(sizeof(find_data_w)==592,"Win64 WIN32_FIND_DATAW ABI");
_Static_assert(sizeof(find_data_a)==320,"Win64 WIN32_FIND_DATAA ABI");
static int encode_find_name(unsigned char *out,const char *name,int wide) {
    int units=utf8_units(name);if(units<0){win64_set_error(ERROR_NO_UNICODE_TRANSLATION);return 0;}
    if(!wide){size_t n=strlen(name);if(n>=260){win64_set_error(ERROR_FILENAME_EXCED_RANGE);return 0;}memcpy(out,name,n+1);return 1;}
    if(units>=260){win64_set_error(ERROR_FILENAME_EXCED_RANGE);return 0;}
    const unsigned char *p=(const unsigned char *)name;size_t used=0;
    while(*p) {
        uint32_t cp;unsigned count;
        if(*p<0x80){cp=*p++;count=0;}else if(*p<0xe0){cp=*p++&31;count=1;}
        else if(*p<0xf0){cp=*p++&15;count=2;}else{cp=*p++&7;count=3;}
        while(count--)cp=(cp<<6)|(*p++&63);
        if(cp>=0x10000){uint16_t high=(uint16_t)(0xd800+((cp-0x10000)>>10));memcpy(out+used,&high,2);used+=2;cp=0xdc00+((cp-0x10000)&1023);}
        uint16_t ch=(uint16_t)cp;memcpy(out+used,&ch,2);used+=2;
    }
    uint16_t zero=0;memcpy(out+used,&zero,2);return 1;
}
static int find_next(object *o,uint64_t output,int wide,int first) {
    struct dirent *entry;char archive_name[NAME_MAX+1];
    for(;;) {
        struct stat st;const char *name;int archived=0;
        errno=0;entry=o->u.find.physical_done?NULL:readdir(o->u.find.directory);
        if(!entry){if(errno){win64_set_error(posix_error(errno));return 0;}o->u.find.physical_done=1;
            if(!o->u.find.ziproot||!fp_data_next(&o->u.find.zip_cursor,archive_name,sizeof(archive_name),&st)){win64_set_error(first?ERROR_FILE_NOT_FOUND:18);return 0;}
            struct stat actual;if(!fstatat(dirfd(o->u.find.directory),archive_name,&actual,AT_SYMLINK_NOFOLLOW))continue;
            name=archive_name;archived=1;
        }else name=entry->d_name;
        if(!strcmp(name,"..")&&!o->u.find.parent)continue;
        if(!o->u.find.all && (o->u.find.sensitive?strcmp(name,o->u.find.pattern):strcasecmp(name,o->u.find.pattern)))continue;
        if(!archived&&fstatat(dirfd(o->u.find.directory),name,&st,AT_SYMLINK_NOFOLLOW)) {
            if(errno==ENOENT)continue;win64_set_error(posix_error(errno));return 0;
        }
        if(S_ISLNK(st.st_mode)){win64_set_error(ERROR_NOT_SUPPORTED);return 0;}
        if(o->u.find.directories&&!S_ISDIR(st.st_mode))continue;
        unsigned char data[592]={0};put32(data,0,file_attributes(&st));stat_times(data,4,&st);
        uint64_t bytes=S_ISDIR(st.st_mode)?0:(uint64_t)st.st_size;
        put32(data,28,(uint32_t)(bytes>>32));put32(data,32,(uint32_t)bytes);
        if(!encode_find_name(data+44,name,wide))return 0;
        memcpy(win64_pointer(output,wide?sizeof(find_data_w):sizeof(find_data_a),1),data,wide?sizeof(find_data_w):sizeof(find_data_a));return 1;
    }
}
static uint64_t find_first(CPU *c,int wide,int extended) {
    uint64_t output=extended?c->r8:c->rdx;uint32_t info=extended?(uint32_t)c->rdx:0;
    uint32_t search=extended?(uint32_t)c->r9:0,flags=extended?(uint32_t)win64_argument(c,5):0;
    if(info>1||search>1||(extended&&win64_argument(c,4))||(flags&~3u))return error_value(ERROR_NOT_SUPPORTED,INVALID_HANDLE);
    char *text=guest_text(c->rcx,wide);if(!text)return INVALID_HANDLE;
    size_t length=strlen(text);
    if(!length||text[length-1]=='/'||text[length-1]=='\\'){free(text);return error_value(ERROR_INVALID_NAME,INVALID_HANDLE);}
    for(char *p=text;*p;++p)if(*p=='\\')*p='/';
    char *slash=strrchr(text,'/'),*pattern=slash?slash+1:text;int all=!strcmp(pattern,"*")||!strcmp(pattern,"*.*");
    if(!*pattern||!strcmp(pattern,".")||!strcmp(pattern,"..")){free(text);return error_value(ERROR_ACCESS_DENIED,INVALID_HANDLE);}
    if(!all&&(strpbrk(pattern,"*?<>\"")!=NULL)){free(text);return error_value(ERROR_NOT_SUPPORTED,INVALID_HANDLE);}
    char *saved_pattern=strdup(pattern);if(!saved_pattern){free(text);return error_value(ERROR_NOT_ENOUGH_MEMORY,INVALID_HANDLE);}
    if(slash)*slash=0;
    path_info path;int translated=translate_path(slash?text:".",&path,0);free(text);
    if(!translated){free(saved_pattern);return INVALID_HANDLE;}
    if(strpbrk(path.relative,"*?<>\"")){free(saved_pattern);return error_value(ERROR_NOT_SUPPORTED,INVALID_HANDLE);}
    char basename[NAME_MAX+1];int parent=path_parent(&path,basename);if(parent<0){free(saved_pattern);return INVALID_HANDLE;}
    int fd=openat(parent,basename,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),saved_errno=errno;close(parent);
    if(fd<0){free(saved_pattern);return error_value(saved_errno==ENOENT?ERROR_PATH_NOT_FOUND:posix_error(saved_errno),INVALID_HANDLE);}
    DIR *directory=fdopendir(fd);if(!directory){saved_errno=errno;close(fd);free(saved_pattern);return error_value(posix_error(saved_errno),INVALID_HANDLE);}
    object *o=new_object(OBJ_FIND,NULL);if(!o){closedir(directory);free(saved_pattern);return error_value(ERROR_NOT_ENOUGH_MEMORY,INVALID_HANDLE);}
    o->u.find.directory=directory;o->u.find.pattern=saved_pattern;o->u.find.all=all;o->u.find.directories=search==1;
    o->u.find.sensitive=(flags&1)!=0;o->u.find.parent=*path.relative!=0;
    o->u.find.ziproot=!path.profile&&!*path.relative&&fp_data_root_matches(game_root);
    if(!find_next(o,output,wide,1)){o->refs=1;release_object(o);return INVALID_HANDLE;}
    uint64_t id=new_handle(o,0,0);if(!id){o->refs=1;release_object(o);return INVALID_HANDLE;}return id;
}
static uint64_t move_file(uint64_t source,uint64_t destination,int wide) {
    path_info from,to;if(!guest_path(source,wide,1,&from)||!guest_path(destination,wide,1,&to))return 0;
    if(!*from.relative||!*to.relative)return error_value(ERROR_ACCESS_DENIED,0);
    char old_name[NAME_MAX+1],new_name[NAME_MAX+1];int old_parent=path_parent(&from,old_name);if(old_parent<0)return 0;
    int new_parent=path_parent(&to,new_name);if(new_parent<0){close(old_parent);return 0;}
    struct stat old,target;int status=fstatat(old_parent,old_name,&old,AT_SYMLINK_NOFOLLOW),saved=errno;
    if(status){close(old_parent);close(new_parent);return error_value(posix_error(saved),0);}
    if(S_ISLNK(old.st_mode)){close(old_parent);close(new_parent);return error_value(ERROR_ACCESS_DENIED,0);}
    for(object *o=objects;o;o=o->next)if(o->type==OBJ_FILE&&o->u.file.dev==old.st_dev&&o->u.file.ino==old.st_ino&&!(o->u.file.share&4)) {
        close(old_parent);close(new_parent);return error_value(ERROR_SHARING_VIOLATION,0);
    }
    int exists=!fstatat(new_parent,new_name,&target,AT_SYMLINK_NOFOLLOW);
    if(exists) {
        struct stat a,b;const char *literal=strrchr(to.relative,'/');literal=literal?literal+1:to.relative;
        int case_rename=old.st_dev==target.st_dev&&old.st_ino==target.st_ino&&strcmp(old_name,literal)&&!strcasecmp(old_name,literal)&&
            !fstat(old_parent,&a)&&!fstat(new_parent,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
        if(!case_rename){close(old_parent);close(new_parent);return error_value(S_ISLNK(target.st_mode)?ERROR_ACCESS_DENIED:ERROR_ALREADY_EXISTS,0);}
        strcpy(new_name,literal);
    }
    status=(int)syscall(SYS_renameat2,old_parent,old_name,new_parent,new_name,1u);saved=errno;
    close(old_parent);close(new_parent);
    if(status)return error_value(saved==EEXIST?ERROR_ALREADY_EXISTS:saved==EXDEV||saved==ENOSYS?ERROR_NOT_SUPPORTED:posix_error(saved),0);return 1;
}
static uint64_t create_mapping(CPU *c,int wide) {
    uint32_t protection=(uint32_t)c->r8;int inherit;
    if(!security(c->rdx,&inherit))return 0;
    char *name=guest_text(win64_argument(c,5),wide);if(win64_argument(c,5)&&!name)return 0;
    if(name&&!*name){free(name);name=NULL;}
    if(!valid_object_name(name)){free(name);return 0;}
    object *o=find_name(name);
    if(o){free(name);if(o->type!=OBJ_MAPPING)return error_value(ERROR_INVALID_HANDLE,0);uint64_t h=new_handle(o,0xf001f,inherit);if(h)win64_set_error(ERROR_ALREADY_EXISTS);return h;}
    uint32_t base=protection&255;
    if((protection&~0x080000ffu)||(base!=2&&base!=4&&base!=8)){free(name);return error_value(ERROR_NOT_SUPPORTED,0);}
    uint64_t bytes=((uint64_t)(uint32_t)c->r9<<32)|(uint32_t)win64_argument(c,4);
    if(bytes>INT64_MAX){free(name);return error_value(ERROR_INVALID_PARAMETER,0);}
    object *file=NULL;int fd=-1;
    if(c->rcx==INVALID_HANDLE) {
        if(!bytes){free(name);return error_value(ERROR_INVALID_PARAMETER,0);}
        fd=memfd_create("frostpunk-guest-mapping",MFD_CLOEXEC);
        if(fd>=0 && ftruncate(fd,(off_t)bytes)){int saved=errno;close(fd);fd=-1;errno=saved;}
    } else {
        file=file_object(c->rcx);if(!file){free(name);return 0;}
        if(!file->u.file.readable || (base==4&&!file->u.file.writable)){free(name);return error_value(ERROR_ACCESS_DENIED,0);}
        struct stat st;if(file_stat(file,&st)){free(name);return error_value(posix_error(errno),0);}
        if(!bytes)bytes=(uint64_t)st.st_size;
        if(!bytes){free(name);return error_value(1006,0);}
        if(bytes>(uint64_t)st.st_size && (!file->u.file.writable || ftruncate(file->u.file.fd,(off_t)bytes))) {
            free(name);return error_value(file->u.file.writable?posix_error(errno):ERROR_ACCESS_DENIED,0);
        }
        fd=dup(file->u.file.fd);
    }
    if(fd<0){free(name);return error_value(posix_error(errno),0);}
    o=new_object(OBJ_MAPPING,name);free(name);if(!o){close(fd);return error_value(ERROR_NOT_ENOUGH_MEMORY,0);}
    o->u.mapping.fd=fd;o->u.mapping.bytes=bytes;o->u.mapping.protection=base;o->u.mapping.file=file;if(file)++file->refs;
    uint64_t id=new_handle(o,0xf001f,inherit);if(!id){o->refs=1;release_object(o);}else win64_set_error(0);return id;
}
static uint64_t map_view(CPU *c,int extended) {
    handle *h=find_handle(c->rcx);if(!h||h->obj->type!=OBJ_MAPPING)return error_value(ERROR_INVALID_HANDLE,0);
    uint32_t access=(uint32_t)c->rdx;object *o=h->obj;
    if(access&~0xf001fu)return error_value(ERROR_NOT_SUPPORTED,0);
    int copying=(access&1)!=0 && !(access&2),writing=(access&2)!=0;
    if(!(access&7))return error_value(ERROR_INVALID_PARAMETER,0);
    if(writing && (o->u.mapping.protection!=4 || !(h->access&2)))return error_value(ERROR_ACCESS_DENIED,0);
    if(!writing && !(h->access&4) && !(copying&&(h->access&1)))return error_value(ERROR_ACCESS_DENIED,0);
    uint64_t offset=((uint64_t)(uint32_t)c->r8<<32)|(uint32_t)c->r9;
    uint64_t bytes=win64_argument(c,4),requested=extended?win64_argument(c,5):0;
    if(offset&0xffff || requested&0xffff)return error_value(ERROR_MAPPED_ALIGNMENT,0);
    if(offset>=o->u.mapping.bytes)return error_value(ERROR_INVALID_PARAMETER,0);
    if(!bytes)bytes=o->u.mapping.bytes-offset;
    if(bytes>o->u.mapping.bytes-offset || bytes>SIZE_MAX)return error_value(ERROR_INVALID_PARAMETER,0);
    int prot=PROT_READ|((writing||copying)?PROT_WRITE:0);
    void *native_base=NULL;size_t native_bytes=(size_t)bytes;
    void *base=o->u.mapping.file&&o->u.mapping.file->u.file.zipped
        ?fp_data_map(&o->u.mapping.file->u.file.data,offset,(size_t)bytes,prot,(void *)(uintptr_t)requested,&native_base,&native_bytes)
        :mmap((void *)(uintptr_t)requested,(size_t)bytes,prot,copying?MAP_PRIVATE:MAP_SHARED,o->u.mapping.fd,(off_t)offset);
    if(base==MAP_FAILED)return error_value(posix_error(errno),0);
    if(!native_base)native_base=base;
    if(requested && (uint64_t)(uintptr_t)base!=requested){munmap(native_base,native_bytes);return error_value(ERROR_INVALID_PARAMETER,0);}
    view *v=malloc(sizeof(*v));if(!v){munmap(native_base,native_bytes);return error_value(ERROR_NOT_ENOUGH_MEMORY,0);}
    *v=(view){base,(size_t)bytes,o,views,native_base,native_bytes};views=v;++o->refs;
    win64_region_add((uint64_t)(uintptr_t)base,(size_t)bytes,writing||copying);return (uint64_t)(uintptr_t)base;
}

#define ARG(i) win64_argument(c,(i))
#define RETURN(value) do{c->rax=(uint64_t)(value);pthread_mutex_unlock(&kernel_mutex);return 1;}while(0)
int win64_kernel_call(CPU *c,const char *name) {
    pthread_once(&kernel_once,initialize);pthread_mutex_lock(&kernel_mutex);
    if(!strcmp(name,"QueryPerformanceCounter")){pthread_mutex_unlock(&kernel_mutex);return 0;}
    int wide=name[strlen(name)-1]=='W';
    if(!strcmp(name,"CreateEventA")||!strcmp(name,"CreateEventW"))RETURN(create_sync(OBJ_EVENT,c->rcx,(int32_t)c->rdx,(int32_t)c->r8,c->r9,wide,0x1f0003));
    if(!strcmp(name,"CreateEventExA")||!strcmp(name,"CreateEventExW")) {
        if(c->r8&~3u)RETURN(error_value(ERROR_INVALID_PARAMETER,0));
        RETURN(create_sync(OBJ_EVENT,c->rcx,(c->r8&1)!=0,(c->r8&2)!=0,c->rdx,wide,(uint32_t)c->r9));
    }
    if(!strcmp(name,"OpenEventA")||!strcmp(name,"OpenEventW"))RETURN(open_named(OBJ_EVENT,(uint32_t)c->rcx,c->rdx!=0,c->r8,wide));
    if(!strcmp(name,"SetEvent")||!strcmp(name,"ResetEvent")) {
        handle *h=find_handle(c->rcx);if(!h||h->obj->type!=OBJ_EVENT)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        if(!(h->access&2))RETURN(error_value(ERROR_ACCESS_DENIED,0));
        h->obj->u.event.signaled=!strcmp(name,"SetEvent");notify_waiters();RETURN(1);
    }
    if(!strcmp(name,"CreateSemaphoreA")||!strcmp(name,"CreateSemaphoreW"))RETURN(create_sync(OBJ_SEMAPHORE,c->rcx,(int32_t)c->rdx,(int32_t)c->r8,c->r9,wide,0x1f0003));
    if(!strcmp(name,"CreateSemaphoreExA")||!strcmp(name,"CreateSemaphoreExW")) {
        if(ARG(4))RETURN(error_value(ERROR_INVALID_PARAMETER,0));
        RETURN(create_sync(OBJ_SEMAPHORE,c->rcx,(int32_t)c->rdx,(int32_t)c->r8,c->r9,wide,(uint32_t)ARG(5)));
    }
    if(!strcmp(name,"OpenSemaphoreA")||!strcmp(name,"OpenSemaphoreW"))RETURN(open_named(OBJ_SEMAPHORE,(uint32_t)c->rcx,c->rdx!=0,c->r8,wide));
    if(!strcmp(name,"ReleaseSemaphore")) {
        handle *h=find_handle(c->rcx);if(!h||h->obj->type!=OBJ_SEMAPHORE)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        if(!(h->access&2))RETURN(error_value(ERROR_ACCESS_DENIED,0));
        int32_t amount=(int32_t)c->rdx;if(amount<=0)RETURN(error_value(ERROR_INVALID_PARAMETER,0));
        if(amount>h->obj->u.semaphore.maximum-h->obj->u.semaphore.count)RETURN(error_value(ERROR_TOO_MANY_POSTS,0));
        if(c->r8)store32(c->r8,(uint32_t)h->obj->u.semaphore.count);
        h->obj->u.semaphore.count+=amount;notify_waiters();RETURN(1);
    }
    if(!strcmp(name,"WaitForSingleObject")||!strcmp(name,"WaitForSingleObjectEx")) {
        if(!strcmp(name,"WaitForSingleObjectEx")&&c->r8)RETURN(error_value(ERROR_NOT_SUPPORTED,WAIT_FAILED));
        uint64_t id=c->rcx;RETURN(wait_objects(&id,1,0,(uint32_t)c->rdx));
    }
    if(!strcmp(name,"WaitForMultipleObjects")||!strcmp(name,"WaitForMultipleObjectsEx")) {
        if(!strcmp(name,"WaitForMultipleObjectsEx")&&ARG(4))RETURN(error_value(ERROR_NOT_SUPPORTED,WAIT_FAILED));
        unsigned count=(uint32_t)c->rcx;if(!count||count>64)RETURN(error_value(ERROR_INVALID_PARAMETER,WAIT_FAILED));
        uint64_t ids[64];memcpy(ids,win64_pointer(c->rdx,count*8,0),count*8);RETURN(wait_objects(ids,count,c->r8!=0,(uint32_t)c->r9));
    }
    if(!strcmp(name,"CloseHandle")){handle *h=find_handle(c->rcx);if(h&&h->obj->type==OBJ_FIND)RETURN(error_value(ERROR_INVALID_HANDLE,0));RETURN(close_handle(c->rcx));}
    if(!strcmp(name,"GetHandleInformation")) {handle *h=find_handle(c->rcx);if(!h)RETURN(error_value(ERROR_INVALID_HANDLE,0));store32(c->rdx,h->flags);RETURN(1);}
    if(!strcmp(name,"SetHandleInformation")) {
        handle *h=find_handle(c->rcx);if(!h)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        if(c->rdx&~3u)RETURN(error_value(ERROR_INVALID_PARAMETER,0));h->flags=(h->flags&~(uint32_t)c->rdx)|((uint32_t)c->r8&(uint32_t)c->rdx);RETURN(1);
    }
    if(!strcmp(name,"DuplicateHandle")) {
        if(c->rcx!=UINT64_MAX||c->r8!=UINT64_MAX)RETURN(error_value(ERROR_NOT_SUPPORTED,0));
        handle *h=find_handle(c->rdx);if(!h)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        if(h->obj->type==OBJ_FIND)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        uint32_t options=(uint32_t)ARG(6);if(options&~3u)RETURN(error_value(ERROR_INVALID_PARAMETER,0));
        uint32_t access=options&2?h->access:(uint32_t)ARG(4);if((access&h->access)!=access)RETURN(error_value(ERROR_ACCESS_DENIED,0));
        uint64_t id=new_handle(h->obj,access,ARG(5)!=0);if(!id)RETURN(0);store64(c->r9,id);
        if(options&1)close_handle(c->rdx);RETURN(1);
    }
    if(!strcmp(name,"CreateFileMappingA")||!strcmp(name,"CreateFileMappingW"))RETURN(create_mapping(c,wide));
    if(!strcmp(name,"OpenFileMappingA")||!strcmp(name,"OpenFileMappingW"))RETURN(open_named(OBJ_MAPPING,(uint32_t)c->rcx,c->rdx!=0,c->r8,wide));
    if(!strcmp(name,"MapViewOfFile")||!strcmp(name,"MapViewOfFileEx"))RETURN(map_view(c,!strcmp(name,"MapViewOfFileEx")));
    if(!strcmp(name,"UnmapViewOfFile")) {
        view **p=&views;while(*p && (uint64_t)(uintptr_t)(*p)->base!=c->rcx)p=&(*p)->next;
        if(!*p)RETURN(error_value(ERROR_INVALID_PARAMETER,0));view *v=*p;
        win64_region_mutation_begin();
        if(munmap(v->native_base,v->native_bytes)){int saved=errno;win64_region_mutation_end();RETURN(error_value(posix_error(saved),0));}
        *p=v->next;win64_region_remove(c->rcx);win64_region_mutation_end();release_object(v->obj);free(v);RETURN(1);
    }
    if(!strcmp(name,"FlushViewOfFile")) {
        view *v;for(v=views;v;v=v->next)if(c->rcx>=(uint64_t)(uintptr_t)v->base&&c->rcx-(uint64_t)(uintptr_t)v->base<v->bytes)break;
        if(!v)RETURN(error_value(ERROR_INVALID_PARAMETER,0));size_t remain=v->bytes-(size_t)(c->rcx-(uint64_t)(uintptr_t)v->base),bytes=c->rdx?(size_t)c->rdx:remain;
        if(bytes>remain)RETURN(error_value(ERROR_INVALID_PARAMETER,0));size_t page=(size_t)sysconf(_SC_PAGESIZE);
        uintptr_t start=(uintptr_t)c->rcx&~(page-1);size_t count=bytes+((uintptr_t)c->rcx-start);
        RETURN(msync((void *)start,count,MS_SYNC)?error_value(posix_error(errno),0):1);
    }
    if(!strcmp(name,"CreateFileA")||!strcmp(name,"CreateFileW"))RETURN(create_file(c,wide,0));
    if(!strcmp(name,"FindFirstFileA")||!strcmp(name,"FindFirstFileW")||!strcmp(name,"FindFirstFileExA")||!strcmp(name,"FindFirstFileExW"))RETURN(find_first(c,wide,strstr(name,"Ex")!=NULL));
    if(!strcmp(name,"FindNextFileA")||!strcmp(name,"FindNextFileW")||!strcmp(name,"FindClose")) {
        handle *h=find_handle(c->rcx);if(!h||h->obj->type!=OBJ_FIND)RETURN(error_value(ERROR_INVALID_HANDLE,0));
        if(!strcmp(name,"FindClose"))RETURN(close_handle(c->rcx));RETURN(find_next(h->obj,c->rdx,wide,0));
    }
    if(!strcmp(name,"MoveFileA")||!strcmp(name,"MoveFileW"))RETURN(move_file(c->rcx,c->rdx,wide));
    if(!strcmp(name,"ReadFile")||!strcmp(name,"WriteFile")) {
        int writing=!strcmp(name,"WriteFile");object *o=file_object(c->rcx);if(!o)RETURN(0);
        if(c->r9)store32(c->r9,0);if(ARG(4))RETURN(error_value(ERROR_NOT_SUPPORTED,0));
        if(writing?!o->u.file.writable:!o->u.file.readable)RETURN(error_value(ERROR_ACCESS_DENIED,0));
        uint32_t count=(uint32_t)c->r8;void *buffer=count?win64_pointer(c->rdx,count,!writing):NULL;
        /* The object reference keeps the descriptor open while other threads
         * use the kernel (events, waits, timers) during the transfer. */
        int fd=o->u.file.fd,zipped=o->u.file.zipped;uint64_t position=o->u.file.position;
        if(zipped){uint64_t left=position<o->u.file.data.size?o->u.file.data.size-position:0;if(count>left)count=(uint32_t)left;o->u.file.position+=count;}
        ++o->refs;pthread_mutex_unlock(&kernel_mutex);
        ssize_t bytes;do{bytes=zipped?(ssize_t)fp_data_read(&o->u.file.data,buffer,count,position):writing?write(fd,buffer,count):read(fd,buffer,count);}while(bytes<0&&errno==EINTR);
        int saved=errno;pthread_mutex_lock(&kernel_mutex);release_object(o);
        if(bytes<0)RETURN(error_value(posix_error(saved),0));if(c->r9)store32(c->r9,(uint32_t)bytes);RETURN(1);
    }
    if(!strcmp(name,"GetFileSize")||!strcmp(name,"GetFileSizeEx")) {
        object *o=file_object(c->rcx);if(!o)RETURN(!strcmp(name,"GetFileSize")?UINT32_MAX:0);
        struct stat st;if(file_stat(o,&st))RETURN(error_value(posix_error(errno),!strcmp(name,"GetFileSize")?UINT32_MAX:0));
        if(!strcmp(name,"GetFileSizeEx")){store64(c->rdx,(uint64_t)st.st_size);RETURN(1);}
        if(c->rdx)store32(c->rdx,(uint32_t)((uint64_t)st.st_size>>32));win64_set_error(0);RETURN((uint32_t)st.st_size);
    }
    if(!strcmp(name,"SetFilePointer")||!strcmp(name,"SetFilePointerEx")) {
        object *o=file_object(c->rcx);int extended=!strcmp(name,"SetFilePointerEx");if(!o)RETURN(extended?0:UINT32_MAX);
        uint32_t method=(uint32_t)c->r9;if(method>2)RETURN(error_value(ERROR_INVALID_PARAMETER,extended?0:UINT32_MAX));
        int64_t distance=(int64_t)c->rdx;
        if(!extended) {distance=(int32_t)c->rdx;if(c->r8){int32_t high;memcpy(&high,win64_pointer(c->r8,4,0),4);distance=(int64_t)(((uint64_t)(uint32_t)high<<32)|(uint32_t)c->rdx);}}
        off_t result;
        if(o->u.file.zipped){uint64_t base=method==0?0:method==1?o->u.file.position:o->u.file.data.size;
            if((distance<0&&(uint64_t)(-(distance+1))+1>base)||(distance>=0&&(uint64_t)distance>INT64_MAX-base))RETURN(error_value(ERROR_INVALID_PARAMETER,extended?0:UINT32_MAX));
            result=(off_t)(base+(uint64_t)distance);o->u.file.position=(uint64_t)result;
        }else result=lseek(o->u.file.fd,(off_t)distance,(int)method);
        if(result<0)RETURN(error_value(posix_error(errno),extended?0:UINT32_MAX));
        if(extended){if(c->r8)store64(c->r8,(uint64_t)result);RETURN(1);}
        if(c->r8)store32(c->r8,(uint32_t)((uint64_t)result>>32));win64_set_error(0);RETURN((uint32_t)result);
    }
    if(!strcmp(name,"SetEndOfFile")||!strcmp(name,"FlushFileBuffers")) {
        object *o=file_object(c->rcx);if(!o)RETURN(0);if(!o->u.file.writable)RETURN(error_value(ERROR_ACCESS_DENIED,0));
        int status;if(!strcmp(name,"FlushFileBuffers"))status=fsync(o->u.file.fd);
        else{off_t position=lseek(o->u.file.fd,0,SEEK_CUR);status=position<0?-1:ftruncate(o->u.file.fd,position);}
        RETURN(status?error_value(posix_error(errno),0):1);
    }
    if(!strcmp(name,"GetFileType")){object *o=file_object(c->rcx);if(!o)RETURN(0);struct stat st;if(file_stat(o,&st))RETURN(error_value(posix_error(errno),0));RETURN(S_ISREG(st.st_mode)||S_ISDIR(st.st_mode)?1:0);}
    if(!strcmp(name,"GetFileInformationByHandle")) {
        object *o=file_object(c->rcx);if(!o)RETURN(0);struct stat st;if(file_stat(o,&st))RETURN(error_value(posix_error(errno),0));
        unsigned char *out=win64_pointer(c->rdx,52,1);memset(out,0,52);put32(out,0,file_attributes(&st));stat_times(out,4,&st);
        put32(out,28,(uint32_t)st.st_dev);put32(out,32,(uint32_t)((uint64_t)st.st_size>>32));put32(out,36,(uint32_t)st.st_size);
        put32(out,40,(uint32_t)st.st_nlink);put32(out,44,(uint32_t)((uint64_t)st.st_ino>>32));put32(out,48,(uint32_t)st.st_ino);RETURN(1);
    }
    if(!strcmp(name,"GetFileTime")) {
        object *o=file_object(c->rcx);if(!o)RETURN(0);struct stat st;if(file_stat(o,&st))RETURN(error_value(posix_error(errno),0));
        if(c->rdx)store64(c->rdx,0);if(c->r8)store64(c->r8,file_time(st.st_atim));if(c->r9)store64(c->r9,file_time(st.st_mtim));RETURN(1);
    }
    if(!strcmp(name,"GetFileAttributesA")||!strcmp(name,"GetFileAttributesW")||!strcmp(name,"GetFileAttributesExA")||!strcmp(name,"GetFileAttributesExW")) {
        int extended=strstr(name,"Ex")!=NULL;path_info path;if(!guest_path(c->rcx,wide,0,&path))RETURN(extended?0:UINT32_MAX);
        struct stat st;if(!stat_path(&path,&st))RETURN(extended?0:UINT32_MAX);
        if(!extended)RETURN(file_attributes(&st));if(c->rdx)RETURN(error_value(ERROR_INVALID_PARAMETER,0));
        unsigned char *out=win64_pointer(c->r8,36,1);memset(out,0,36);put32(out,0,file_attributes(&st));stat_times(out,4,&st);
        put32(out,28,(uint32_t)((uint64_t)st.st_size>>32));put32(out,32,(uint32_t)st.st_size);RETURN(1);
    }
    if(!strcmp(name,"CreateDirectoryA")||!strcmp(name,"CreateDirectoryW")||!strcmp(name,"DeleteFileA")||!strcmp(name,"DeleteFileW")||!strcmp(name,"RemoveDirectoryA")||!strcmp(name,"RemoveDirectoryW")) {
        path_info path;if(!guest_path(c->rcx,wide,1,&path))RETURN(0);
        int creating=!strncmp(name,"CreateDirectory",15);int inherit;if(creating&&!security(c->rdx,&inherit))RETURN(0);
        if(!*path.relative){if(!creating)RETURN(error_value(ERROR_ACCESS_DENIED,0));RETURN(mkdir(profile_root,0700)?error_value(errno==EEXIST?ERROR_ALREADY_EXISTS:posix_error(errno),0):1);}
        char basename[NAME_MAX+1];int parent=path_parent(&path,basename);if(parent<0)RETURN(0);
        struct stat st;int exists=!fstatat(parent,basename,&st,AT_SYMLINK_NOFOLLOW);
        if(exists&&S_ISLNK(st.st_mode)){close(parent);RETURN(error_value(ERROR_ACCESS_DENIED,0));}
        if(exists&&!creating)for(object *o=objects;o;o=o->next)
            if(o->type==OBJ_FILE&&o->u.file.dev==st.st_dev&&o->u.file.ino==st.st_ino&&!(o->u.file.share&4)) {
                close(parent);RETURN(error_value(ERROR_SHARING_VIOLATION,0));
            }
        int status=creating?mkdirat(parent,basename,0700):unlinkat(parent,basename,!strncmp(name,"RemoveDirectory",15)?AT_REMOVEDIR:0),saved=errno;
        close(parent);RETURN(status?error_value(creating&&saved==EEXIST?ERROR_ALREADY_EXISTS:posix_error(saved),0):1);
    }
    pthread_mutex_unlock(&kernel_mutex);return 0;
}
#undef RETURN
#undef ARG

/* Tests call the public Win64 ABI dispatcher with registered guest buffers.
 * No Steam objects or original game/profile files are created by this test. */
static void test_require(int ok,const char *what) {if(!ok){fprintf(stderr,"STOP kernel self-test %s error=%u\n",what,win64_get_error());exit(73);}}
static uint64_t test_call(const char *name,const uint64_t args[8]) {
    CPU c={0};uint64_t stack[16]={0};c.rsp=(uint64_t)(uintptr_t)stack;
    c.rcx=args[0];c.rdx=args[1];c.r8=args[2];c.r9=args[3];for(unsigned i=4;i<8;++i)stack[5+i-4]=args[i];
    win64_region_add(c.rsp,sizeof(stack),1);test_require(win64_kernel_call(&c,name),"unhandled test API");
    test_require(c.rsp==(uint64_t)(uintptr_t)stack,"kernel must preserve guest RSP");win64_region_remove(c.rsp);return c.rax;
}
static void *test_signal_event(void *argument) {
    struct timespec delay={0,1000000};while(nanosleep(&delay,&delay)&&errno==EINTR){}
    CPU c={0};c.rcx=*(const uint64_t *)argument;
    return (void *)(uintptr_t)(!win64_kernel_call(&c,"SetEvent")||c.rax!=1);
}
static void *test_signal_reset_before_resume(void *argument) {
    uint64_t id=*(const uint64_t *)argument;
    for(unsigned attempt=0;attempt<1000;++attempt) {
        pthread_mutex_lock(&kernel_mutex);handle *h=find_handle(id);int pending=0;
        if(h)for(waiter *w=waiters;w;w=w->next)for(unsigned i=0;i<w->count;++i)
            if(!w->done&&w->selected[i]==h->obj)pending=1;
        if(pending) {
            /* Force the valid schedule in which SetEvent and ResetEvent both
             * finish before the waiting native thread can resume. */
            h->obj->u.event.signaled=1;notify_waiters();h->obj->u.event.signaled=0;
            pthread_mutex_unlock(&kernel_mutex);return NULL;
        }
        pthread_mutex_unlock(&kernel_mutex);struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
    return (void *)1;
}
static int mapping_nx(uint64_t address) {
    FILE *f=fopen("/proc/self/maps","r");if(!f)return 0;char line[512],permissions[5];uint64_t first,last;int result=0;
    while(fgets(line,sizeof(line),f))if(sscanf(line,"%"SCNx64"-%"SCNx64" %4s",&first,&last,permissions)==3 && address>=first&&address<last){result=permissions[2]!='x';break;}
    fclose(f);return result;
}
#define CALL(name,...) test_call((name),(uint64_t[8]){__VA_ARGS__})
#define ADDRESS(p) ((uint64_t)(uintptr_t)(p))
void win64_kernel_roots_self_test(void) {
    char base[]="/data/local/tmp/frostpunk/roots-XXXXXX",game[PATH_MAX],profile[PATH_MAX];
    test_require(mkdtemp(base)!=NULL,"root fixture directory");
    test_require(snprintf(game,sizeof(game),"%s/game",base)>0 && snprintf(profile,sizeof(profile),"%s/profile",base)>0,"root fixture paths");
    test_require(!mkdir(game,0700)&&!mkdir(profile,0700),"root fixture children");
    pid_t child=fork();test_require(child>=0,"root fixture fork");
    if(!child) {
        path_info path;int ok=!win64_kernel_configure_roots(base,profile) &&
            !win64_kernel_configure_roots("relative",profile) &&
            win64_kernel_configure_roots(game,profile) && !win64_kernel_configure_roots(game,profile) &&
            translate_path(guest_profile,&path,1) && path.profile && !*path.relative &&
            translate_path("c:/FROSTPUNKPROFILE/AppData",&path,1) && path.profile && !strcmp(path.relative,"AppData") &&
            !translate_path("C:/FrostpunkProfileEvil",&path,1) &&
            !translate_path("C:/FrostpunkProfile/../escape",&path,1) && win64_get_error()==ERROR_ACCESS_DENIED &&
            translate_path(game,&path,0) && !path.profile && !*path.relative &&
            translate_path(profile,&path,1) && path.profile && !*path.relative &&
            translate_path("C:",&path,0) && !path.profile && !*path.relative &&
            translate_path("c:",&path,0) && !path.profile && !*path.relative &&
            !translate_path("C:relative",&path,0) && win64_get_error()==ERROR_PATH_NOT_FOUND &&
            !translate_path("C:/",&path,0) && win64_get_error()==ERROR_PATH_NOT_FOUND &&
            !translate_path("C:",&path,1) && win64_get_error()==ERROR_ACCESS_DENIED &&
            !translate_path(game,&path,1) && win64_get_error()==ERROR_ACCESS_DENIED;
        pthread_once(&kernel_once,initialize);
        ok=ok&&!win64_kernel_configure_roots(game,profile);_exit(ok?0:1);
    }
    int status;test_require(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),"host root configuration/alias/isolation");
    test_require(!rmdir(game)&&!rmdir(profile)&&!rmdir(base),"root fixture cleanup");
    fprintf(stderr,"KERNEL_ROOTS_SELF_TEST existing_disjoint_directories=1 overlap_rejected=1 immutable_after_first_API=1 physical_and_Windows_aliases=1 game_writes_denied=1 passed\n");
}
void win64_kernel_self_test(void) {
    uint32_t saved_error=win64_get_error();
    char event_name[]="Local\\FrostpunkKernelSelfTestEvent",mapping_name[]="Local\\FrostpunkKernelSelfTestMapping";
    win64_region_add(ADDRESS(event_name),sizeof(event_name),0);win64_region_add(ADDRESS(mapping_name),sizeof(mapping_name),0);
    test_require(!CALL("OpenEventA",SYNCHRONIZE,0,ADDRESS(event_name))&&win64_get_error()==2,"absent event must remain absent");
    uint64_t event=CALL("CreateEventA",0,0,0,ADDRESS(event_name));test_require(event!=0,"create event");
    test_require(CALL("WaitForSingleObject",event,0)==WAIT_TIMEOUT,"unsignaled event timeout");
    test_require(CALL("SetEvent",event)==1&&CALL("WaitForSingleObject",event,0)==0,"signal auto event");
    test_require(CALL("WaitForSingleObject",event,0)==WAIT_TIMEOUT,"auto event consumption");
    uint64_t alias=CALL("CreateEventA",0,1,1,ADDRESS(event_name));test_require(alias&&win64_get_error()==183,"named create alias");
    test_require(CALL("WaitForSingleObject",alias,0)==WAIT_TIMEOUT,"existing event ignores initial state");
    test_require(CALL("CloseHandle",event)&&CALL("CloseHandle",alias),"event close");
    test_require(!CALL("OpenEventA",SYNCHRONIZE,0,ADDRESS(event_name))&&win64_get_error()==2,"named lifetime");
    uint64_t asynchronous=CALL("CreateEventA",0,0,0,0);pthread_t signaler=0;
    test_require(asynchronous&&!pthread_create(&signaler,NULL,test_signal_event,&asynchronous),"native wake helper");
    test_require(CALL("WaitForSingleObject",asynchronous,1000)==0,"blocking event wake");
    void *thread_result;test_require(!pthread_join(signaler,&thread_result)&&!thread_result&&CALL("CloseHandle",asynchronous),"wake helper join");
    uint64_t immediate_reset=CALL("CreateEventA",0,1,0,0);signaler=0;
    test_require(immediate_reset&&!pthread_create(&signaler,NULL,test_signal_reset_before_resume,&immediate_reset),"manual reset schedule helper");
    test_require(CALL("WaitForSingleObject",immediate_reset,1000)==0,"completed manual wake survives immediate reset");
    test_require(!pthread_join(signaler,&thread_result)&&!thread_result&&CALL("WaitForSingleObject",immediate_reset,0)==WAIT_TIMEOUT,"manual event is reset after completed wake");
    test_require(CALL("CloseHandle",immediate_reset),"close reset schedule event");
    uint64_t pair[2]={CALL("CreateEventA",0,0,1,0),CALL("CreateEventA",0,0,0,0)};
    win64_region_add(ADDRESS(pair),sizeof(pair),0);
    test_require(CALL("WaitForMultipleObjects",2,ADDRESS(pair),1,0)==WAIT_TIMEOUT&&CALL("WaitForSingleObject",pair[0],0)==0,"wait-all cannot consume early signal");
    test_require(CALL("SetEvent",pair[0])&&CALL("SetEvent",pair[1])&&CALL("WaitForMultipleObjects",2,ADDRESS(pair),1,0)==0,"atomic wait-all satisfaction");
    test_require(CALL("WaitForSingleObject",pair[0],0)==WAIT_TIMEOUT&&CALL("WaitForSingleObject",pair[1],0)==WAIT_TIMEOUT,"wait-all auto event consumption");
    test_require(CALL("CloseHandle",pair[0])&&CALL("CloseHandle",pair[1]),"close wait-all handles");win64_region_remove(ADDRESS(pair));
    uint64_t manual=CALL("CreateEventA",0,1,1,0);
    test_require(CALL("WaitForSingleObject",manual,0)==0&&CALL("WaitForSingleObject",manual,0)==0,"manual state persists");
    test_require(CALL("ResetEvent",manual)&&CALL("WaitForSingleObject",manual,0)==WAIT_TIMEOUT,"manual reset");
    uint64_t semaphore=CALL("CreateSemaphoreA",0,1,2,0);
    test_require(semaphore&&CALL("WaitForSingleObject",semaphore,0)==0,"semaphore initial count");
    test_require(CALL("ReleaseSemaphore",semaphore,2,0)==1,"semaphore release");
    test_require(!CALL("ReleaseSemaphore",semaphore,1,0)&&win64_get_error()==ERROR_TOO_MANY_POSTS,"semaphore maximum");
    test_require(CALL("WaitForSingleObject",semaphore,0)==0&&CALL("WaitForSingleObject",semaphore,0)==0&&CALL("WaitForSingleObject",semaphore,0)==WAIT_TIMEOUT,"semaphore consumption");
    test_require(CALL("CloseHandle",manual)&&CALL("CloseHandle",semaphore),"close synchronization handles");
    test_require(!CALL("OpenFileMappingA",4,0,ADDRESS(mapping_name))&&win64_get_error()==2,"absent mapping must remain absent");
    uint64_t mapping=CALL("CreateFileMappingA",INVALID_HANDLE,0,4,0,65536,ADDRESS(mapping_name));test_require(mapping,"anonymous named mapping");
    uint64_t reopened=CALL("OpenFileMappingA",6,0,ADDRESS(mapping_name));test_require(reopened,"open own mapping");
    uint64_t first=CALL("MapViewOfFile",mapping,6,0,0,0),second=CALL("MapViewOfFile",reopened,4,0,0,0);
    test_require(first&&second,"map views");
    test_require(mapping_nx(first)&&mapping_nx(second),"mapped views must be NX");
    const unsigned char *zero=win64_pointer(first,65536,0);for(unsigned i=0;i<65536;++i)test_require(!zero[i],"mapping zero initialization");
    *(uint32_t *)win64_pointer(first+12,4,1)=0x12345678;uint32_t value;memcpy(&value,win64_pointer(second+12,4,0),4);
    test_require(value==0x12345678,"coherent shared views");
    test_require(!CALL("MapViewOfFile",mapping,4,0,1,4096)&&win64_get_error()==ERROR_MAPPED_ALIGNMENT,"mapping alignment");
    test_require(CALL("CloseHandle",mapping)&&CALL("CloseHandle",reopened),"mapping handles close with live views");
    test_require(CALL("UnmapViewOfFile",first)&&CALL("UnmapViewOfFile",second),"view release");
    test_require(!CALL("OpenFileMappingA",4,0,ADDRESS(mapping_name))&&win64_get_error()==2,"mapping lifetime");
    test_require(!CALL("CloseHandle",0x1234567)&&win64_get_error()==6,"invalid handle");
    path_info path;test_require(!translate_path("../outside",&path,0)&&win64_get_error()==5,"anchor traversal");
    test_require(!translate_path("Frostpunk.exe",&path,1)&&win64_get_error()==5,"read-only input writes");
    uint16_t unicode[]={0x416,0xd83d,0xde00,0};win64_region_add(ADDRESS(unicode),sizeof(unicode),0);char *converted=guest_text(ADDRESS(unicode),1);
    test_require(converted&&!strcmp(converted,"\xd0\x96\xf0\x9f\x98\x80"),"UTF16 surrogate conversion");free(converted);win64_region_remove(ADDRESS(unicode));
    char temporary[]="/data/local/tmp/frostpunk/kernel-selftest-XXXXXX";
    test_require(mkdtemp(temporary)!=NULL,"temporary kernel test directory");
    char inputs[PATH_MAX],profile[PATH_MAX],fixture[PATH_MAX],escape[PATH_MAX],escaped[PATH_MAX];
    snprintf(inputs,sizeof(inputs),"%s/game",temporary);snprintf(profile,sizeof(profile),"%s/profile",temporary);
    test_require(!mkdir(inputs,0700)&&!mkdir(profile,0700),"private filesystem anchors");
    snprintf(fixture,sizeof(fixture),"%s/game/input.bin",temporary);
    int fd=open(fixture,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);unsigned char payload[]={1,2,0x1a,0,0xff},readback[5]={0};
    test_require(fd>=0&&write(fd,payload,sizeof(payload))==(ssize_t)sizeof(payload),"create read-only fixture");close(fd);
    const char *saved_game=game_root,*saved_profile=profile_root;game_root=inputs;profile_root=profile;
    char input_name[]="input.bin";uint32_t transferred=0;uint64_t size=0;unsigned char info[52];
    win64_region_add(ADDRESS(input_name),sizeof(input_name),0);win64_region_add(ADDRESS(payload),sizeof(payload),0);
    win64_region_add(ADDRESS(readback),sizeof(readback),1);win64_region_add(ADDRESS(&transferred),sizeof(transferred),1);
    win64_region_add(ADDRESS(&size),sizeof(size),1);win64_region_add(ADDRESS(info),sizeof(info),1);
    uint64_t input=CALL("CreateFileA",ADDRESS(input_name),GENERIC_READ,7,0,3,0x80,0);
    test_require(input!=INVALID_HANDLE&&CALL("ReadFile",input,ADDRESS(readback),5,ADDRESS(&transferred),0)&&transferred==5&&!memcmp(payload,readback,5),"read original bytes through Win64 ABI");
    test_require(CALL("CloseHandle",input),"close input fixture");
    test_require(CALL("CreateFileA",ADDRESS(input_name),GENERIC_WRITE,7,0,3,0x80,0)==INVALID_HANDLE&&win64_get_error()==5,"input write refusal");
    uint16_t profile_name[256];size_t index=0;for(const char *p=guest_profile;*p;++p)profile_name[index++]=(unsigned char)*p;
    profile_name[index++]='/';profile_name[index++]=0x416;profile_name[index++]=0xd83d;profile_name[index++]=0xde00;
    profile_name[index++]='.';profile_name[index++]='b';profile_name[index++]='i';profile_name[index++]='n';profile_name[index]=0;
    win64_region_add(ADDRESS(profile_name),sizeof(profile_name),0);
    uint64_t file=CALL("CreateFileW",ADDRESS(profile_name),GENERIC_READ|GENERIC_WRITE,7,0,1,0x80,0);
    test_require(file!=INVALID_HANDLE,"create Unicode profile file");
    test_require(CALL("WriteFile",file,ADDRESS(payload),5,ADDRESS(&transferred),0)&&transferred==5,"binary write with CtrlZ and NUL");
    test_require(CALL("GetFileSizeEx",file,ADDRESS(&size))&&size==5,"64-bit file size");
    uint64_t file_mapping=CALL("CreateFileMappingW",file,0,4,0,0,0),file_view=CALL("MapViewOfFile",file_mapping,6,0,0,0);
    test_require(file_mapping&&file_view,"file-backed mapping");
    *(unsigned char *)win64_pointer(file_view+3,1,1)=0x42;
    test_require(CALL("FlushViewOfFile",file_view,0)&&CALL("UnmapViewOfFile",file_view)&&CALL("CloseHandle",file_mapping),"mapped profile writeback");
    test_require(CALL("SetFilePointerEx",file,0,0,0)&&CALL("ReadFile",file,ADDRESS(readback),5,ADDRESS(&transferred),0)&&readback[3]==0x42,"read mapped writeback");
    test_require(CALL("GetFileInformationByHandle",file,ADDRESS(info)),"packed Win64 file information");
    uint32_t info_size;memcpy(&info_size,info+36,4);test_require(info_size==5,"Win64 information field offsets");
    uint64_t times[3]={0};unsigned char attributes[36]={0};find_data_w found;find_data_a found_a;
    win64_region_add(ADDRESS(times),sizeof(times),1);win64_region_add(ADDRESS(attributes),sizeof(attributes),1);
    win64_region_add(ADDRESS(&found),sizeof(found),1);win64_region_add(ADDRESS(&found_a),sizeof(found_a),1);
    test_require(CALL("GetFileType",file)==1&&CALL("FlushFileBuffers",file),"file type and durable profile flush");
    test_require(CALL("GetFileTime",file,ADDRESS(times),ADDRESS(times+1),ADDRESS(times+2))&&times[1]&&times[2],"real Win64 FILETIME outputs");
    test_require(CALL("GetFileAttributesW",ADDRESS(profile_name))==0x80&&CALL("GetFileAttributesExW",ADDRESS(profile_name),0,ADDRESS(attributes)),"profile attributes ordinary and extended");
    uint64_t attribute_mtime;uint32_t attribute_bytes;memcpy(&attribute_mtime,attributes+20,8);memcpy(&attribute_bytes,attributes+32,4);
    test_require(attribute_mtime==times[2]&&attribute_bytes==5,"attribute time and size ABI offsets");
    uint64_t find=CALL("FindFirstFileW",ADDRESS(profile_name),ADDRESS(&found));
    const uint16_t expected_name[]={0x416,0xd83d,0xde00,'.','b','i','n',0};
    test_require(find!=INVALID_HANDLE&&found.fields[8]==5&&!memcmp(found.filename,expected_name,sizeof(expected_name)),"exact Unicode find data 592-byte ABI");
    test_require(!CALL("CloseHandle",find)&&win64_get_error()==6,"search handle requires FindClose");
    test_require(!CALL("FindNextFileW",find,ADDRESS(&found))&&win64_get_error()==18&&CALL("FindClose",find),"exact search end and search close");
    find=CALL("FindFirstFileA",ADDRESS(input_name),ADDRESS(&found_a));
    test_require(find!=INVALID_HANDLE&&!strcmp(found_a.filename,input_name)&&found_a.fields[8]==5&&CALL("FindClose",find),"ANSI find data 320-byte ABI");
    uint16_t pattern[256];size_t prefix_units=strlen(guest_profile)+1;memcpy(pattern,profile_name,prefix_units*2);
    pattern[prefix_units]='*';pattern[prefix_units+1]='.';pattern[prefix_units+2]='*';pattern[prefix_units+3]=0;
    win64_region_add(ADDRESS(pattern),sizeof(pattern),1);
    find=CALL("FindFirstFileExW",ADDRESS(pattern),1,ADDRESS(&found),0,0,2);
    test_require(find!=INVALID_HANDLE,"Windows star-dot-star directory search");unsigned enumerated=0;
    do {
        if(found.filename[0]=='.'&&(!found.filename[1]||(found.filename[1]=='.'&&!found.filename[2])))continue;
        test_require(!memcmp(found.filename,expected_name,sizeof(expected_name))&&found.fields[8]==5&&!found.alternate[0],"Unicode enumeration and empty short alias");++enumerated;
    }while(CALL("FindNextFileW",find,ADDRESS(&found)));
    test_require(enumerated==1&&win64_get_error()==18&&CALL("FindClose",find),"complete Unicode profile enumeration");
    pattern[prefix_units]='?';pattern[prefix_units+1]=0;
    test_require(CALL("FindFirstFileW",ADDRESS(pattern),ADDRESS(&found))==INVALID_HANDLE&&win64_get_error()==50,"unsupported wildcard rejects without POSIX interpretation");
    char save_directory[PATH_MAX],moved_name[PATH_MAX],case_name[PATH_MAX];uint16_t moved_w[256];
    snprintf(save_directory,sizeof(save_directory),"%s/saves",guest_profile);snprintf(moved_name,sizeof(moved_name),"%s/saves/moved.bin",guest_profile);
    snprintf(case_name,sizeof(case_name),"%s/saves/MOVED.bin",guest_profile);
    size_t moved_units=0;for(const char *p=moved_name;*p;++p)moved_w[moved_units++]=(unsigned char)*p;moved_w[moved_units]=0;
    win64_region_add(ADDRESS(save_directory),sizeof(save_directory),0);win64_region_add(ADDRESS(moved_name),sizeof(moved_name),0);
    win64_region_add(ADDRESS(case_name),sizeof(case_name),0);win64_region_add(ADDRESS(moved_w),sizeof(moved_w),0);
    test_require(CALL("CreateDirectoryA",ADDRESS(save_directory),0)&&CALL("GetFileAttributesA",ADDRESS(save_directory))==0x10,"create own profile directory");
    uint64_t destination=CALL("CreateFileA",ADDRESS(moved_name),GENERIC_READ|GENERIC_WRITE,7,0,1,0x80,0);
    test_require(destination!=INVALID_HANDLE&&CALL("CloseHandle",destination),"existing move destination fixture");
    test_require(!CALL("MoveFileW",ADDRESS(profile_name),ADDRESS(moved_w))&&win64_get_error()==183,"move cannot replace existing destination");
    test_require(CALL("DeleteFileA",ADDRESS(moved_name))&&CALL("CloseHandle",file)&&CALL("MoveFileW",ADDRESS(profile_name),ADDRESS(moved_w)),"atomic Unicode profile move");
    test_require(CALL("GetFileAttributesW",ADDRESS(profile_name))==UINT32_MAX&&win64_get_error()==2,"move removes source name");
    destination=CALL("CreateFileA",ADDRESS(moved_name),GENERIC_READ,1,0,3,0x80,0);
    test_require(destination!=INVALID_HANDLE&&!CALL("MoveFileA",ADDRESS(moved_name),ADDRESS(case_name))&&win64_get_error()==32,"move honors existing share-delete denial");
    test_require(CALL("CloseHandle",destination)&&CALL("MoveFileA",ADDRESS(moved_name),ADDRESS(case_name)),"case-only no-replace rename");
    test_require(!CALL("MoveFileA",ADDRESS(input_name),ADDRESS(moved_name))&&win64_get_error()==5,"game anchor cannot be moved into writable profile");
    test_require(!CALL("RemoveDirectoryA",ADDRESS(save_directory))&&win64_get_error()==145,"nonempty directory removal fails");
    test_require(CALL("DeleteFileA",ADDRESS(moved_name))&&CALL("RemoveDirectoryA",ADDRESS(save_directory)),"profile-only file and directory cleanup");
    win64_region_remove(ADDRESS(save_directory));win64_region_remove(ADDRESS(moved_name));win64_region_remove(ADDRESS(case_name));win64_region_remove(ADDRESS(moved_w));
    win64_region_remove(ADDRESS(pattern));win64_region_remove(ADDRESS(times));win64_region_remove(ADDRESS(attributes));win64_region_remove(ADDRESS(&found));win64_region_remove(ADDRESS(&found_a));
    snprintf(escape,sizeof(escape),"%s/profile/escape",temporary);snprintf(escaped,sizeof(escaped),"%s/game/escaped.bin",temporary);
    test_require(!symlink(inputs,escape),"create own symlink boundary fixture");
    char symlink_path[PATH_MAX];snprintf(symlink_path,sizeof(symlink_path),"%s/escape/escaped.bin",guest_profile);
    win64_region_add(ADDRESS(symlink_path),sizeof(symlink_path),0);
    test_require(CALL("CreateFileA",ADDRESS(symlink_path),GENERIC_WRITE,7,0,1,0x80,0)==INVALID_HANDLE&&access(escaped,F_OK),"symlink write boundary");
    win64_region_remove(ADDRESS(symlink_path));win64_region_remove(ADDRESS(profile_name));win64_region_remove(ADDRESS(input_name));
    win64_region_remove(ADDRESS(payload));win64_region_remove(ADDRESS(readback));win64_region_remove(ADDRESS(&transferred));win64_region_remove(ADDRESS(&size));win64_region_remove(ADDRESS(info));
    game_root=saved_game;profile_root=saved_profile;
    test_require(!unlink(escape)&&!unlink(fixture)&&!rmdir(inputs)&&!rmdir(profile)&&!rmdir(temporary),"remove own filesystem fixtures");
    win64_region_remove(ADDRESS(event_name));win64_region_remove(ADDRESS(mapping_name));
    win64_set_error(saved_error);
    fprintf(stderr,"KERNEL_SELF_TEST passed event_lifetime=1 auto_manual=1 immediate_reset_wake=1 semaphore=1 mapping_shared_zero_NX=1 utf16=1 anchored_paths=1 binary_file_IO=1 file_mapping=1 file_attributes_time=1 file_type_flush=1 unicode_find_W592_A320=1 strict_wildcards=1 atomic_move_noreplace=1 share_delete_and_case_rename=1 temporary_directory_cleanup=1 symlink_write_denied=1\n");
}
