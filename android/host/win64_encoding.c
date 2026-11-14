/* Windows UTF-16/UTF-8 ABI. Guest ACP/OEMCP explicitly equal65001. */
#include "win64_encoding.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void encoding_stop(const char *reason){fprintf(stderr,"STOP Win64 encoding %s\n",reason);fflush(stderr);_Exit(73);}
static uint64_t encoding_error(uint32_t error){win64_set_error(error);return 0;}
static uint16_t unit(const unsigned char *text,size_t at){uint16_t value;memcpy(&value,text+at*2,2);return value;}
static unsigned utf8_scalar(uint32_t value,unsigned char bytes[4]){
    if(value<0x80){bytes[0]=(unsigned char)value;return 1;}
    if(value<0x800){bytes[0]=(unsigned char)(0xc0|(value>>6));bytes[1]=(unsigned char)(0x80|(value&63));return 2;}
    if(value<0x10000){bytes[0]=(unsigned char)(0xe0|(value>>12));bytes[1]=(unsigned char)(0x80|((value>>6)&63));bytes[2]=(unsigned char)(0x80|(value&63));return 3;}
    bytes[0]=(unsigned char)(0xf0|(value>>18));bytes[1]=(unsigned char)(0x80|((value>>12)&63));bytes[2]=(unsigned char)(0x80|((value>>6)&63));bytes[3]=(unsigned char)(0x80|(value&63));return 4;
}
static uint64_t wide_to_utf8(CPU *cpu){
    uint32_t page=(uint32_t)cpu->rcx,flags=(uint32_t)cpu->rdx;uint64_t source=cpu->r8;int32_t units=(int32_t)cpu->r9;
    uint64_t destination=win64_argument(cpu,4);int32_t capacity=(int32_t)win64_argument(cpu,5);
    (void)win64_argument(cpu,6);uint64_t used_default=win64_argument(cpu,7);
    if(page!=0&&page!=1&&page!=65001)encoding_stop("unsupported codepage (guest ACP/OEMCP=UTF8)");
    if(!source||!units||capacity<0||(capacity&&(!destination||destination==source)))return encoding_error(87);
    /* Pinned Windows accepts these legacy bits; only0x80 changes UTF8 behavior. */
    if(flags&~UINT32_C(0x6f0))return encoding_error(1004);
    /* Pinned Windows UTF8 accepts/ignores lpDefaultChar. */
    size_t count;
    if(units<0){
        /* Actual Windows treats every negative count as NUL-terminated. */
        for(count=0;;++count){
            if(count==INT32_MAX)return encoding_error(87);
            if(source>UINT64_MAX-count*2)encoding_stop("UTF16 address overflow");
            uint16_t value;memcpy(&value,win64_pointer(source+count*2,2,0),2);if(!value){++count;break;}
        }
    }else count=(size_t)units;
    const unsigned char *input=win64_pointer(source,count*2,0);
    unsigned char *output=capacity?win64_pointer(destination,(size_t)capacity,1):NULL;
    /* shortcut: reject partial overlaps87; add a tested alias policy if a real caller needs it. */
    if(capacity&&source<destination+(uint32_t)capacity&&destination<source+count*2)return encoding_error(87);
    size_t written=0;int invalid=0;
    for(size_t i=0;i<count;++i){uint32_t value=unit(input,i);
        if(value>=0xd800&&value<=0xdbff){
            if(i+1<count){uint32_t low=unit(input,i+1);if(low>=0xdc00&&low<=0xdfff){value=0x10000+((value-0xd800)<<10)+(low-0xdc00);++i;}else{value=0xfffd;invalid=1;}}
            else{value=0xfffd;invalid=1;}
        }else if(value>=0xdc00&&value<=0xdfff){value=0xfffd;invalid=1;}
        unsigned char bytes[4];unsigned n=utf8_scalar(value,bytes);
        if(written>INT32_MAX-n)return encoding_error(122);
        if(capacity&&(n>(size_t)capacity-written))return encoding_error(122);
        if(output)memcpy(output+written,bytes,n);written+=n;
    }
    /* Publish only after conversion completes, including strict-invalid failure. */
    if(used_default){uint32_t replacement=(uint32_t)invalid;memcpy(win64_pointer(used_default,4,1),&replacement,4);}
    /* Buffer exhaustion takes precedence, and complete prefixes remain written. */
    if(invalid&&(flags&0x80))return encoding_error(1113);
    return written;
}
static size_t utf8_value(const unsigned char *text,size_t count,uint32_t *value,int *invalid){
    unsigned first=text[0],length;uint32_t scalar;
    if(first<0x80){*value=first;return 1;}
    if(first>=0xc2&&first<=0xdf){length=2;scalar=first&31;}
    else if(first>=0xe0&&first<=0xef){length=3;scalar=first&15;}
    else if(first>=0xf0&&first<=0xf4){length=4;scalar=first&7;}
    else{*value=0xfffd;*invalid=1;return 1;}
    size_t used=1;
    for(unsigned at=1;at<length;++at){
        if(used==count || (text[used]&0xc0)!=0x80){*value=0xfffd;*invalid=1;return used;}
        unsigned byte=text[used++];scalar=(scalar<<6)|(byte&63);
        /* Pinned Windows consumes the first continuation before detecting
         * an overlong, surrogate or out-of-range prefix (two-byte replacement). */
        if(at==1 && ((first==0xe0&&byte<0xa0)||(first==0xed&&byte>=0xa0)||
                     (first==0xf0&&byte<0x90)||(first==0xf4&&byte>=0x90))){*value=0xfffd;*invalid=1;return used;}
    }
    *value=scalar;return used;
}
static uint64_t utf8_to_wide(CPU *cpu){
    uint32_t page=(uint32_t)cpu->rcx,flags=(uint32_t)cpu->rdx;uint64_t source=cpu->r8;int32_t bytes=(int32_t)cpu->r9;
    uint64_t destination=win64_argument(cpu,4);int32_t capacity=(int32_t)win64_argument(cpu,5);
    if(page!=0&&page!=1&&page!=65001)encoding_stop("unsupported codepage (guest ACP/OEMCP=UTF8)");
    if(!source||!bytes||capacity<0||(capacity&&(!destination||destination==source)))return encoding_error(87);
    /* Pinned Windows accepts legacy low bits; only MB_ERR_INVALID_CHARS8
     * changes UTF8 conversion. Query and output share replacement decoding. */
    if(flags&~UINT32_C(15))return encoding_error(1004);
    size_t count;
    if(bytes<0){
        for(count=0;;++count){
            if(count==INT32_MAX)return encoding_error(87);
            if(source>UINT64_MAX-count)encoding_stop("UTF8 address overflow");
            if(!*(const unsigned char*)win64_pointer(source+count,1,0)){++count;break;}
        }
    }else count=(size_t)bytes;
    const unsigned char *input=win64_pointer(source,count,0);
    unsigned char *output=capacity?win64_pointer(destination,(size_t)capacity*2,1):NULL;
    /* shortcut: reject partial overlaps87; add a tested alias policy if a real caller needs it. */
    if(capacity&&source<destination+(size_t)capacity*2&&destination<source+count)return encoding_error(87);
    size_t written=0;int invalid=0;
    for(size_t at=0;at<count;){
        uint32_t value;at+=utf8_value(input+at,count-at,&value,&invalid);uint16_t wide[2];unsigned length=1;
        if(value<0x10000)wide[0]=(uint16_t)value;
        else{value-=0x10000;wide[0]=(uint16_t)(0xd800+(value>>10));wide[1]=(uint16_t)(0xdc00+(value&1023));length=2;}
        for(unsigned i=0;i<length;++i){
            if(written==INT32_MAX || (capacity&&written==(size_t)capacity))return encoding_error(122);
            if(output)memcpy(output+written*2,&wide[i],2);++written;
        }
    }
    /* Native short-buffer output may end in a high surrogate. Buffer122
     * precedes strict1113, while a complete invalid conversion retains U+FFFD. */
    if(invalid&&(flags&8))return encoding_error(1113);return written;
}
int win64_encoding_call(CPU *cpu,const char *name){
    if(!strcmp(name,"WideCharToMultiByte")){cpu->rax=wide_to_utf8(cpu);return 1;}
    if(!strcmp(name,"MultiByteToWideChar")){cpu->rax=utf8_to_wide(cpu);return 1;}return 0;
}
void win64_encoding_self_test(void){
    unsigned char bytes[4];if(utf8_scalar(0x1f328,bytes)!=4||memcmp(bytes,"\xf0\x9f\x8c\xa8",4)||utf8_scalar(0xfffd,bytes)!=3||memcmp(bytes,"\xef\xbf\xbd",3))encoding_stop("UTF8 scalar self-test");
    uint32_t value;int invalid=0;if(utf8_value((const unsigned char*)"\xf0\x9f\x8c\xa8",4,&value,&invalid)!=4||value!=0x1f328||invalid ||
       utf8_value((const unsigned char*)"\xe0\x80\x80",3,&value,&invalid)!=2||value!=0xfffd||!invalid)encoding_stop("UTF8 decode/replacement self-test");
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;if(win64_encoding_call(&cpu,"unknown_encoding_function")||memcmp(&cpu,&before,sizeof(cpu)))encoding_stop("unknown API self-test");
}
