#include "win64_string.h"
#include "win64.h"
#include "win64_stdio.h"
#include "win64_locale.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void string_stop(const char *detail) {
    fprintf(stderr,"STOP Win64 string %s; guest invalid-parameter callbacks are not implemented\n",detail);
    exit(73);
}

static unsigned element(uint64_t pointer,size_t index,unsigned width) {
    if (index>(UINT64_MAX-pointer)/width) string_stop("source overflow");
    const unsigned char *p=win64_pointer(pointer+index*width,width,0);
    unsigned value=p[0]; if (width==2) value|=(unsigned)p[1]<<8;
    return value;
}

static size_t bounded_length(uint64_t pointer,size_t limit,unsigned width) {
    size_t n=0;
    while (n<limit && element(pointer,n,width)) ++n;
    return n;
}

static int compare(uint64_t first,uint64_t second,size_t count,unsigned width,int fold) {
    for(size_t i=0;i<count;++i) {
        unsigned a=element(first,i,width),b=element(second,i,width);
        /* The supported C/en-US UTF-8 LC_CTYPE folds individual ASCII bytes.
         * It does not decode multibyte UTF-8 in _stricmp/_strnicmp. */
        if(fold){if(a>='A'&&a<='Z')a+='a'-'A';if(b>='A'&&b<='Z')b+='a'-'A';}
        if(a!=b)return fold?(int)a-(int)b:a<b?-1:1;
        if(!a)return 0;
    }
    return 0;
}

static void character_set(uint64_t source,unsigned char set[256]) {
    memset(set,0,256);
    for(size_t i=0;;++i){unsigned value=element(source,i,1);if(!value)break;set[value]=1;}
}

static uint64_t duplicate(uint64_t source,unsigned width) {
    if(!source)return 0;
    size_t n=bounded_length(source,SIZE_MAX,width);
    if(n>SIZE_MAX/width-1)string_stop("duplicate size overflow");size_t bytes=(n+1)*width;
    uint64_t result=win64_alloc(bytes,0);if(!result){win64_stdio_set_errno(ENOMEM);return 0;}
    memcpy(win64_pointer(result,bytes,1),win64_pointer(source,bytes,0),bytes);return result;
}

int win64_string_call(CPU *c,const char *name) {
    if(!strcmp(name,"_stricmp")||!strcmp(name,"_strnicmp")||!strcmp(name,"strncmp")||!strcmp(name,"wcscmp")) {
        int counted=!strcmp(name,"_strnicmp")||!strcmp(name,"strncmp");
        int value=compare(c->rcx,c->rdx,counted?(size_t)c->r8:SIZE_MAX,!strcmp(name,"wcscmp")?2:1,name[0]=='_');
        c->rax=(uint32_t)value;return 1;
    }
    if(!strcmp(name,"wcsnlen")||!strcmp(name,"__strncnt")) {
        c->rax=bounded_length(c->rcx,(size_t)c->rdx,!strcmp(name,"wcsnlen")?2:1);return 1;
    }
    if(!strcmp(name,"_strdup")||!strcmp(name,"_wcsdup")){c->rax=duplicate(c->rcx,!strcmp(name,"_wcsdup")?2:1);return 1;}
    if(!strcmp(name,"strchr")||!strcmp(name,"strrchr")) {
        unsigned wanted=(unsigned)c->rdx&255;uint64_t found=0;int last=!strcmp(name,"strrchr");
        for(size_t i=0;;++i){unsigned value=element(c->rcx,i,1);if(value==wanted){found=c->rcx+i;if(!last)break;}if(!value)break;}
        c->rax=found;return 1;
    }
    if(!strcmp(name,"strpbrk")||!strcmp(name,"strcspn")) {
        unsigned char set[256];character_set(c->rdx,set);size_t n=0;
        for(;;++n){unsigned value=element(c->rcx,n,1);if(!value||set[value])break;}
        c->rax=!strcmp(name,"strcspn")?n:element(c->rcx,n,1)?c->rcx+n:0;return 1;
    }
    if(!strcmp(name,"strstr")) {
        size_t needle=bounded_length(c->rdx,SIZE_MAX,1);if(!needle){c->rax=c->rcx;return 1;}
        size_t haystack=bounded_length(c->rcx,SIZE_MAX,1);const char *first=win64_pointer(c->rcx,haystack+1,0),*second=win64_pointer(c->rdx,needle+1,0);
        const char *found=strstr(first,second);c->rax=found?c->rcx+(size_t)(found-first):0;return 1;
    }
    if(!strcmp(name,"strncpy")||!strcmp(name,"strncat")) {
        size_t count=(size_t)c->r8;int append=!strcmp(name,"strncat");size_t prefix=append?bounded_length(c->rcx,SIZE_MAX,1):0;
        if(prefix>UINT64_MAX-c->rcx)string_stop("append pointer overflow");
        if(!count&&!append){c->rax=c->rcx;return 1;}
        size_t length=bounded_length(c->rdx,count,1);if(append&&length==SIZE_MAX)string_stop("append size overflow");size_t bytes=append?length+1:count;
        unsigned char *out=win64_pointer(c->rcx+prefix,bytes,1);
        if(length)memcpy(out,win64_pointer(c->rdx,length,0),length);
        if(append)out[length]=0;else if(length<count)memset(out+length,0,count-length);c->rax=c->rcx;return 1;
    }
    if(!strcmp(name,"strtok_s")) {
        unsigned char set[256];character_set(c->rdx,set);void *context=win64_pointer(c->r8,8,1);uint64_t previous;memcpy(&previous,context,8);uint64_t cursor=c->rcx?c->rcx:previous;
        if(!cursor)string_stop("NULL strtok_s source/context");
        unsigned value;while((value=element(cursor,0,1))&&set[value])++cursor;
        if(!value){memcpy(context,&cursor,8);c->rax=0;return 1;}
        uint64_t token=cursor;while((value=element(cursor,0,1))&&!set[value])++cursor;
        if(value){*(unsigned char*)win64_pointer(cursor,1,1)=0;++cursor;}memcpy(context,&cursor,8);c->rax=token;return 1;
    }
    if(!strcmp(name,"_strlwr_s")) {
        size_t capacity=(size_t)c->rdx;if(!c->rcx||!capacity)string_stop("invalid lowercase destination/size");
        unsigned char *out=win64_pointer(c->rcx,capacity,1);size_t length=bounded_length(c->rcx,capacity,1);
        if(length==capacity){out[0]=0;string_stop("unterminated lowercase destination");}
        CPU locale={0};if(!win64_locale_call(&locale,"___lc_codepage_func"))string_stop("missing lowercase locale");
        if(locale.rax!=0&&locale.rax!=65001)string_stop("unsupported lowercase codepage");
        /* C preserves high bytes. UTF-8 Unicode lowercasing requires a full
         * Unicode mapping and buffer-size check, so fail before mutation. */
        if(locale.rax==65001)for(size_t i=0;i<length;++i)if(out[i]>=128){fprintf(stderr,"STOP Win64 string unsupported non-ASCII UTF-8 _strlwr_s\n");exit(73);}
        for(size_t i=0;i<length;++i)if(out[i]>='A'&&out[i]<='Z')out[i]+='a'-'A';c->rax=0;return 1;
    }
    int wide=!strcmp(name,"wcscpy_s"),append=!strcmp(name,"strcat_s") || !strcmp(name,"strncat_s");
    int counted=!strcmp(name,"strncpy_s") || !strcmp(name,"strncat_s");
    if (!wide && !append && !counted && strcmp(name,"strcpy_s")) return 0;
    uint64_t destination=c->rcx,source=c->r8; size_t capacity=(size_t)c->rdx,count=(size_t)c->r9;
    unsigned width=wide?2:1;
    if (!destination || !capacity || capacity>SIZE_MAX/width) string_stop("invalid destination/size");
    unsigned char *out=win64_pointer(destination,capacity*width,1);
    size_t prefix=append?strnlen((const char*)out,capacity):0;
    if (prefix==capacity) string_stop("unterminated destination");
    if (counted && !count) { memset(out+prefix*width,0,width); c->rax=0; return 1; }
    if (!source) string_stop("NULL source");
    size_t available=capacity-prefix;
    int truncate=counted && count==SIZE_MAX;
    size_t limit=counted && !truncate && count<available?count:available;
    size_t length=bounded_length(source,limit,width);
    if (length>=available) {
        if (!truncate) { memset(out,0,width); string_stop("destination too small"); }
        length=available-1;
    }
    if (length) memcpy(out+prefix*width,win64_pointer(source,length*width,0),length*width);
    memset(out+(prefix+length)*width,0,width);
    c->rax=truncate && element(source,length,width)?80:0;
    return 1;
}

void win64_string_self_test(void) {
    unsigned char buffer[64]; char source[]="Frostpunk"; uint16_t wide[]={0x41,0x20ac,0xd83d,0xde00,0};
    win64_region_add((uintptr_t)buffer,sizeof(buffer),1);
    win64_region_add((uintptr_t)source,sizeof(source),0);
    win64_region_add((uintptr_t)wide,sizeof(wide),0);
    const char *names[]={"strcpy_s","strcat_s","strncpy_s","strncat_s","wcscpy_s"};
    for (unsigned i=0;i<5;++i) {
        memset(buffer,0xa5,sizeof(buffer)); buffer[0]=0;
        if (i==1 || i==3) memcpy(buffer,"Ice ",5);
        CPU c={0}; c.rsp=0x12345678; c.rcx=(uintptr_t)buffer; c.rdx=32; c.r8=(uintptr_t)(i==4?(void*)wide:(void*)source); c.r9=5;
        if (!win64_string_call(&c,names[i]) || c.rax || c.rsp!=0x12345678) abort();
        if (i==4) { if (memcmp(buffer,wide,sizeof(wide)) || buffer[sizeof(wide)]!=0xa5) abort(); }
        else if (strcmp((char*)buffer,i==0?"Frostpunk":i==1?"Ice Frostpunk":i==2?"Frost":"Ice Frost")) abort();
    }
    for (size_t cap=1;cap<12;++cap) {
        memset(buffer,0xa5,sizeof(buffer)); CPU c={0}; c.rcx=(uintptr_t)buffer; c.rdx=cap; c.r8=(uintptr_t)source; c.r9=SIZE_MAX;
        win64_string_call(&c,"strncpy_s"); size_t length=cap-1<9?cap-1:9;
        if (c.rax!=(cap<=9?80u:0u) || memcmp(buffer,source,length) || buffer[length] || buffer[cap]!=0xa5) abort();
    }
    CPU c={0}; c.rcx=(uintptr_t)buffer; c.rdx=1; c.r8=0; c.r9=0;
    win64_string_call(&c,"strncpy_s"); if (c.rax || buffer[0]) abort();
    const char first[]="FROSTPUNK",second[]="frostpunk",utf8_upper[]={0xc3,0x84,0},utf8_lower[]={0xc3,0xa4,0};
    win64_region_add((uintptr_t)first,sizeof(first),0);win64_region_add((uintptr_t)second,sizeof(second),0);win64_region_add((uintptr_t)utf8_upper,sizeof(utf8_upper),0);win64_region_add((uintptr_t)utf8_lower,sizeof(utf8_lower),0);
    c=(CPU){0};c.rsp=0x12345678;c.rcx=(uintptr_t)first;c.rdx=(uintptr_t)second;
    win64_string_call(&c,"_stricmp");if(c.rax||c.rsp!=0x12345678)abort();
    c.r8=0;c.rcx=UINT64_MAX;c.rdx=0;win64_string_call(&c,"_strnicmp");if(c.rax)abort();
    win64_string_call(&c,"strncmp");if(c.rax)abort();
    c.rcx=(uintptr_t)utf8_upper;c.rdx=(uintptr_t)utf8_lower;win64_string_call(&c,"_stricmp");if((int32_t)c.rax!=-32)abort();
    c.rcx=(uintptr_t)wide;c.rdx=9;win64_string_call(&c,"wcsnlen");if(c.rax!=4)abort();
    c.rcx=(uintptr_t)first;c.rdx=5;win64_string_call(&c,"__strncnt");if(c.rax!=5)abort();
    c.rcx=(uintptr_t)source;c.rdx='p';win64_string_call(&c,"strchr");if(c.rax!=(uintptr_t)source+5)abort();
    c.rdx=0;win64_string_call(&c,"strrchr");if(c.rax!=(uintptr_t)source+9)abort();
    c.rcx=(uintptr_t)source;c.rdx=(uintptr_t)source+5;win64_string_call(&c,"strstr");if(c.rax!=(uintptr_t)source+5)abort();
    c.rdx=(uintptr_t)source+5;win64_string_call(&c,"strcspn");if(c.rax!=5)abort();
    win64_string_call(&c,"strpbrk");if(c.rax!=(uintptr_t)source+5)abort();
    c.rcx=(uintptr_t)source;win64_string_call(&c,"_strdup");uint64_t copy=c.rax;if(!copy||copy==(uintptr_t)source||memcmp(win64_pointer(copy,10,0),source,10))abort();win64_free(copy);
    c.rcx=(uintptr_t)wide;win64_string_call(&c,"_wcsdup");copy=c.rax;if(!copy||memcmp(win64_pointer(copy,sizeof(wide),0),wide,sizeof(wide)))abort();win64_free(copy);
    memset(buffer,0xa5,sizeof(buffer));c.rcx=(uintptr_t)buffer;c.rdx=(uintptr_t)source;c.r8=12;win64_string_call(&c,"strncpy");if(c.rax!=(uintptr_t)buffer||memcmp(buffer,source,9)||buffer[9]||buffer[10]||buffer[11]||buffer[12]!=0xa5)abort();
    c.r8=5;win64_string_call(&c,"strncat");if(c.rax!=(uintptr_t)buffer||strcmp((char*)buffer,"FrostpunkFrost"))abort();
    uint64_t context=0;win64_region_add((uintptr_t)&context,sizeof(context),1);
    memcpy(buffer,"a,,b,",6);c.rcx=(uintptr_t)buffer;c.rdx=(uintptr_t)buffer+1;c.r8=(uintptr_t)&context;
    /* Separate immutable delimiter: source modification must not change it. */
    const char delimiter[]=",";win64_region_add((uintptr_t)delimiter,sizeof(delimiter),0);c.rdx=(uintptr_t)delimiter;
    win64_string_call(&c,"strtok_s");if(c.rax!=(uintptr_t)buffer||context!=(uintptr_t)buffer+2||buffer[1])abort();
    c.rcx=0;win64_string_call(&c,"strtok_s");if(c.rax!=(uintptr_t)buffer+3||context!=(uintptr_t)buffer+5)abort();
    win64_string_call(&c,"strtok_s");if(c.rax||context!=(uintptr_t)buffer+5)abort();
    win64_region_remove((uintptr_t)delimiter);win64_region_remove((uintptr_t)&context);
    memcpy(buffer,"ICE FROST",10);c.rcx=(uintptr_t)buffer;c.rdx=sizeof(buffer);win64_string_call(&c,"_strlwr_s");if(c.rax||strcmp((char*)buffer,"ice frost"))abort();
    win64_region_remove((uintptr_t)utf8_lower);win64_region_remove((uintptr_t)utf8_upper);win64_region_remove((uintptr_t)second);win64_region_remove((uintptr_t)first);
    win64_region_remove((uintptr_t)wide);win64_region_remove((uintptr_t)source);win64_region_remove((uintptr_t)buffer);
    fprintf(stderr,"WIN64_STRING_SELF_TEST secure_copy_append=5 truncate_capacities=11 UTF16_surrogates=1 zero_count_NULL_source=1 comparison_and_length=6 UTF8_individual_byte_casefold=1 search=5 duplicate=2 copy_append=2 tokenizer=3 lowercase=1 passed\n");
}
