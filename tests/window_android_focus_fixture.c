/* Reuse the existing injected-host harness; call the actual USER32 adapter. */
#define main unused_window_regression_main
#include "window_fixture.c"
#undef main

static void minimized(uint64_t hwnd,message *msg) {
    require(api("ShowWindow",2,hwnd,V(6))==1,"SDL minimizes its virtual fullscreen window");
    require(api("IsIconic",1,hwnd)==1&&!api("GetFocus",0),"minimized window has no guest focus");
    drain(msg);
}

static void restored(uint64_t hwnd,message *msg,int expect_key) {
    require(!api("IsIconic",1,hwnd)&&api("GetFocus",0)==hwnd,"Android foreground restores HWND and guest focus");
    require(!(api("GetWindowLongPtrW",2,hwnd,V(-16))&0x20000000u),"WS_MINIMIZE cleared");
    unsigned size=0,active=0,focus=0,key=0;
    while(peek(msg,0,0,0,1)) {
        if(msg->message==5){require(!msg->wp&&msg->lp==(960u|(540u<<16)),"restored client dimensions");size++;}
        if(msg->message==6&&msg->wp==1){require(size==1,"SIZE precedes ACTIVATE");active++;}
        if(msg->message==7){require(active==1,"ACTIVATE precedes SETFOCUS");focus++;}
        if(msg->message==0x100){require(focus==1&&msg->wp=='A',"first foreground keystroke delivered after restore");key++;}
    }
    require(size==1&&active==1&&focus==1&&key==(unsigned)expect_key,"one complete restore sequence");
}

int main(void) {
    uint16_t name[]={'T','o','u','c','h','F','o','c','u','s',0};
    win64_region_add((uintptr_t)name,sizeof(name),0);
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};
    win64_region_add((uintptr_t)&cls,sizeof(cls),1);
    uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));require(atom>=0xc000,"fixture class");
    win64_window_configure(&interface);host.available=1;
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    uint64_t hwnd=make_window(atom);require(hwnd!=0,"fixture native window");
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);drain(&msg);
    uint8_t keys[256];

    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=0});drain(&msg);minimized(hwnd,&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,keys)==0&&!keys[0x1e]&&api("IsIconic",1,hwnd),"background input neither restores nor latches keys");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    require(win64_window_input_snapshot(hwnd,keys)==1&&!keys[0x1e],"foreground does not resurrect background key");
    restored(hwnd,&msg,0);

    // SDL's queued focus-loss handler may minimize after Android focus has already returned.
    minimized(hwnd,&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,keys)==1&&keys[0x1e]==0x80,"late SDL minimize restored by first real input");
    restored(hwnd,&msg,1);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_A});drain(&msg);

    minimized(hwnd,&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.x=500,.y=400});
    require(win64_window_input_snapshot(hwnd,keys)==1,"first pointer restores a minimized foreground window");
    restored(hwnd,&msg,0);

    minimized(hwnd,&msg);++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A,.window_generation=host.generation-1});
    require(win64_window_input_snapshot(hwnd,keys)==0&&api("IsIconic",1,hwnd),"stale input cannot restore a new surface");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});drain(&msg);

    minimized(hwnd,&msg);host.available=0;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    require(win64_window_input_snapshot(hwnd,keys)==0&&api("IsIconic",1,hwnd),"missing native surface cannot restore");drain(&msg);
    host.available=1;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,keys)==1,"restored surface accepts first key");restored(hwnd,&msg,1);

    minimized(hwnd,&msg);require(api("ShowWindow",2,hwnd,V(0))==1,"hide virtual window");drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,keys)==0&&!api("IsWindowVisible",1,hwnd),"foreground never unhides an intentionally hidden HWND");
    drain(&msg);require(api("DestroyWindow",1,hwnd)==1,"fixture destroy");win64_window_configure(NULL);
    puts("{\"success\":true,\"scenarios\":7,\"actual_user32_adapter\":true,\"no_latched_background_keys\":true}");
    return 0;
}
