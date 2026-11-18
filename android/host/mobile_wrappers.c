#include "mobile_controls.h"
#include "win64_window.h"
#include "text_input.h"
#include "temperature_ui.h"
#include "../dxvk/bridge.h"
void L_000140D8DB10(CPU* c){win64_d3d11_save_photo(c);c->rip=pop64(c);}
void L_000140D8E470(CPU* c){
    CPU request=*c;request.rsp=(request.rsp-512)&~UINT64_C(15);
    request.rcx=rd64(c->rdx+0x28);
    push64(&request,UINT64_C(0xe5ee0010));
    dispatch(&request,rd64(rd64(request.rcx)+0x88)-g_image_delta);
    request.rcx=rd64(GVA(0x14400F178));request.rdx=request.rax;request.r9=c->r8;
    win64_d3d11_save_photo(&request);c->rax=request.rax;c->rip=pop64(c);
}
void fp_original_L_000141250610(CPU*);
void fp_original_L_00014124C7B0(CPU*);
void fp_original_L_000140EC6200(CPU*);
void fp_original_L_000140EC7240(CPU*);
void fp_original_L_000140E74B30(CPU*);
void fp_original_L_000140F99F00(CPU*);
void fp_original_L_000140F9D240(CPU*);
void fp_original_L_000141A04EB0(CPU*);
void fp_original_L_000141AAAFB0(CPU*);
void fp_original_L_00014180A400(CPU*);
void fp_original_L_0001419EF190(CPU*);
void fp_original_L_000141A5EE10(CPU*);
void fp_original_L_000141A62E90(CPU*);
void fp_original_L_0001410CCA60(CPU*);
void L_0001410CCA60(CPU* c){fp_mobile_canvas_render(c,fp_original_L_0001410CCA60);}
void L_000141A5EE10(CPU* c){fp_mobile_law_camera_init(c,fp_original_L_000141A5EE10);}
void L_000141A62E90(CPU* c){fp_mobile_law_camera(c,fp_original_L_000141A62E90);}
void L_000141A04EB0(CPU* c){fp_mobile_painters_created(c,fp_original_L_000141A04EB0);}
void L_000141AAAFB0(CPU* c){fp_mobile_building_aim(c,fp_original_L_000141AAAFB0);}
void L_00014180A400(CPU* c){fp_mobile_show_feedback(c,fp_original_L_00014180A400);}
void L_0001419EF190(CPU* c){fp_mobile_panels_created(c,fp_original_L_0001419EF190);}
void L_000141250610(CPU* c){fp_mobile_camera_before(c);fp_original_L_000141250610(c);fp_mobile_camera_after(c);}
void L_00014124C7B0(CPU* c){fp_mobile_camera_destroy();fp_original_L_00014124C7B0(c);}
void L_000140EC6200(CPU* c){fp_mobile_ui_scale();fp_original_L_000140EC6200(c);}
void L_000140EC7240(CPU* c){fp_temperature_ui_before(c);fp_mobile_ui_before(c);fp_original_L_000140EC7240(c);fp_mobile_text_process(c);}

void L_000140E74B30(CPU* c){
    uint64_t game=c->rcx,epoch=win64_window_ui_begin();uint32_t frame=rd32(game+0x444);
    fp_original_L_000140E74B30(c);
    /* Loading/synchronization returns skip native UI input handling and its frame increment. */
    if(rd32(game+0x444)!=frame)win64_window_ui_end(epoch);
}
void L_000140F99F00(CPU* c){fp_mobile_ui_position(c,fp_original_L_000140F99F00);}
void L_000140F9D240(CPU* c){fp_mobile_ui_template(c,fp_original_L_000140F9D240);}
