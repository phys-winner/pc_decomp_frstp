/* Bounded UCRT narrow scanf over validated Win64 8-byte argument slots.
 * Independent implementation. No native va_list crosses the guest boundary.
 * Current callers need UUID hexadecimal fields; unsupported floats/wide/locale
 * stop explicitly instead of silently applying Android libc semantics. */
#include "win64_scan.h"
#include "win64.h"
#include "win64_stdio.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {uint64_t input,count,pos,va;unsigned assigned;int failed;} scanner;
static _Noreturn void scan_stop(const char *reason){fprintf(stderr,"STOP Win64 scanner %s\n",reason);exit(73);}
static int space(unsigned c){return c==' '||(c>=9&&c<=13);}
static unsigned digit(unsigned c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return UINT_MAX;}
static int peek(scanner *s){if(s->pos==s->count)return -1;unsigned c=*(const unsigned char *)win64_pointer(s->input+s->pos,1,0);return c?(int)c:-1;}
static int get(scanner *s){int c=peek(s);if(c>=0)++s->pos;return c;}
static void whitespace(scanner *s){int c;while((c=peek(s))>=0&&space((unsigned)c))++s->pos;}
static uint64_t argument(scanner *s){uint64_t value;memcpy(&value,win64_pointer(s->va,8,0),8);if(s->va>UINT64_MAX-8)scan_stop("argument slot overflow");s->va+=8;return value;}
static void store(scanner *s,uint64_t value,unsigned bytes){uint64_t p=argument(s);if(!p){win64_stdio_set_errno(EINVAL);scan_stop("null destination invalid-parameter callback unsupported");}memcpy(win64_pointer(p,bytes,1),&value,bytes);}
static int bounded_peek(scanner *s,uint64_t begin,uint64_t width){return width&&s->pos-begin>=width?-1:peek(s);}

static int integer(scanner *s,unsigned base,int is_signed,unsigned bytes,uint64_t width,int suppress){
    whitespace(s);uint64_t begin=s->pos,value=0;int negative=0,overflow=0;unsigned digits=0;
    int c=bounded_peek(s,begin,width);if(c=='-'||c=='+'){negative=c=='-';++s->pos;c=bounded_peek(s,begin,width);}
    if((!base||base==16)&&c=='0'){
        ++s->pos;int next=bounded_peek(s,begin,width);
        if(next=='x'||next=='X'){++s->pos;base=16;}
        else{base=base?base:8;digits=1;}
    }else if(!base)base=10;
    while((c=bounded_peek(s,begin,width))>=0){unsigned d=digit((unsigned)c);if(d>=base)break;
        if(value>(UINT64_MAX-d)/base)overflow=1;value=value*base+d;++s->pos;++digits;
    }
    if(!digits)return 0;
    if(is_signed&&(value>(negative?UINT64_C(0x8000000000000000):INT64_MAX)))overflow=1;
    if(overflow){win64_stdio_set_errno(ERANGE);value=is_signed?(negative?UINT64_C(0x8000000000000000):INT64_MAX):UINT64_MAX;}
    else if(negative)value=UINT64_C(0)-value;
    if(!suppress)store(s,value,bytes);return 1;
}

static int string(scanner *s,int kind,unsigned bytes,uint64_t width,int suppress,int secure,const unsigned char set[256]){
    if(bytes!=1)scan_stop("UTF16 scanf conversion unsupported");if(kind=='s')whitespace(s);
    uint64_t target=0,capacity=UINT64_MAX;if(!suppress){target=argument(s);if(!target){win64_stdio_set_errno(EINVAL);scan_stop("null string destination invalid-parameter callback unsupported");}
        if(secure)capacity=(uint32_t)argument(s);}
    if(!capacity){win64_stdio_set_errno(ENOMEM);return 0;}
    uint64_t limit=capacity;if(kind!='c'&&limit!=UINT64_MAX)--limit;
    uint64_t n=0;while(!width||n<width){int c=peek(s);if(c<0||(kind=='s'&&space((unsigned)c))||(kind=='['&&!set[c]))break;
        ++s->pos;if(!suppress){if(n==limit){if(kind!='c')*(char *)win64_pointer(target,1,1)=0;else *(char *)win64_pointer(target,1,1)=0;win64_stdio_set_errno(ENOMEM);return 0;}
            *(unsigned char *)win64_pointer(target+n,1,1)=(unsigned char)c;}++n;
    }
    if(!n||(kind=='c'&&n!=width))return 0;
    if(!suppress&&kind!='c')*(char *)win64_pointer(target+n,1,1)=0;return 1;
}

static int scan(uint64_t options,uint64_t input,uint64_t count,uint64_t format,uint64_t locale,uint64_t va){
    if(options&~UINT64_C(3))scan_stop("unsupported scanf option bits");if(locale)scan_stop("non-NULL scanf locale unsupported");
    if(!input||!format){win64_stdio_set_errno(EINVAL);scan_stop("null input/format invalid-parameter callback unsupported");}
    const unsigned char *f=(const unsigned char *)win64_string_a(format);scanner s={input,count,0,va,0,0};
    while(*f){
        if(space(*f)){while(space(*f))++f;whitespace(&s);continue;}
        if(*f!='%'||f[1]=='%'){int expected=*f=='%'?'%':*f;f+=*f=='%'?2:1;
            if(peek(&s)!=expected){s.failed=1;break;}++s.pos;continue;}
        ++f;int suppress=*f=='*';if(suppress)++f;uint64_t width=0;int has_width=0;
        while(*f>='0'&&*f<='9'){has_width=1;if(width>(UINT64_MAX-9)/10)scan_stop("field width overflow");width=width*10+(*f++-'0');}
        if(has_width&&!width)scan_stop("zero scanf field width");
        unsigned bytes=4;int wide=0;
        if(*f=='h'){++f;bytes=2;if(*f=='h'){++f;bytes=1;}}
        else if(*f=='l'){++f;if(*f=='l'){++f;bytes=8;wide=1;}else{bytes=4;wide=1;}}
        else if(*f=='I'){if(f[1]=='3'&&f[2]=='2'){f+=3;bytes=4;}else if(f[1]=='6'&&f[2]=='4'){f+=3;bytes=8;}
            else if(strchr("diouxX",f[1])&&f[1]){++f;bytes=8;}}
        else if(*f=='j'||*f=='z'||*f=='t'){++f;bytes=8;}
        else if(*f=='L'||*f=='w'||*f=='T')scan_stop("wide/long-double scanf length unsupported");
        unsigned spec=*f++;int matched=0;
        if(spec=='n'){if(!suppress)store(&s,s.pos,bytes);continue;}
        if(spec=='d'||spec=='i'||spec=='I'||spec=='u'||spec=='o'||spec=='x'||spec=='X'||spec=='p'){
            unsigned base=spec=='i'||spec=='I'?0:spec=='o'?8:spec=='x'||spec=='X'||spec=='p'?16:10;
            matched=integer(&s,base,spec=='d'||spec=='i'||spec=='I',spec=='p'?8:bytes,width,suppress);
        }else if(spec=='s'||spec=='c'||spec=='['){unsigned char set[256]={0};
            if(wide)scan_stop("UTF16 scanf conversion unsupported");
            if(spec=='['){int invert=*f=='^';if(invert)++f;int first=1,previous=-1;
                while(*f&&(*f!=']'||first)){unsigned c=*f++;first=0;if(c=='-'&&previous>=0&&*f&&*f!=']'){
                        unsigned end=*f++;unsigned lo=(unsigned)previous<end?(unsigned)previous:end,hi=(unsigned)previous>end?(unsigned)previous:end;
                        for(unsigned n=lo;n<=hi;++n)set[n]=1;previous=-1;
                    }else{set[c]=1;previous=(int)c;}}
                if(*f!=']')scan_stop("unterminated scanf scanset");++f;if(invert)for(unsigned n=0;n<256;++n)set[n]=!set[n];}
            matched=string(&s,(int)spec,1,spec=='c'&&!has_width?1:width,suppress,(options&1)!=0,set);
        }else scan_stop("unsupported scanf conversion (floating/wide/unknown)");
        if(!matched){s.failed=1;break;}if(!suppress)++s.assigned;
    }
    return !s.assigned&&s.failed&&peek(&s)<0?-1:(int)s.assigned;
}
int win64_scan_call(CPU *c,const char *name){if(strcmp(name,"__stdio_common_vsscanf"))return 0;
    int result=scan(c->rcx,c->rdx,c->r8,c->r9,win64_argument(c,4),win64_argument(c,5));c->rax=(uint32_t)result;return 1;}

static int test_call(const char *input,const char *format,uint64_t options,uint64_t count,uint64_t *args,size_t arguments,void *output,size_t bytes){
    char input_copy[256],format_copy[512];if(strlen(input)>=sizeof(input_copy)||strlen(format)>=sizeof(format_copy))scan_stop("self-test string size");strcpy(input_copy,input);strcpy(format_copy,format);input=input_copy;format=format_copy;
    uint64_t stack[8]={0};CPU c={0};c.rsp=(uintptr_t)stack;c.rcx=options;c.rdx=(uintptr_t)input;c.r8=count;c.r9=(uintptr_t)format;stack[6]=(uintptr_t)args;
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)input,strlen(input)+1,0);win64_region_add((uintptr_t)format,strlen(format)+1,0);
    win64_region_add((uintptr_t)args,arguments*8,0);win64_region_add((uintptr_t)output,bytes,1);
    if(!win64_scan_call(&c,"__stdio_common_vsscanf")||c.rsp!=(uintptr_t)stack)scan_stop("self-test ABI");
    win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)args);win64_region_remove((uintptr_t)format);win64_region_remove((uintptr_t)input);win64_region_remove((uintptr_t)stack);return(int32_t)c.rax;
}
void win64_scan_self_test(void){
    uint32_t out[18];uint64_t va[18];for(unsigned i=0;i<18;++i){out[i]=0xA5A5A5A5;va[i]=(uintptr_t)&out[i];}
    const char *format="%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X";
    if(test_call("00112233-4455-6677-8899-AABBCCDDEEFF",format,3,UINT64_MAX,va,16,out,sizeof(out))!=16)scan_stop("self-test actual UUID result");
    for(unsigned i=0;i<16;++i)if(out[i]!=i*17)scan_stop("self-test actual UUID bytes");if(out[16]!=0xA5A5A5A5||out[17]!=0xA5A5A5A5)scan_stop("self-test UUID canary");
    struct {const char *in,*format;int result;uint32_t value;} cases[]={
        {"0x","%i",-1,0xA5A5A5A5},{"0xZ","%x",0,0xA5A5A5A5},{"+","%d",-1,0xA5A5A5A5},
        {"1","%*d%d",-1,0xA5A5A5A5},{"","%n",0,0},{"","",0,0xA5A5A5A5},
        {"-42","%ld",1,UINT32_C(0)-42},{"1234","%2u",1,12},{"077","%i",1,63},{"%17","%%%d",1,17}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i){out[0]=0xA5A5A5A5;if(test_call(cases[i].in,cases[i].format,3,UINT64_MAX,va,1,out,sizeof(out))!=cases[i].result||out[0]!=cases[i].value)scan_stop("self-test integer/EOF");}
    fprintf(stderr,"SCAN_SELF_TEST passed actual_UUID16=1 options_secure_legacywide3=1 LLP64_long32=1 width_suppress_literals=1 prefix_EOF=1 count_n=1 canaries_RSP=1 cases=11 floats_wide_locale=explicit_unsupported\n");
}
