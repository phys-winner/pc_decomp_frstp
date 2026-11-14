// Include after mobile_law_layout.inc. Call for EXT_SELECTION_PANEL after choosing its w/h.
// The three backplate layers have native anchor-driven nine-slice children.
typedef struct {uint64_t root,controller,background;uint32_t id,background_id;unsigned seen,closing,hidden;} selection_close_state;
static selection_close_state selection_closes[16];
static unsigned selection_close_count;

static unsigned selection_background(CPU *cpu,uint64_t root,float width,float height) {
    const char *root_name=law_photo_name(root);
    if(!root_name||strcmp(root_name,"EXT_SELECTION_PANEL"))return 0;
    mobile_layout *root_base=remember_layout(root);if(!root_base)return 0;
    selection_close_state *state=NULL;
    uint64_t controller=panels?rd64(panels+0x240):0,reference=controller?rd64(controller+0xa8):0;
    if(controller&&rd64(controller)==GVA(0x14212E5D8)&&reference&&rd64(reference)==root){
        uint32_t id=rd32(root+0x1e4);unsigned i=0;
        for(;i<selection_close_count;++i)if(selection_closes[i].root==root&&selection_closes[i].id==id&&selection_closes[i].controller==controller)break;
        if(i==selection_close_count&&i<16)selection_closes[selection_close_count++]=(selection_close_state){.root=root,.controller=controller,.id=id};
        if(i<16){
            state=&selection_closes[i];
            if(rd64(controller+0xd0)){state->seen=1;state->closing=0;}
            // Native close clears ActiveSubPanel/Idx before the root's hide animation finishes.
            else if(state->seen&&rd32(controller+0xd8)==UINT32_MAX)state->closing=applied_scale>100;
            if(applied_scale<=100)state->closing=0;
        }
    }
    unsigned count=0;
    for(uint64_t animation=rd64(root+0x220);animation&&count++<128;animation=rd64(animation+0x240)){
        const char *name=law_photo_name(animation);if(!name||strcmp(name,"ANI"))continue;
        unsigned children=0;
        for(uint64_t backplate=rd64(animation+0x220);backplate&&children++<128;backplate=rd64(backplate+0x240)){
            name=law_photo_name(backplate);if(!name||strcmp(name,"Background"))continue;
            if(state){
                uint32_t id=rd32(backplate+0x1e4);
                if(state->hidden&&(state->background!=backplate||state->background_id!=id))state->hidden=0;
                if(state->closing){
                    if(!(rd32(backplate+0x1e0)&2u)){
                        call_value(cpu,UINT64_C(0x140EC0F60),backplate,2);
                        state->background=backplate;state->background_id=id;state->hidden=1;
                    }
                    continue;
                }
                if(state->hidden){call_value(cpu,UINT64_C(0x140EBE470),backplate,2);state->hidden=0;}
            }
            mobile_layout *base=remember_layout(backplate);if(!base)continue;
            float w=base->w,h=base->h;
            if(applied_scale>100){
                w+=width-root_base->w;h+=height-root_base->h;
                // The PC controller moves this frame below some portraits; both mobile columns need its full height.
                if(law_photo_remember(backplate))layout_position(cpu,backplate,base->x,62);
            }else law_photo_restore(cpu,backplate);
            // Native SetSize propagates the size delta through blur/color/stroke anchors.
            // Leave slice dimensions/UVs and the original backplate margins untouched.
            {static float logged_w,logged_h;
             if(fabsf(w-logged_w)>.5f||fabsf(h-logged_h)>.5f){logged_w=w;logged_h=h;
                fprintf(stderr,"FP_SELECTION_BACKPLATE size=%.1fx%.1f base=%.1fx%.1f panel=%.1fx%.1f root_base=%.1fx%.1f actual=%.1fx%.1f\n",
                    w,h,base->w,base->h,width,height,root_base->w,root_base->h,rdss(backplate+0x100),rdss(backplate+0x104));}}
            // Switching to another building type resets the live box to the PC column (360x538) without
            // touching the requested size, so compare both before trusting the cached request.
            if(w>0&&h>0){
                if(applied_scale>100&&(fabsf(rdss(backplate+0x100)-w)>.5f||fabsf(rdss(backplate+0x104)-h)>.5f)){
                    float size[4]={w,h,0,0};call(cpu,UINT64_C(0x140EC4C70),backplate,size,0,0,0);
                } else law_photo_size(cpu,backplate,w,h);
            }
        }
    }
    return state&&state->closing;
}
