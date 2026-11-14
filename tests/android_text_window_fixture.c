static unsigned input_cancels,text_cancels;
void frostpunk_android_cancel_input(void){++input_cancels;}
void frostpunk_mobile_text_cancel(void){++text_cancels;}
#define main unused_window_main
#include "window_fixture.c"
#undef main
static unsigned render_calls,update_calls;
static uint32_t update_ticket;
static int skip_update,deliver_in_update;
static uint64_t update_hwnd;
static message *update_message;
void L_000140EC7240(CPU *cpu);
static void fp_mobile_ui_before(CPU *cpu){(void)cpu;}
static void fp_mobile_text_process(CPU *cpu){(void)cpu;}
static void fp_original_L_000140EC7240(CPU *cpu){(void)cpu;++render_calls;}
static void fp_original_L_000140E74B30(CPU *cpu){
    ++update_calls;
    require(!frostpunk_android_touch_ready(update_ticket),"Update cannot acknowledge before original input handling");
    if(skip_update)return;
    if(deliver_in_update){deliver_in_update=0;require(peek(update_message,update_hwnd,0x200,0x200,1),"pointer delivered inside native Update");}
    wr32(cpu->rcx+0x444,rd32(cpu->rcx+0x444)+1);
    for(unsigned screen=0;screen<3;++screen)L_000140EC7240(cpu);
    require(!frostpunk_android_touch_ready(update_ticket),"multiple screen renders cannot acknowledge unfinished native Update");
    cpu->rax=0x12345678;cpu->rcx=0x87654321;cpu->rsp+=8;
}
#include "touch_update_wrappers.inc"
int main(void){
    uint16_t name[]={'I','M','E',0};win64_region_add((uintptr_t)name,sizeof(name),0);
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};
    win64_region_add((uintptr_t)&cls,sizeof(cls),1);uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));
    win64_window_configure(&interface);host.available=1;event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    uint64_t hwnd=make_window(atom);require(hwnd!=0,"text fixture window");
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);drain(&msg);
    uint32_t units[]={0x416,0x65e5,0xd83d,0xde00};
    for(unsigned i=0;i<4;++i){event((FrostpunkAndroidEvent){.type=FP_ANDROID_TEXT,.keycode=units[i]});
        require(peek(&msg,hwnd,0,0,1)&&msg.message==0x102&&msg.wp==units[i]&&msg.lp==1,"IME preserves ordered UTF16 WM_CHAR units");}
    _Alignas(16) unsigned char game[0x448]={0};CPU update_cpu;memset(&update_cpu,0xa5,sizeof(update_cpu));update_cpu.rcx=(uintptr_t)game;update_cpu.rsp=0x1000;CPU expected=update_cpu;
    update_ticket=31;
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.repeat=update_ticket,.x=120,.y=120});
    require(peek(&msg,hwnd,0x200,0x200,1),"Update-boundary hover delivered");
    for(unsigned screen=0;screen<3;++screen)L_000140EC7240(&update_cpu);
    require(!frostpunk_android_touch_ready(update_ticket),"screen rendering alone never acknowledges a pointer ticket");
    skip_update=1;L_000140E74B30(&update_cpu);
    require(!frostpunk_android_touch_ready(update_ticket)&&!memcmp(&update_cpu,&expected,sizeof(expected)),"skipped native Update cannot acknowledge input or change CPU");
    skip_update=0;L_000140E74B30(&update_cpu);expected.rax=0x12345678;expected.rcx=0x87654321;expected.rsp+=8;
    require(frostpunk_android_touch_ready(update_ticket)&&render_calls==6&&update_calls==2,"completed native Update acknowledges after all screen handling");
    require(!memcmp(&update_cpu,&expected,sizeof(expected)),"Update wrapper retains all original CPU results");
    update_ticket=33;update_hwnd=hwnd;update_message=&msg;deliver_in_update=1;update_cpu.rcx=(uintptr_t)game;
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.repeat=update_ticket,.x=120,.y=120});
    L_000140E74B30(&update_cpu);require(!frostpunk_android_touch_ready(update_ticket),"delivery inside Update requires a later complete Update");
    update_cpu.rcx=(uintptr_t)game;L_000140E74B30(&update_cpu);require(frostpunk_android_touch_ready(update_ticket),"later complete Update acknowledges delivery during prior Update");
    for(unsigned button=0;button<2;++button){
        unsigned down=button?0x204:0x201,up=button?0x205:0x202,mask=button?2:1;
        uint64_t old=win64_window_ui_begin();uint32_t ticket=41+button;
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=mask,.repeat=ticket,.x=120,.y=120});
        require(!frostpunk_android_touch_ready(ticket),"enqueue is not a delivery acknowledgement");
        require(peek(&msg,hwnd,down,down,0)&&!frostpunk_android_touch_ready(ticket),"PM_NOREMOVE is not a delivery acknowledgement");
        require(peek(&msg,hwnd,0x200,0x200,1),"Down companion move delivered first");
        uint64_t move_pass=win64_window_ui_begin();win64_window_ui_end(move_pass);
        require(!frostpunk_android_touch_ready(ticket),"companion move cannot acknowledge unremoved Down");
        require(peek(&msg,hwnd,down,down,1),"Down delivered");
        win64_window_ui_end(old);require(!frostpunk_android_touch_ready(ticket),"pass begun before delivery cannot acknowledge Down");
        uint64_t next=win64_window_ui_begin();win64_window_ui_end(next);require(frostpunk_android_touch_ready(ticket),"later original UI pass acknowledges exact Down ticket");
        drain(&msg);old=win64_window_ui_begin();ticket=61+button;
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=mask,.repeat=ticket,.x=140,.y=140});
        require(!frostpunk_android_touch_ready(ticket),"held move enqueue is not acknowledgement");
        require(peek(&msg,hwnd,0x200,0x200,1),"held move delivered");win64_window_ui_end(old);
        require(!frostpunk_android_touch_ready(ticket),"held move requires a later UI pass");
        next=win64_window_ui_begin();win64_window_ui_end(next);require(frostpunk_android_touch_ready(ticket),"held left/right drag move acknowledged");
        ticket=81+button;
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.repeat=ticket,.x=120,.y=120});
        require(peek(&msg,hwnd,0x200,0x200,1),"Up companion move delivered first");
        move_pass=win64_window_ui_begin();win64_window_ui_end(move_pass);
        require(!frostpunk_android_touch_ready(ticket),"companion move cannot acknowledge unremoved Up");
        require(peek(&msg,hwnd,up,up,1),"Up delivered");
        next=win64_window_ui_begin();win64_window_ui_end(next);require(frostpunk_android_touch_ready(ticket),"later original UI pass acknowledges exact Up ticket");drain(&msg);
    }
    uint64_t old=win64_window_ui_begin();
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.repeat=51,.x=120,.y=120});
    require(peek(&msg,hwnd,0x200,0x200,1),"hover delivered");win64_window_ui_end(old);
    require(!frostpunk_android_touch_ready(51),"hover requires a subsequent original UI pass");
    uint64_t next=win64_window_ui_begin();win64_window_ui_end(next);require(frostpunk_android_touch_ready(51),"original hover acknowledgement");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=1,.x=120,.y=120});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.x=120,.y=120});
    usleep(250000);
    require(peek(&msg,hwnd,0x201,0x202,1)&&msg.message==0x201,"short tap Down retained for five-FPS consumer");
    require(peek(&msg,hwnd,0x201,0x202,1)&&msg.message==0x202,"short tap Up retains its order");drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(peek(&msg,hwnd,0x100,0x100,1),"pre-Surface key delivered");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=1,.repeat=71,.x=120,.y=120});
    require(peek(&msg,hwnd,0x201,0x201,1),"pre-Surface Down delivered");
    next=win64_window_ui_begin();win64_window_ui_end(next);require(frostpunk_android_touch_ready(71),"pre-Surface ack ready");
    require(api("PostMessageW",4,hwnd,V(0x8051),V(9),V(10)),"post retained across Surface generation");
    unsigned old_generation=(unsigned)host.generation;++host.generation;
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});
    require(peek(&msg,hwnd,0x8051,0x8051,1)&&msg.wp==9,"Surface cancel retains non-input posted messages");
    require(!api("GetAsyncKeyState",1,V('A'))&&!api("GetAsyncKeyState",1,V(1)),"Surface clears async pressed and held keys");
    require(!api("GetKeyState",1,V('A'))&&!api("GetKeyState",1,V(1)),"Surface clears delivered key state");
    require(!frostpunk_android_touch_ready(71)&&input_cancels==1&&text_cancels==1,"Surface invalidates tickets and cancels Java/editor queue");
    require(!peek(&msg,hwnd,0x200,0x20e,1),"Surface cancellation does not commit a synthetic release");
    require(peek(&msg,hwnd,8,8,1),"old Surface delivers focus loss");
    api("ShowWindow",2,hwnd,V(6)); /* SDL may minimize while handling the old loss. */
    require(api("IsIconic",1,hwnd),"simulated late SDL minimize");
    require(peek(&msg,hwnd,7,7,1),"new Surface focus is delivered without an extra tap");
    require(api("GetFocus",0)==hwnd&&!api("IsIconic",1,hwnd),"new Surface focus restores virtual fullscreen window");
    while(peek(&msg,hwnd,0,0,1))require(msg.message!=8&&!(msg.message==6&&msg.wp==0)&&!(msg.message==5&&msg.wp==1),"late SDL loss/minimize messages discarded after restore");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.window_generation=old_generation,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.x=120,.y=120});
    require(!peek(&msg,hwnd,0x200,0x20e,1),"old Surface input is discarded");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.x=120,.y=120});
    require(peek(&msg,hwnd,0x200,0x200,1)&&!peek(&msg,hwnd,0x202,0x202,1),"new hover cannot release the old press");
    require(api("DestroyWindow",1,hwnd)==1,"text fixture destroy");win64_window_configure(NULL);
    puts("{\"success\":true,\"unicode\":true,\"hover_ack\":true,\"down_delivery_ack\":true,\"held_move_ack\":true,\"later_ui_pass\":true,\"game_update_boundary\":true,\"render_never_acknowledges\":true,\"skipped_update_never_acknowledges\":true,\"original_update_cpu_results\":true,\"slow_consumer\":true,\"surface_cancel\":true,\"surface_focus_restore\":true}");
}
