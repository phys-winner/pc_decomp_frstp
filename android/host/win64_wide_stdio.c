/* shortcut: Only Frostpunk's eleven original UTF16 path/color/UI formats are supported.
 * Extend from an actual caller and native UCRT oracle, never host wchar_t/va_list. */
#include "win64_wide_stdio.h"
#include "win64.h"
#include "win64_stdio.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void stop_wide(const char *reason) {
    fprintf(stderr,"STOP Win64 wide stdio %s\n",reason);fflush(stderr);_Exit(73);
}
static _Noreturn void invalid_wide(int error,const char *reason) {
    win64_stdio_set_errno(error);stop_wide(reason);
}
static uint16_t unit(uint64_t address,size_t index) {
    uint16_t value;
    if(index>(UINT64_MAX-address)/2)stop_wide("UTF16 address overflow");
    memcpy(&value,win64_pointer(address+index*2,2,0),2);return value;
}
static _Noreturn void unsupported_format(CPU *c,const char *name,uint64_t address) {
    uint64_t caller;memcpy(&caller,win64_pointer(c->rsp,8,0),8);
    fprintf(stderr,"STOP Win64 wide stdio unsupported UTF16 format api=%s guest_return=0x%" PRIx64 " format=0x%" PRIx64 " units=",name,caller,address);fflush(stderr);
    for(size_t i=0;i<128;++i){
        uint16_t value=unit(address,i);fprintf(stderr,"%s%04x",i?" ":"",(unsigned)value);fflush(stderr);
        if(!value){fputc('\n',stderr);fflush(stderr);_Exit(73);}
    }
    fprintf(stderr," [max128]\n");fflush(stderr);_Exit(73);
}
static int path_format(CPU *c,const char *name,uint64_t address) {
    static const char *const formats[]={"%s/%s","%s/11bitstudios/%s","%s/%s.dat","_%.3d%s","%s/custom_localizations","%s.alt","|#color=%02x%02x%02x|","%s.%s","%s - %s","Saved game %3d","%.*s..."};
    unsigned candidates=2047;
    for(size_t i=0;candidates;++i){
        uint16_t value=unit(address,i);
        for(unsigned j=0;j<11;++j)if(candidates&(1u<<j)){
            unsigned expected=(unsigned char)formats[j][i];
            if(value!=expected)candidates&=~(1u<<j);
            else if(!value)return(int)j;
        }
    }
    unsupported_format(c,name,address);
}
typedef struct {uint64_t guest;const uint16_t *literal;size_t length;} piece;
static piece argument_string(uint64_t address) {
    static const uint16_t null_text[]={'(','n','u','l','l',')',0};
    piece result={address,NULL,0};
    if(!address){result.literal=null_text;result.length=6;return result;}
    while(unit(address,result.length)){
        if(result.length==INT_MAX)stop_wide("path string exceeds signed32 result contract");
        ++result.length;
    }
    return result;
}
static size_t decimal3(uint64_t bits,uint16_t output[12]) {
    uint32_t value=(uint32_t)bits;int negative=(value&UINT32_C(0x80000000))!=0;
    uint32_t magnitude=negative?UINT32_C(0)-value:value;uint16_t reverse[10];size_t digits=0,used=0;
    do{reverse[digits++]=(uint16_t)('0'+magnitude%10);magnitude/=10;}while(magnitude);
    if(negative)output[used++]='-';
    for(size_t pad=digits;pad<3;++pad)output[used++]='0';
    while(digits)output[used++]=reverse[--digits];return used;
}
static size_t hex2(uint64_t bits,uint16_t output[8]) {
    static const char digits[]="0123456789abcdef";
    uint32_t value=(uint32_t)bits;uint16_t reverse[8];size_t used=0,length=0;
    do{reverse[used++]=(uint16_t)digits[value&15];value>>=4;}while(value);
    if(used<2)output[length++]='0';
    while(used)output[length++]=reverse[--used];return length;
}
static piece precision_string(uint64_t address,int32_t precision) {
    if(precision<0)return argument_string(address);
    if(!address){piece result=argument_string(0);if(result.length>(uint32_t)precision)result.length=(uint32_t)precision;return result;}
    piece result={address,NULL,0};
    while(result.length<(uint32_t)precision&&unit(address,result.length))++result.length;
    return result;
}
static size_t decimal_width3(uint64_t bits,uint16_t output[12]) {
    uint32_t value=(uint32_t)bits;int negative=(value&UINT32_C(0x80000000))!=0;
    uint32_t magnitude=negative?UINT32_C(0)-value:value;uint16_t reverse[10];size_t digits=0,used=0;
    do{reverse[digits++]=(uint16_t)('0'+magnitude%10);magnitude/=10;}while(magnitude);
    for(size_t width=digits+(unsigned)negative;width<3;++width)output[used++]=' ';
    if(negative)output[used++]='-';
    while(digits)output[used++]=reverse[--digits];return used;
}
static int format_path(CPU *c,const char *name,int secure,uint64_t options,uint64_t buffer,uint64_t capacity,
                       uint64_t count,uint64_t format,uint64_t locale,uint64_t va) {
    if(options!=(secure?UINT64_C(4):UINT64_C(6)))stop_wide("unsupported printf options");
    if(locale)stop_wide("non-NULL locale unsupported");
    if(!format)invalid_wide(EINVAL,"NULL format invalid-parameter callback unsupported");
    int kind=path_format(c,name,format);
    if(!secure){
        if(!buffer&&capacity)invalid_wide(EINVAL,"NULL nonzero-capacity buffer invalid-parameter callback unsupported");
        if(buffer||capacity)stop_wide("non-query vswprintf call unsupported");
    }else{
        if(!count&&!buffer&&!capacity)return 0;
        if(!buffer||!capacity)invalid_wide(EINVAL,"secure destination invalid-parameter callback unsupported");
    }
    uint64_t arguments[3]={0,0,0};size_t argument_bytes=kind==6?24:(kind==4||kind==5||kind==9)?8:16;
    memcpy(arguments,win64_pointer(va,argument_bytes,0),argument_bytes);
    static const uint16_t slash[]={'/'};
    static const uint16_t studio[]={'/','1','1','b','i','t','s','t','u','d','i','o','s','/'};
    static const uint16_t dat[]={'.','d','a','t'};
    static const uint16_t custom[]={'/','c','u','s','t','o','m','_','l','o','c','a','l','i','z','a','t','i','o','n','s'};
    static const uint16_t alt[]={'.','a','l','t'};
    static const uint16_t underscore[]={'_'};uint16_t number[12],color[33];piece parts[4];
    if(kind>=7){
        static const uint16_t dot[]={'.'},dash[]={' ','-',' '},saved[]={'S','a','v','e','d',' ','g','a','m','e',' '},ellipsis[]={'.','.','.'};
        if(kind==9){parts[0]=(piece){0,saved,11};parts[1]=(piece){0,number,decimal_width3(arguments[0],number)};parts[2]=parts[3]=(piece){0,NULL,0};}
        else if(kind==10){parts[0]=precision_string(arguments[1],(int32_t)(uint32_t)arguments[0]);parts[1]=(piece){0,ellipsis,3};parts[2]=parts[3]=(piece){0,NULL,0};}
        else{parts[0]=argument_string(arguments[0]);parts[1]=kind==7?(piece){0,dot,1}:(piece){0,dash,3};parts[2]=argument_string(arguments[1]);parts[3]=(piece){0,NULL,0};}
    }
    else if(kind==6){
        static const uint16_t prefix[]={'|','#','c','o','l','o','r','='};size_t used=sizeof(prefix)/sizeof(prefix[0]);
        memcpy(color,prefix,sizeof(prefix));
        for(unsigned i=0;i<3;++i)used+=hex2(arguments[i],color+used);
        color[used++]='|';parts[0]=(piece){0,color,used};parts[1]=parts[2]=parts[3]=(piece){0,NULL,0};
    }
    else if(kind==3){parts[0]=(piece){0,underscore,1};parts[1]=(piece){0,number,decimal3(arguments[0],number)};parts[2]=argument_string(arguments[1]);parts[3]=(piece){0,NULL,0};}
    else if(kind>=4){parts[0]=argument_string(arguments[0]);parts[1]=kind==4?(piece){0,custom,sizeof(custom)/sizeof(custom[0])}:(piece){0,alt,sizeof(alt)/sizeof(alt[0])};parts[2]=parts[3]=(piece){0,NULL,0};}
    else{parts[0]=argument_string(arguments[0]);parts[1]=(piece){0,kind==1?studio:slash,kind==1?14u:1u};parts[2]=argument_string(arguments[1]);parts[3]=(piece){0,dat,kind==2?4u:0u};}
    size_t length=parts[0].length+parts[1].length+parts[2].length+parts[3].length;
    if(length>INT_MAX)stop_wide("formatted path exceeds signed32 result contract");
    if(!secure)return(int)length;
    if(capacity>SIZE_MAX/2)stop_wide("UTF16 buffer size overflow");
    unsigned char *destination=win64_pointer(buffer,(size_t)capacity*2,1);
    size_t allowed=capacity-1;
    if(count<allowed)allowed=(size_t)count;
    if(length>allowed&&count>=capacity&&count!=UINT64_MAX){
        uint16_t zero=0;memcpy(destination,&zero,2);
        invalid_wide(ERANGE,"secure range invalid-parameter callback unsupported");
    }
    size_t written=0;
    for(unsigned i=0;i<4&&written<allowed;++i){
        size_t n=parts[i].length;if(n>allowed-written)n=allowed-written;
        if(n){const void *source=parts[i].literal?(const void *)parts[i].literal:win64_pointer(parts[i].guest,n*2,0);
            memmove(destination+written*2,source,n*2);written+=n;}
    }
    uint16_t zero=0;memcpy(destination+written*2,&zero,2);
    /* Native secure formatting also terminates the final slot in this branch. */
    if(count>=capacity)memcpy(destination+((size_t)capacity-1)*2,&zero,2);
    return length>allowed?-1:(int)length;
}
int win64_wide_stdio_call(CPU *c,const char *name) {
    int secure=!strcmp(name,"__stdio_common_vsnwprintf_s");
    if(!secure&&strcmp(name,"__stdio_common_vswprintf"))return 0;
    uint64_t format=secure?win64_argument(c,4):c->r9;
    uint64_t locale=win64_argument(c,secure?5:4);
    uint64_t arguments=win64_argument(c,secure?6:5);
    int result=format_path(c,name,secure,c->rcx,c->rdx,c->r8,secure?c->r9:UINT64_MAX,format,locale,arguments);
    c->rax=(uint32_t)result;return 1;
}

static void check(int condition,const char *message){if(!condition)stop_wide(message);}
void win64_wide_stdio_self_test(void) {
    uint16_t format[32],left[]={'A',0xD83D,0xDE00,0},right[]={0xD800,'B',0},output[40];
    uint64_t arguments[3]={(uintptr_t)left,(uintptr_t)right,0},stack[9];CPU c,expected;
    uint16_t wanted[]={'A',0xD83D,0xDE00,'/',0xD800,'B',0};
    const char *fmt="%s/%s";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    win64_region_add((uintptr_t)format,sizeof(format),0);win64_region_add((uintptr_t)left,sizeof(left),0);
    win64_region_add((uintptr_t)right,sizeof(right),0);win64_region_add((uintptr_t)arguments,sizeof(arguments),0);
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)output,sizeof(output),1);
    for(unsigned secure=0;secure<2;++secure){
        memset(&c,0xA5,sizeof(c));memset(stack,0xC7,sizeof(stack));memset(output,0x5A,sizeof(output));
        c.rsp=(uintptr_t)stack;c.rcx=secure?4:6;c.rdx=secure?(uintptr_t)output:0;c.r8=secure?7:0;
        c.r9=secure?6:(uintptr_t)format;stack[5]=secure?(uintptr_t)format:0;stack[6]=secure?0:(uintptr_t)arguments;stack[7]=(uintptr_t)arguments;
        expected=c;expected.rax=6;uint64_t stack_before[9],args_before[3];memcpy(stack_before,stack,sizeof(stack));memcpy(args_before,arguments,sizeof(arguments));
        check(win64_wide_stdio_call(&c,secure?"__stdio_common_vsnwprintf_s":"__stdio_common_vswprintf"),"self-test dispatch");
        check(!memcmp(&c,&expected,sizeof(c))&&!memcmp(stack,stack_before,sizeof(stack))&&!memcmp(arguments,args_before,sizeof(arguments)),"self-test full CPU/stack/va preservation");
        if(secure)check(!memcmp(output,wanted,sizeof(wanted))&&output[7]==0x5A5A,"self-test UTF16 units/canary");
    }
    memset(&c,0,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=4;c.rdx=(uintptr_t)output;c.r8=3;c.r9=2;
    stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==UINT32_MAX&&output[0]=='A'&&output[1]==0xD83D&&output[2]==0,"self-test UTF16 unit truncation");
    fmt="%s/11bitstudios/%s";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    c.rcx=6;c.rdx=0;c.r8=0;c.r9=(uintptr_t)format;stack[5]=0;stack[6]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==19,"self-test actual project literal");
    fmt="%s/%s.dat";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==10,"self-test actual archive literal query");
    c.rcx=4;c.rdx=(uintptr_t)output;c.r8=11;c.r9=10;stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;memset(output,0x5A,sizeof(output));
    const uint16_t wanted_dat[]={'A',0xD83D,0xDE00,'/',0xD800,'B','.','d','a','t',0};
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==10&&!memcmp(output,wanted_dat,sizeof(wanted_dat))&&output[11]==0x5A5A,"self-test actual archive literal write/canary");
    fmt="_%.3d%s";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];arguments[0]=UINT64_C(0xA5A5A5A5FFFFFFF9);
    c.rcx=6;c.rdx=0;c.r8=0;c.r9=(uintptr_t)format;stack[5]=0;stack[6]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==7,"self-test archive index signed32 precision query");
    c.rcx=4;c.rdx=(uintptr_t)output;c.r8=8;c.r9=7;stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;memset(output,0x5A,sizeof(output));
    const uint16_t wanted_index[]={'_','-','0','0','7',0xD800,'B',0};
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==7&&!memcmp(output,wanted_index,sizeof(wanted_index))&&output[8]==0x5A5A&&arguments[0]==UINT64_C(0xA5A5A5A5FFFFFFF9),"self-test archive index write/high32/va/canary");
    fmt="%s/custom_localizations";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    arguments[0]=(uintptr_t)left;arguments[1]=UINT64_MAX;
    win64_region_remove((uintptr_t)arguments);win64_region_add((uintptr_t)arguments,8,0);
    c.rcx=6;c.rdx=0;c.r8=0;c.r9=(uintptr_t)format;stack[5]=0;stack[6]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==24&&arguments[1]==UINT64_MAX,"self-test custom localization single-QWORD query");
    c.rcx=4;c.rdx=(uintptr_t)output;c.r8=25;c.r9=24;stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;memset(output,0x5A,sizeof(output));
    const uint16_t wanted_custom[]={'A',0xD83D,0xDE00,'/','c','u','s','t','o','m','_','l','o','c','a','l','i','z','a','t','i','o','n','s',0};
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==24&&!memcmp(output,wanted_custom,sizeof(wanted_custom))&&output[25]==0x5A5A&&arguments[1]==UINT64_MAX,"self-test custom localization one-slot write/unused-second-slot/canary");
    fmt="%s.alt";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    c.rcx=6;c.rdx=0;c.r8=0;c.r9=(uintptr_t)format;stack[5]=0;stack[6]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==7,"self-test alternate path single-QWORD query");
    c.rcx=4;c.rdx=(uintptr_t)output;c.r8=8;c.r9=7;stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;memset(output,0x5A,sizeof(output));
    const uint16_t wanted_alt[]={'A',0xD83D,0xDE00,'.','a','l','t',0};
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==7&&!memcmp(output,wanted_alt,sizeof(wanted_alt))&&output[8]==0x5A5A&&arguments[1]==UINT64_MAX,"self-test alternate path one-slot write/canary");
    fmt="|#color=%02x%02x%02x|";for(size_t i=0;i<=strlen(fmt);++i)format[i]=(unsigned char)fmt[i];
    arguments[0]=UINT64_C(0xA5A5A5A500000000);arguments[1]=UINT64_C(0xB6B6B6B6000000FF);arguments[2]=UINT64_C(0xC7C7C7C700000123);
    win64_region_remove((uintptr_t)arguments);win64_region_add((uintptr_t)arguments,sizeof(arguments),0);
    c.rcx=6;c.rdx=0;c.r8=0;c.r9=(uintptr_t)format;stack[5]=0;stack[6]=(uintptr_t)arguments;
    check(win64_wide_stdio_call(&c,"__stdio_common_vswprintf")&&c.rax==16,"self-test color three-U32 query");
    c.rcx=4;c.rdx=(uintptr_t)output;c.r8=17;c.r9=16;stack[5]=(uintptr_t)format;stack[6]=0;stack[7]=(uintptr_t)arguments;memset(output,0x5A,sizeof(output));
    const uint16_t wanted_color[]={'|','#','c','o','l','o','r','=','0','0','f','f','1','2','3','|',0};
    expected=c;expected.rax=16;
    check(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(output,wanted_color,sizeof(wanted_color))&&output[17]==0x5A5A,"self-test color lowerhex/minwidth/high32/three-slots/fullCPU/canary");
    win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)stack);win64_region_remove((uintptr_t)arguments);
    win64_region_remove((uintptr_t)right);win64_region_remove((uintptr_t)left);win64_region_remove((uintptr_t)format);
    fprintf(stderr,"WIDE_STDIO_SELF_TEST passed formats=7 color_three_U32_va_slots=1 archive_dat_suffix=1 signed32_precision3=1 custom_localizations_single_va_slot=1 alt_single_va_slot=1 query_options6=1 secure_options4=1 UTF16_units_lone_surrogates=1 truncation=1 full_CPU_RSP_canaries_va=1 no_native_va_list=1\n");
}
