#include "text_input.h"
#include "win64.h"
#include "win64_window.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { INSERT=0,DELETE_UNITS=1,DELETE_POINTS=2,EDITOR_KEY=3,TEXT_QUEUE=16 };
typedef struct {uint64_t editor;uint32_t id,action,before,after,units;uint16_t text[257];} text_command;
static pthread_mutex_t text_mutex=PTHREAD_MUTEX_INITIALIZER;
static text_command commands[TEXT_QUEUE];
static unsigned command_count;
static uint64_t active_editor,text_epoch;
static uint32_t active_id;
static float ime_visible_fraction=1;

static int valid_utf16(const uint16_t *text,unsigned units){
    if(!text&&units)return 0;
    for(unsigned i=0;i<units;++i){
        unsigned c=text[i];if(!c)return 0;
        if(c>=0xd800&&c<=0xdbff){if(++i==units||text[i]<0xdc00||text[i]>0xdfff)return 0;}
        else if(c>=0xdc00&&c<=0xdfff)return 0;
    }
    return 1;
}
__attribute__((visibility("default"))) int frostpunk_mobile_text_submit(unsigned action,const uint16_t *text,unsigned units,unsigned before,unsigned after){
    if(action>EDITOR_KEY||units>256||before>256||(action!=EDITOR_KEY&&after>256)||(action==INSERT&&!valid_utf16(text,units)))return 0;
    if(action==EDITOR_KEY){unsigned scan=before==13?0x1c:before==0x25?0xe04b:before==0x27?0xe04d:before==0x24?0xe047:before==0x23?0xe04f:0;
        if(!scan||after!=scan||units)return 0;}
    if(action==INSERT&&!units)return 1;
    pthread_mutex_lock(&text_mutex);int accepted=active_editor&&command_count<TEXT_QUEUE;
    if(accepted){text_command *c=&commands[command_count++];*c=(text_command){.editor=active_editor,.id=active_id,.action=action,.before=before,.after=after,.units=units};
        if(action==INSERT)memcpy(c->text,text,units*sizeof(uint16_t));}
    pthread_mutex_unlock(&text_mutex);return accepted;
}
__attribute__((visibility("default"))) void frostpunk_mobile_text_cancel(void){
    pthread_mutex_lock(&text_mutex);active_editor=0;active_id=0;command_count=0;ime_visible_fraction=1;++text_epoch;pthread_mutex_unlock(&text_mutex);
}
__attribute__((visibility("default"))) void frostpunk_mobile_ime_viewport(float fraction){
    if(!isfinite(fraction)||fraction<0||fraction>1)return;
    pthread_mutex_lock(&text_mutex);ime_visible_fraction=fraction;pthread_mutex_unlock(&text_mutex);
}
float fp_mobile_ime_visible_fraction(void){
    pthread_mutex_lock(&text_mutex);float fraction=ime_visible_fraction;pthread_mutex_unlock(&text_mutex);return fraction;
}
__attribute__((visibility("default"))) int frostpunk_mobile_text_editing(void){
    pthread_mutex_lock(&text_mutex);int editing=active_editor!=0;pthread_mutex_unlock(&text_mutex);return editing;
}
static uint64_t focused_editor(void){
    uint64_t list=rd64(GVA(0x142A89088));uint32_t count=rd32(GVA(0x142A89090));uint64_t result=0;
    if(!list||count>20000)return 0;
    for(uint32_t i=0;i<count;++i){uint64_t node=rd64(list+(uint64_t)i*16);
        if(!node||rd64(node)!=GVA(0x141DAC248)||!rd8(node+0x6c8))continue;
        uint64_t parent=node;unsigned depth=0;
        for(;parent&&depth<40;++depth,parent=rd64(parent+0x230))if(rd32(parent+0x1e0)&2)break;
        if(parent)continue;
        if(result&&result!=node)return 0;result=node;
    }
    return result;
}
uint64_t fp_mobile_text_focused_editor(void){return focused_editor();}
static void editor_call(CPU *cpu,uint64_t function,uint64_t editor,uint64_t a,uint64_t b){
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=editor;c.rdx=a;c.r8=b;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,function);
}
static int pair_boundary(const uint16_t *text,unsigned length,unsigned at){
    return at<=length&&(!at||at==length||text[at-1]<0xd800||text[at-1]>0xdbff||text[at]<0xdc00||text[at]>0xdfff);
}
static int insert_text(CPU *cpu,uint64_t editor,text_command *command){
    uint64_t buffer=rd64(editor+0x678);uint32_t length=rd32(editor+0x680),maximum=rd32(editor+0x684),cursor=rd32(editor+0x688);
    uint32_t start=rd32(editor+0x690),selected=rd8(editor+0x6c9)?rd32(editor+0x694):0;
    if(!buffer||length>maximum||maximum>65536||cursor>length||(selected&&(start>length||selected>length-start)))return 0;
    const uint16_t *text=(const uint16_t*)(uintptr_t)buffer;
    if(!pair_boundary(text,length,cursor)||(selected&&(!pair_boundary(text,length,start)||!pair_boundary(text,length,start+selected))))return 0;
    uint32_t units=command->units,space=maximum-(length-selected);
    if(units>space)units=space;
    if(units&&command->text[units-1]>=0xd800&&command->text[units-1]<=0xdbff)--units;
    if(!units)return 1;
    command->text[units]=0;
    win64_region_add((uintptr_t)command->text,(units+1)*sizeof(uint16_t),0);
    editor_call(cpu,UINT64_C(0x140FC30A0),editor,(uintptr_t)command->text,0);
    win64_region_remove((uintptr_t)command->text);return 1;
}
static int point_range(const uint16_t *text,unsigned length,unsigned start,unsigned end,unsigned before,unsigned after,unsigned *left,unsigned *right){
    unsigned a=start,b=end;
    while(before--&&a){unsigned c=text[--a];
        if(c>=0xdc00&&c<=0xdfff){if(!a||text[a-1]<0xd800||text[a-1]>0xdbff)return 0;--a;}
        else if(c>=0xd800&&c<=0xdbff)return 0;
    }
    while(after--&&b<length){unsigned c=text[b++];
        if(c>=0xd800&&c<=0xdbff){if(b==length||text[b]<0xdc00||text[b]>0xdfff)return 0;++b;}
        else if(c>=0xdc00&&c<=0xdfff)return 0;
    }
    *left=start-a;*right=b-end;return 1;
}
static int delete_text(CPU *cpu,uint64_t editor,const text_command *command){
    uint64_t buffer=rd64(editor+0x678);uint32_t length=rd32(editor+0x680),cursor=rd32(editor+0x688);
    uint32_t start=cursor,selected=rd8(editor+0x6c9)?rd32(editor+0x694):0;
    if(selected)start=rd32(editor+0x690);
    if(!buffer||length>65536||cursor>length||start>length||selected>length-start||(selected&&(cursor<start||cursor>start+selected)))return 0;
    uint32_t end=start+selected,left=command->before<start?command->before:start,right=command->after<length-end?command->after:length-end;
    if(command->action==DELETE_POINTS&&!point_range((const uint16_t*)(uintptr_t)buffer,length,start,end,command->before,command->after,&left,&right))return 0;
    editor_call(cpu,UINT64_C(0x140FC2AA0),editor,0,0);
    if(rd8(editor+0x6c9)){
        if(right){editor_call(cpu,UINT64_C(0x140FC3C70),editor,end,right);editor_call(cpu,UINT64_C(0x140FC2AA0),editor,1,0);}
        if(left){editor_call(cpu,UINT64_C(0x140FC3C70),editor,start-left,left);editor_call(cpu,UINT64_C(0x140FC2AA0),editor,1,0);}
    }else{
        editor_call(cpu,UINT64_C(0x140FC3A50),editor,end,0);
        for(uint32_t i=0;i<right;++i)editor_call(cpu,UINT64_C(0x140FC2E30),editor,0,0);
        editor_call(cpu,UINT64_C(0x140FC3A50),editor,start,0);
        for(uint32_t i=0;i<left;++i)editor_call(cpu,UINT64_C(0x140FC29C0),editor,0,0);
    }
    editor_call(cpu,UINT64_C(0x140FC3A50),editor,cursor-left,0);
    if(selected)editor_call(cpu,UINT64_C(0x140FC3C70),editor,start-left,selected);
    return 1;
}
void fp_mobile_text_process(CPU *cpu){
    uint64_t editor=focused_editor();uint32_t id=editor?rd32(editor+0x1e4):0;
    text_command queued[TEXT_QUEUE];unsigned count;uint64_t epoch;
    pthread_mutex_lock(&text_mutex);active_editor=editor;active_id=id;count=command_count;epoch=text_epoch;
    memcpy(queued,commands,count*sizeof(*queued));command_count=0;pthread_mutex_unlock(&text_mutex);
    for(unsigned i=0;i<count;++i){
        pthread_mutex_lock(&text_mutex);int cancelled=epoch!=text_epoch;pthread_mutex_unlock(&text_mutex);
        text_command *c=&queued[i];if(cancelled||!editor||c->editor!=editor||c->id!=id)continue;
        if(c->action==INSERT){if(!insert_text(cpu,editor,c))fprintf(stderr,"FP_IME_INSERT_REJECTED invalid_native_utf16_or_cursor=1\n");}
        else if(c->action==EDITOR_KEY)win64_window_ime_key(c->before,c->after);
        else if(!delete_text(cpu,editor,c))fprintf(stderr,"FP_IME_DELETE_REJECTED invalid_native_utf16_or_cursor=1\n");
        if(focused_editor()!=editor||rd32(editor+0x1e4)!=id)break;
    }
}
