#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
typedef struct {uint64_t expected_rsp,expected_rip,post_rsp,result,unused;uint32_t mxcsr;uint16_t cw,pad;uint32_t post_mxcsr;uint16_t post_cw,pad2;uint64_t regs[8];unsigned char xmm[160];} observation;
_Static_assert(sizeof(_JUMP_BUFFER)==256,"Win64 buffer256");
_Static_assert(__alignof(_JUMP_BUFFER)==16,"Win64 buffer alignment16");
_Static_assert(offsetof(_JUMP_BUFFER,Frame)==0&&offsetof(_JUMP_BUFFER,Rbx)==8&&offsetof(_JUMP_BUFFER,Rsp)==16&&offsetof(_JUMP_BUFFER,Rbp)==24&&offsetof(_JUMP_BUFFER,Rsi)==32&&offsetof(_JUMP_BUFFER,Rdi)==40&&offsetof(_JUMP_BUFFER,R12)==48&&offsetof(_JUMP_BUFFER,R13)==56&&offsetof(_JUMP_BUFFER,R14)==64&&offsetof(_JUMP_BUFFER,R15)==72&&offsetof(_JUMP_BUFFER,Rip)==80&&offsetof(_JUMP_BUFFER,MxCsr)==88&&offsetof(_JUMP_BUFFER,FpCsr)==92&&offsetof(_JUMP_BUFFER,Spare)==94&&offsetof(_JUMP_BUFFER,Xmm6)==96&&offsetof(_JUMP_BUFFER,Xmm15)==240,"SDK x64 setjmp offsets");
_Static_assert(offsetof(observation,regs)==56&&offsetof(observation,xmm)==120,"assembly observation layout");
void jump_capture(void*,void*,uint64_t,observation*);
unsigned short jump_read_cw(void);
void jump_write_cw(unsigned short);
static unsigned records,aligned,negative;
static void hex(const unsigned char *p,size_t n){for(size_t i=0;i<n;++i)printf("%02x",p[i]);}
static int one(void *fn,int offset,uint64_t frame,unsigned csr,unsigned short cw){
 __declspec(align(16)) unsigned char memory[320],expected[256];memset(memory,0xa5,sizeof(memory));memset(expected,0xa5,sizeof(expected));
 unsigned char *buffer=offset<0?NULL:memory+16+offset;observation state={0};state.mxcsr=csr;state.cw=cw;
 unsigned original=_mm_getcsr();unsigned short original_cw=jump_read_cw();DWORD exception=0;
 __try{jump_capture(fn,buffer,frame,&state);}__except(exception=GetExceptionCode(),EXCEPTION_EXECUTE_HANDLER){}
 _mm_setcsr(original);jump_write_cw(original_cw);
 int outside=1,spare=1,nv=1,controls=1,fields=1;
 if(buffer){for(size_t i=0;i<sizeof(memory);++i)if((memory+i<buffer||memory+i>=buffer+256)&&memory[i]!=0xa5)outside=0;spare=buffer[94]==0xa5&&buffer[95]==0xa5;}
 memcpy(expected,&frame,8);memcpy(expected+16,&state.expected_rsp,8);memcpy(expected+80,&state.expected_rip,8);memcpy(expected+88,&csr,4);memcpy(expected+92,&cw,2);
 const unsigned offsets[]={8,24,32,40,48,56,64,72};
 for(unsigned i=0;i<8;++i){uint64_t value=UINT64_C(0x1111111122222201)+i;memcpy(expected+offsets[i],&value,8);if(state.regs[i]!=value)nv=0;}
 for(unsigned i=0;i<160;++i){expected[96+i]=(unsigned char)i;if(state.xmm[i]!=(unsigned char)i)nv=0;}
 if(offset==0){if(exception||memcmp(buffer,expected,256)||state.result||state.post_rsp!=state.expected_rsp)return 10;controls=state.post_mxcsr==csr&&state.post_cw==cw;if(!nv||!controls)return 11;++aligned;}
 else{if(exception!=EXCEPTION_ACCESS_VIOLATION)return 12;if(buffer)fields=!memcmp(buffer,expected,94);if(!fields)return 13;++negative;}
 if(!outside||!spare)return 14;
 printf("%s{\"offset\":%d,\"frame\":\"0x%llx\",\"input_mxcsr\":\"0x%x\",\"input_cw\":\"0x%x\",\"exception_code\":\"0x%lx\",\"result\":%llu,\"expected_rsp\":\"0x%llx\",\"expected_rip\":\"0x%llx\",\"spare_untouched\":%s,\"outside_untouched\":%s,\"scalar_fields_match\":%s,\"post_NV_preserved\":%s,\"post_controls_preserved\":%s,\"buffer_hex\":\"",records++?",":"",offset,(unsigned long long)frame,csr,cw,(unsigned long)exception,(unsigned long long)state.result,(unsigned long long)state.expected_rsp,(unsigned long long)state.expected_rip,buffer?"true":"null",buffer?"true":"null",buffer?"true":"null",offset==0?"true":"null",offset==0?"true":"null");
 if(buffer)hex(buffer,256);printf("\",\"normalized_buffer_hex\":\"");if(offset==0){memset(expected+16,0,8);memset(expected+80,0,8);hex(expected,256);}printf("\"}");return 0;
}
int main(void){
 wchar_t path[MAX_PATH],actual[MAX_PATH];UINT length=GetSystemDirectoryW(path,MAX_PATH);if(!length||length+20>=MAX_PATH)return 2;wcscat_s(path,MAX_PATH,L"\\vcruntime140.dll");HMODULE dll=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)return 3;if(!GetModuleFileNameW(dll,actual,MAX_PATH)||_wcsicmp(path,actual))return 5;void *fn=(void*)GetProcAddress(dll,"__intrinsic_setjmp");if(!fn)return 4;
 printf("{\"SDK_size\":%zu,\"SDK_alignment\":%zu,\"records\":[",sizeof(_JUMP_BUFFER),(size_t)__alignof(_JUMP_BUFFER));
 const uint64_t frames[]={0,UINT64_C(0x123456789abcdef0),UINT64_MAX};const unsigned short words[]={0x027f,0x067f,0x0a7f,0x0e7f,0x037f,0x0f7f};
 for(unsigned r=0;r<4;++r)for(unsigned daz=0;daz<2;++daz)for(unsigned ftz=0;ftz<2;++ftz)for(unsigned w=0;w<6;++w)for(unsigned f=0;f<3;++f){int status=one(fn,0,frames[f],0x1f80|(r<<13)|(daz<<6)|(ftz<<15)|0x2a,words[w]);if(status)return status;}
 for(int offset=1;offset<16;++offset){int status=one(fn,offset,frames[1],0x1f80,0x027f);if(status)return status;}
 int status=one(fn,-1,0,0x1f80,0x027f);if(status)return status;
 printf("],\"aligned_cases\":%u,\"negative_cases\":%u,\"success\":true}\n",aligned,negative);return 0;
}
