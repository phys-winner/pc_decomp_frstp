/* MSVC/UCRT's unsigned 32-bit LCG, with one independent guest state per thread.
 * Bionic's rand/srand use a different algorithm and are never substituted. */
#include "win64_utility.h"
#include "win64.h"
#include "win64_stdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <locale.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#else
#include <pthread.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
static _Thread_local uint32_t random_state=1;
static _Noreturn void utility_stop(const char *reason){fprintf(stderr,"STOP Win64 utility %s\n",reason);fflush(stderr);_Exit(73);}
static unsigned decimal_byte(uint64_t address){return *(const unsigned char*)win64_pointer(address,1,0);}
static unsigned decimal_next(uint64_t *address){if(*address==UINT64_MAX)utility_stop("atoi address overflow");return decimal_byte(++*address);}
static uint32_t windows_atoi(uint64_t address){
    if(!address){win64_stdio_set_errno(22);utility_stop("atoi invalid-parameter callback required for NULL");}
    unsigned c=decimal_byte(address);
    /* These are exactly the whitespace bytes in both supported C/UTF8 locales. */
    while(c==' '||(c>=9&&c<=13))c=decimal_next(&address);
    int negative=c=='-';if(c=='-'||c=='+')c=decimal_next(&address);
    uint32_t value=0,limit=negative?UINT32_C(2147483648):UINT32_C(2147483647);int overflow=0;
    while(c>='0'&&c<='9'){unsigned digit=c-'0';
        if(value>limit/10||(value==limit/10&&digit>limit%10))overflow=1;
        else if(!overflow)value=value*10+digit;
        c=decimal_next(&address);
    }
    if(overflow){win64_stdio_set_errno(34);value=limit;}
    return negative?UINT32_C(0)-value:value;
}
/* UCRT _atoi64: decimal with ASCII whitespace, saturating at INT64 bounds with ERANGE. */
static uint64_t windows_atoi64(uint64_t address){
    if(!address){win64_stdio_set_errno(22);utility_stop("_atoi64 invalid-parameter callback required for NULL");}
    unsigned c=decimal_byte(address);
    while(c==' '||(c>=9&&c<=13))c=decimal_next(&address);
    int negative=c=='-';if(c=='-'||c=='+')c=decimal_next(&address);
    uint64_t value=0,limit=negative?UINT64_C(9223372036854775808):UINT64_C(9223372036854775807);int overflow=0;
    while(c>='0'&&c<='9'){unsigned digit=c-'0';
        if(value>limit/10||(value==limit/10&&digit>limit%10))overflow=1;
        else if(!overflow)value=value*10+digit;
        c=decimal_next(&address);
    }
    if(overflow){win64_stdio_set_errno(34);value=limit;}
    return negative?UINT64_C(0)-value:value;
}
/* UCRT _itoa_s(int value, char *buffer, size_t size, int radix): a minus sign only for radix 10,
 * other radixes print the 32-bit two's complement. Invalid arguments and a short buffer invoke the
 * invalid-parameter handler in UCRT, which this runtime models as STOP. */
static uint32_t windows_itoa_s(CPU *cpu){
    int32_t value=(int32_t)cpu->rcx;uint64_t buffer=cpu->rdx,size=cpu->r8;int radix=(int32_t)cpu->r9;
    if(!buffer||!size||radix<2||radix>36){win64_stdio_set_errno(22);utility_stop("_itoa_s invalid-parameter callback required");}
    char digits[40];size_t n=0;int negative=radix==10&&value<0;
    uint32_t magnitude=negative?UINT32_C(0)-(uint32_t)value:(uint32_t)value;
    do{unsigned d=magnitude%(unsigned)radix;digits[n++]=(char)(d<10?'0'+d:'a'+d-10);magnitude/=(unsigned)radix;}while(magnitude);
    if(negative)digits[n++]='-';
    if(n+1>size){win64_stdio_set_errno(34);utility_stop("_itoa_s buffer too small: invalid-parameter callback required");}
    char *out=win64_pointer(buffer,n+1,1);for(size_t i=0;i<n;++i)out[i]=digits[n-1-i];out[n]=0;
    return 0;
}
static void numeric_end(uint64_t output,uint64_t address){if(output)memcpy(win64_pointer(output,8,1),&address,8);}
static unsigned numeric_digit(unsigned c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='Z')return c-'A'+10;
    if(c>='a'&&c<='z')return c-'a'+10;
    return 36;
}
/* Win64 long/unsigned long stay32bit on the AArch64 LP64 host. The shared
 * integer parser uses the supported C/UTF8 locale's ASCII whitespace/digits;
 * native UCRT accepts0x without digits as no conversion (endptr=input).
 * Invalid input/base retains the existing explicit callback STOP policy. */
/* One code unit of a narrow (width 1) or UTF-16 (width 2) guest string. */
static unsigned numeric_unit(uint64_t address,unsigned width){
    if(width==1)return decimal_byte(address);
    uint16_t unit;memcpy(&unit,win64_pointer(address,2,0),2);return unit;
}
static unsigned numeric_next(uint64_t *address,unsigned width){
    if(*address>UINT64_MAX-width)utility_stop("integer parse address overflow");
    *address+=width;return numeric_unit(*address,width);
}
/* wcstol (width 2) shares the parser; like the narrow forms it accepts ASCII whitespace and digits
 * only (Frostpunk reads the numeric "-N" suffix of its save names with it). */
static uint32_t windows_integer(uint64_t input,uint64_t output,int base,int is_signed,unsigned width){
    numeric_end(output,input);
    if(!input||base<0||base==1||base>36){win64_stdio_set_errno(22);utility_stop(is_signed?"strtol invalid-parameter callback required":"strtoul invalid-parameter callback required");}
    uint64_t address=input;unsigned c=numeric_unit(address,width);
    while(c==' '||(c>=9&&c<=13))c=numeric_next(&address,width);
    int negative=c=='-';if(c=='-'||c=='+')c=numeric_next(&address,width);
    if((base==0||base==16)&&c=='0'){
        uint64_t next=address;unsigned look=numeric_next(&next,width);
        if(look=='x'||look=='X'){base=16;address=next;c=numeric_next(&address,width);}
        else if(!base)base=8;
    }
    if(!base)base=10;
    uint32_t value=0,limit=is_signed?(negative?UINT32_C(2147483648):UINT32_C(2147483647)):UINT32_MAX;
    int any=0,overflow=0;unsigned digit;
    while((digit=numeric_digit(c))<(unsigned)base){
        any=1;
        if(value>limit/(unsigned)base||(value==limit/(unsigned)base&&digit>limit%(unsigned)base))overflow=1;
        else if(!overflow)value=value*(unsigned)base+digit;
        c=numeric_next(&address,width);
    }
    if(any)numeric_end(output,address);
    if(overflow){win64_stdio_set_errno(34);return limit;}
    return negative?UINT32_C(0)-value:value;
}
#ifdef _WIN32
static INIT_ONCE numeric_once=INIT_ONCE_STATIC_INIT;
static _locale_t numeric_locale;
static BOOL CALLBACK numeric_init(PINIT_ONCE once,PVOID argument,PVOID *context){
    (void)once;(void)argument;(void)context;numeric_locale=_create_locale(LC_NUMERIC,"C");return TRUE;
}
#else
static pthread_once_t numeric_once=PTHREAD_ONCE_INIT;
static locale_t numeric_locale;
static void numeric_init(void){numeric_locale=newlocale(LC_NUMERIC_MASK,"C",(locale_t)0);}
#endif
static uint64_t numeric_convert(const char *text,char **end){
    uint64_t bits;int saved_errno=errno;
#if defined(__aarch64__)
    uint64_t control,status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=control&~((UINT64_C(3)<<22)|(UINT64_C(1)<<24)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15));
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u);
#endif
#ifdef _WIN32
    if(!InitOnceExecuteOnce(&numeric_once,numeric_init,NULL,NULL)||!numeric_locale)utility_stop("C numeric locale allocation");
    double value=_strtod_l(text,end,numeric_locale);
#else
    if(pthread_once(&numeric_once,numeric_init)||!numeric_locale)utility_stop("C numeric locale allocation");
    double value=strtod_l(text,end,numeric_locale);
#endif
    memcpy(&bits,&value,8);
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(control),"r"(status):"memory");
#else
    _mm_setcsr(control);
#endif
    errno=saved_errno;return bits;
}
static unsigned numeric_lower(unsigned c){return c>='A'&&c<='Z'?c+('a'-'A'):c;}
static int numeric_word(const char *text,const char *word){
    while(*word)if(numeric_lower((unsigned char)*text++)!=(unsigned char)*word++)return 0;
    return 1;
}
static uint64_t windows_strtod(uint64_t input,uint64_t output,unsigned rounding){
    numeric_end(output,input);
    if(!input){win64_stdio_set_errno(22);utility_stop("strtod invalid-parameter callback required for NULL");}
    /* Both supported guest locales have NUMERIC C. The core validates a NUL
     * string (at most 32767 bytes); no host locale or unchecked guest pointer. */
    const char *text=win64_string_a(input),*token=text;char *end;
    while(*token==' '||(*token>=9&&*token<=13))++token;
    int negative=*token=='-';if(*token=='+'||*token=='-')++token;
    uint64_t sign=negative?UINT64_C(0x8000000000000000):0;
    uint64_t bits;
    if(numeric_word(token,"nan")){
        end=(char*)token+3;
        if(token[3]=='('){
            const char *p=token+4;
            while(numeric_digit((unsigned char)*p)<36||*p=='_')++p;
            if(*p==')')end=(char*)p+1;
        }
        bits=sign|UINT64_C(0x7fffffffffffffff);
        if(numeric_word(token+3,"(snan)"))bits=sign|UINT64_C(0x7ff0000000000001);
        else if(numeric_word(token+3,"(ind)"))bits=UINT64_C(0xfff8000000000000);
    }else if(numeric_word(token,"inf")){
        bits=sign|UINT64_C(0x7ff0000000000000);end=(char*)token+(numeric_word(token,"infinity")?8:3);
    }else{
        bits=numeric_convert(text,&end);uint64_t magnitude=bits&UINT64_C(0x7fffffffffffffff);
        int hex=token[0]=='0'&&(token[1]=='x'||token[1]=='X'),nonzero=0;unsigned significant_digits=0;
        const char *mantissa=token+(hex?2:0);
        for(const char *p=mantissa;p<end;++p){
            unsigned c=(unsigned char)*p;
            if((!hex&&(c=='e'||c=='E'))||(hex&&(c=='p'||c=='P')))break;
            if(numeric_digit(c)<(hex?16u:10u)){
                if(c!='0')nonzero=1;
                if(nonzero&&++significant_digits>768)utility_stop("strtod mantissa exceeds 768 significant digits");
            }
        }
        /* shortcut: nonzero numeric literals require RNE. UCRT's directed
         * decimal rounding and range clamps differ from Bionic. RNE supports
         * the full range through 768 significant mantissa digits. Longer input
         * requires UCRT's truncation algorithm and is explicitly unsupported;
         * all RCs support exact zero, no conversion and specials. */
        if(rounding&&nonzero)utility_stop("strtod directed-rounding nonzero literal");
        if(nonzero&&(magnitude==0||magnitude==UINT64_C(0x7ff0000000000000)))win64_stdio_set_errno(34);
    }
    numeric_end(output,input+(uint64_t)(end-text));return bits;
}
static uint64_t windows_bsearch(CPU *cpu){
    int saved_errno=errno;
    uint64_t key=cpu->rcx,base=cpu->rdx,count=cpu->r8,width=cpu->r9,function=win64_argument(cpu,4);
    if((!base&&count)||!width||!function){
        win64_stdio_set_errno(22);utility_stop("bsearch invalid-parameter callback required");
    }
    if(!count){errno=saved_errno;return 0;}
    if(width>UINT64_MAX/count||base>UINT64_MAX-count*width)utility_stop("bsearch array geometry overflow");
    win64_pointer(base,(size_t)(count*width),0);
    if(cpu->rsp<64)utility_stop("bsearch comparator stack underflow");
    uint64_t frame=(cpu->rsp-48)&~UINT64_C(15),sentinel=UINT64_C(0xe5ef00d0);
    win64_pointer(frame-8,40,1);
    while(count){
        /* UCRT selects the lower median for even counts. The key is opaque:
         * NULL and pointer-valued keys are passed untouched to the comparator. */
        uint64_t middle=(count-1)/2,element=base+middle*width;
        win64_pointer(element,(size_t)width,0);
        CPU call=*cpu;call.rcx=key;call.rdx=element;call.rsp=frame;
        errno=saved_errno;push64(&call,sentinel);dispatch(&call,function);saved_errno=errno;
        uint64_t returned;memcpy(&returned,win64_pointer(frame-8,8,0),8);
        if(call.rsp!=frame||returned!=sentinel)utility_stop("bsearch comparator shadow/return stack imbalance");
        int32_t result=(int32_t)(uint32_t)call.rax;
        if(!result){errno=saved_errno;return element;}
        if(result<0)count=middle;
        else{base=element+width;count-=middle+1;}
    }
    errno=saved_errno;return 0;
}
/* The function behind an ordinary main-image target or NULL (android/host/bootstrap.c); absent in
 * the host test builds, where every comparison goes through dispatch(). */
#if defined(__GNUC__)
void (*frostpunk_static_target(uint64_t target))(CPU *) __attribute__((weak));
#define SORT_STATIC_TARGET(target) (frostpunk_static_target?frostpunk_static_target(target):NULL)
#else
#define SORT_STATIC_TARGET(target) NULL
#endif
/* The render thread sorts its draw queue with qsort every frame; looking the comparator up in the
 * dispatcher for every comparison was about 4 % of that thread.  The comparator is resolved once
 * per sort; every comparison still starts from a full copy of the caller's CPU state and checks
 * both elements and the frame, as before. */
typedef int32_t (*native_compare)(uint64_t left,uint64_t right);
typedef struct {CPU *parent;uint64_t base,count,width,function,frame;void (*direct)(CPU *);int host_errno;native_compare native;} guest_sort;
/* Native forms of the two comparators the render thread sorts with every frame (the comparator
 * calls were 7 % of that thread on Adreno 650 in native Vulkan: a full CPU copy, the dispatcher and
 * the guest body per comparison).  Each one is the lifted body's own arithmetic, read from the same
 * memory, so it returns the same value for every input; the heap sort around it is unchanged, so
 * the order of the sorted array is the same as before byte for byte (the draw-queue comparator is
 * not transitive: distances within 1.0 compare equal, so the algorithm must stay as it is).
 * Neither comparator writes memory, calls anything or touches errno.  debug.fp.native_sort=0 keeps
 * the guest comparators. */
static int32_t draw_queue_compare(uint64_t left,uint64_t right){
    /* 1410310D0: cvttss2si(([[rdx]+0x200] - [[rcx]+0x200])) sign, then [+0x10], [+0x18], [+0x20] differences. */
    float difference=rdss(rd64(right)+0x200ull)-rdss(rd64(left)+0x200ull);
    int32_t truncated=sse_cvtt_i32(difference);
    if(truncated>0)return 1;
    if(truncated<0)return -1;
    uint32_t value=rd32(right+0x10ull)-rd32(left+0x10ull);if(value)return (int32_t)value;
    value=rd32(right+0x18ull)-rd32(left+0x18ull);if(value)return (int32_t)value;
    return (int32_t)(rd32(right+0x20ull)-rd32(left+0x20ull));
}
static int32_t key_descending_compare(uint64_t left,uint64_t right){
    /* 14102F320: mov eax,[rcx]; mov ecx,[rdx]; cmp ecx,eax; ja -> 1; else sbb eax,eax (-1 or 0). */
    uint32_t a=rd32(left),b=rd32(right);
    return b>a?1:b<a?-1:0;
}
/* debug.fp.native_sort: 0 guest comparators, 1 native (default), 2 both on every comparison, the
 * guest result is used and differences are logged (NATIVE_SORT_VERIFY*; diagnostics only). */
static int sort_mode=-1;
static native_compare native_comparator(uint64_t function,uint64_t width){
#ifdef __ANDROID__
    if(sort_mode<0){char value[PROP_VALUE_MAX]={0};int saved=errno;
        sort_mode=__system_property_get("debug.fp.native_sort",value)<=0?1:value[0]=='0'?0:value[0]=='2'?2:1;errno=saved;}
    if(!sort_mode)return NULL;
#endif
    if(function==GVA(0x1410310D0)&&width==0x30)return draw_queue_compare;
    if(function==GVA(0x14102F320)&&width==8)return key_descending_compare;
    return NULL;
}
static int sort_compare(guest_sort *sort,uint64_t left,uint64_t right){
    uint64_t a=sort->base+left*sort->width,b=sort->base+right*sort->width;
    if(sort->native&&sort_mode!=2)return sort->native(a,b);
    win64_pointer(a,(size_t)sort->width,0);win64_pointer(b,(size_t)sort->width,0);
    win64_pointer(sort->frame-8,40,1);
    CPU call=*sort->parent;call.rcx=a;call.rdx=b;call.rsp=sort->frame;
    const uint64_t sentinel=UINT64_C(0xe5ef00d8);
    errno=sort->host_errno;push64(&call,sentinel);
    if(sort->direct){call.rip=sort->function;sort->direct(&call);}else dispatch(&call,sort->function);
    sort->host_errno=errno;
    uint64_t returned;memcpy(&returned,win64_pointer(sort->frame-8,8,0),8);
    if(call.rsp!=sort->frame||returned!=sentinel)utility_stop("qsort comparator shadow/return stack imbalance");
    if(sort->native){
        static _Atomic unsigned long compared,differed;
        unsigned long n=atomic_fetch_add_explicit(&compared,1,memory_order_relaxed)+1;
        int32_t native=sort->native(a,b);
        if(native!=(int32_t)(uint32_t)call.rax){
            unsigned long m=atomic_fetch_add_explicit(&differed,1,memory_order_relaxed)+1;
            if(m<=20)fprintf(stderr,"NATIVE_SORT_VERIFY_MISMATCH function=0x%llx native=%d guest=%d\n",
                             (unsigned long long)sort->function,native,(int32_t)(uint32_t)call.rax);
        }
        if(!(n%200000))fprintf(stderr,"NATIVE_SORT_VERIFY compared=%lu mismatches=%lu\n",n,atomic_load(&differed));
    }
    return (int32_t)(uint32_t)call.rax;
}
static void sort_swap(guest_sort *sort,uint64_t left,uint64_t right){
    if(left==right)return;
    size_t remaining=(size_t)sort->width;
    unsigned char *a=win64_pointer(sort->base+left*sort->width,remaining,1);
    unsigned char *b=win64_pointer(sort->base+right*sort->width,remaining,1);
    while(remaining>=16){
        unsigned char value[16];memcpy(value,a,16);memcpy(a,b,16);memcpy(b,value,16);
        a+=16;b+=16;remaining-=16;
    }
    while(remaining--){unsigned char value=*a;*a++=*b;*b++=value;}
}
static void sort_sift(guest_sort *sort,uint64_t root,uint64_t count){
    while(root<count/2){
        uint64_t child=root*2+1;
        if(child+1<count&&sort_compare(sort,child,child+1)<0)++child;
        if(sort_compare(sort,root,child)>=0)return;
        sort_swap(sort,root,child);root=child;
    }
}
static void windows_qsort(CPU *cpu){
    guest_sort sort={cpu,cpu->rcx,cpu->rdx,cpu->r8,cpu->r9,0,NULL,errno,NULL};
    if((!sort.base&&sort.count)||!sort.width||!sort.function){
        win64_stdio_set_errno(22);utility_stop("qsort invalid-parameter callback required");
    }
    if(!sort.count)return;
    if(sort.width>SIZE_MAX/sort.count||sort.base>UINT64_MAX-sort.count*sort.width)
        utility_stop("qsort array geometry overflow");
    size_t bytes=(size_t)(sort.count*sort.width);win64_pointer(sort.base,bytes,1);
    if(sort.count<2){errno=sort.host_errno;return;}
    if(cpu->rsp<64)utility_stop("qsort comparator stack underflow");
    sort.frame=(cpu->rsp-48)&~UINT64_C(15);win64_pointer(sort.frame-8,40,1);
    sort.direct=SORT_STATIC_TARGET(sort.function);
    sort.native=native_comparator(sort.function,sort.width);
    if(sort.base<sort.frame+32&&sort.frame-8<sort.base+bytes)utility_stop("qsort array overlaps comparator stack");
    /* Bionic API30 has no contextual qsort_r. An in-place heap gives bounded
     * O(n log n) comparisons, no native callback cast and no shared context. */
    for(uint64_t parent=sort.count/2;parent;){--parent;sort_sift(&sort,parent,sort.count);}
    for(uint64_t count=sort.count;count>1;){--count;sort_swap(&sort,0,count);sort_sift(&sort,0,count);}
    win64_pointer(sort.base,bytes,1);errno=sort.host_errno;
}
/* Direct handlers for win64.c's import cache: the same calls as win64_utility_call's branches. */
static int fast_atoi(CPU *cpu){cpu->rax=windows_atoi(cpu->rcx);return 1;}
static int fast_qsort(CPU *cpu){windows_qsort(cpu);return 1;}
static int fast_bsearch(CPU *cpu){cpu->rax=windows_bsearch(cpu);return 1;}
int (*win64_utility_fast_import(const char *name))(CPU *){
    if(!strcmp(name,"atoi"))return fast_atoi;
    if(!strcmp(name,"qsort"))return fast_qsort;
    if(!strcmp(name,"bsearch"))return fast_bsearch;
    return NULL;
}
int win64_utility_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"qsort")){windows_qsort(cpu);return 1;}
    if(!strcmp(name,"bsearch")){cpu->rax=windows_bsearch(cpu);return 1;}
    if(!strcmp(name,"strtod")||!strcmp(name,"strtoul")||!strcmp(name,"strtol")||!strcmp(name,"wcstol")){
        int saved_errno=errno;
        if(!strcmp(name,"strtod"))cpu->xmm[0].u64[0]=windows_strtod(cpu->rcx,cpu->rdx,(cpu->mxcsr>>13)&3u);
        else cpu->rax=windows_integer(cpu->rcx,cpu->rdx,(int32_t)cpu->r8,strcmp(name,"strtoul")!=0,name[0]=='w'?2u:1u);
        errno=saved_errno;return 1;
    }
    if(!strcmp(name,"atoi")){cpu->rax=windows_atoi(cpu->rcx);return 1;}
    if(!strcmp(name,"_atoi64")){cpu->rax=windows_atoi64(cpu->rcx);return 1;}
    if(!strcmp(name,"_itoa_s")){cpu->rax=windows_itoa_s(cpu);return 1;}
    if(!strcmp(name,"srand")){random_state=(uint32_t)win64_argument(cpu,0);cpu->rax=0;return 1;}
    if(!strcmp(name,"rand")){
        random_state=random_state*UINT32_C(214013)+UINT32_C(2531011);
        cpu->rax=(random_state>>16)&UINT32_C(32767);return 1;
    }
    return 0;
}
void win64_utility_self_test(void) {
    uint32_t saved=random_state;CPU cpu={0};cpu.rsp=UINT64_C(0x12345678);cpu.rbx=UINT64_C(0xabcdef);cpu.cf=1;
    static const uint16_t seed1[]={41,18467,6334,26500,19169,15724,11478,29358};
    cpu.rcx=1;int valid=win64_utility_call(&cpu,"srand");
    for(unsigned i=0;i<sizeof(seed1)/sizeof(seed1[0]);++i)
        valid=win64_utility_call(&cpu,"rand") && cpu.rax==seed1[i] && valid;
    valid=valid && cpu.rsp==UINT64_C(0x12345678) && cpu.rbx==UINT64_C(0xabcdef) && cpu.cf && !win64_utility_call(&cpu,"unknown_utility_function");
    random_state=saved;
    if(!valid)utility_stop("random state/protocol self-test");
    fputs("WIN64_UTILITY_SELF_TEST RAND_MAX=32767 LCG32=1 thread_local=1 state_preserved=1 no_rsp_pop=1 passed\n",stderr);
}
