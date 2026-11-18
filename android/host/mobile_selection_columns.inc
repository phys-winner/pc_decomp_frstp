/* UILayout task82 (141097460) owns direct-child flow. Keep that flow for the
 * right column; its native ignore-layout bit removes only the left header.
 * The geometry and shrink contract are exercised with the original bodies. */
typedef struct {
    uint64_t node,header;uint32_t id,header_id,header_ignore;
    uint32_t header_anchors,header_dock;
    float padding[4],requested[4],header_x,header_y;unsigned char wrap,shrink,mobile;
} mobile_selection_flow;
static mobile_selection_flow selection_flows[512];
static unsigned selection_flow_count;
typedef struct {
    uint64_t node,parent;uint32_t id,parent_id,anchors,dock;float width;
} mobile_selection_child;
static mobile_selection_child selection_flow_children[2048];
static unsigned selection_flow_child_count;
static mobile_selection_child *selection_flow_child_base(uint64_t node,uint64_t parent){
    uint32_t id=rd32(node+0x1e4),parent_id=rd32(parent+0x1e4);
    for(unsigned i=0;i<selection_flow_child_count;++i)
        if(selection_flow_children[i].node==node&&selection_flow_children[i].id==id&&selection_flow_children[i].parent==parent&&selection_flow_children[i].parent_id==parent_id)return &selection_flow_children[i];
    if(selection_flow_child_count==2048){static unsigned once;if(!once++)fprintf(stderr,"FP_UI_REGISTRY_FULL selection_children=2048\n");return NULL;}
    mobile_selection_child *base=&selection_flow_children[selection_flow_child_count++];
    *base=(mobile_selection_child){node,parent,id,parent_id,rd32(node+0x13c),rd32(node+0x138),rdss(node+0x100)};return base;
}
static mobile_selection_flow *selection_flow_base(uint64_t node,uint64_t header){
    uint32_t id=rd32(node+0x1e4),header_id=rd32(header+0x1e4);
    for(unsigned i=0;i<selection_flow_count;++i)
        if(selection_flows[i].node==node&&selection_flows[i].id==id&&selection_flows[i].header==header&&selection_flows[i].header_id==header_id)return &selection_flows[i];
    if(selection_flow_count==512){static unsigned once;if(!once++)fprintf(stderr,"FP_UI_REGISTRY_FULL selection_flows=512\n");return NULL;}
    mobile_selection_flow *base=&selection_flows[selection_flow_count++];
    *base=(mobile_selection_flow){.node=node,.header=header,.id=id,.header_id=header_id,
        .header_ignore=rd32(header+0x1e0)&4u,.wrap=rd8(node+0x2a8),.shrink=rd8(node+0x2a9)};
    base->header_anchors=rd32(header+0x13c);base->header_dock=rd32(header+0x138);
    base->header_x=rdss(header+0xc0);base->header_y=rdss(header+0xc4);
    memcpy(base->padding,(void*)(uintptr_t)(node+0x280),16);memcpy(base->requested,(void*)(uintptr_t)(node+0x210),16);return base;
}
static uint64_t selection_named_child(uint64_t node,const char *wanted){
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<64;child=rd64(child+0x240)){
        const char *name=(const char*)(uintptr_t)rd64(child+0x1d8);
        if(name&&!strcmp(name,wanted))return child;
    }
    return 0;
}
/* Workplaces: the Workers page (count line, a fixed 154-unit table and buttons at 174) is taller than
 * most first pages, so opening it grew the frame over the bottom HUD and shrank it back on the other
 * tab.  Every tab keeps the Workers height, like buildings whose first page is as tall (Advanced
 * Steelworks).  The page's own rows give its height even while it is hidden. */
static float selection_workers_edge(uint64_t child){
    const char *name=(const char*)(uintptr_t)rd64(child+0x1d8);
    if(!name||strcmp(name,"TAB_CONTROLLER"))return 0;
    uint64_t content=selection_named_child(child,"CONTENT"),page=content?selection_named_child(content,"EXT_TAB_WORKER_DETAILS"):0;
    uint64_t details=page?selection_named_child(page,"EXT_WORKERS_DETAILS"):0;
    if(!details)return 0;
    float extent=0;unsigned count=0;
    for(uint64_t item=rd64(details+0x220);item&&count++<32;item=rd64(item+0x240)){
        float s=rdss(item+0xf4),bottom=rdss(item+0xc4)+rdss(item+0xd4)*s+rdss(item+0x104)*s;
        if(isfinite(bottom)&&bottom<1000)extent=fmaxf(extent,bottom);
    }
    float scale=rdss(child+0xf4),inner=rdss(page+0xf4)*rdss(details+0xf4);
    if(!(extent>0)||!(scale>0)||!(inner>0))return 0;
    return rdss(child+0xc4)+rdss(child+0xd4)*scale+(rdss(content+0xc4)+rdss(page+0xc4)+rdss(details+0xc4)+extent*inner)*scale;
}
static unsigned selection_columns(CPU *cpu,uint64_t node,unsigned depth,float *height,unsigned apply){
    if(depth>5||(rd32(node+0x1e0)&2))return 0;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);unsigned found=0;
    if(name&&strstr(name,"SUBPANEL")&&rd64(node)==GVA(0x141DE0A48)&&rd32(node+0x298)==0&&rd32(node+0x29c)==0){
        uint64_t header=0;unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
            const char *label=(const char*)(uintptr_t)rd64(child+0x1d8);
            if(label&&!strcmp(label,"EXT_BUILDING_HEADER"))header=child;
        }
        if(header){
            mobile_selection_flow *base=selection_flow_base(node,header);if(!base)return 2;
            mobile_selection_child *children[128];unsigned child_count=0;
            count=0;
            for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
                if(child==header||(rd32(child+0x1e0)&4u))continue;
                mobile_selection_child *item=selection_flow_child_base(child,node);if(!item)return 2;
                children[child_count++]=item;
            }
            unsigned mobile=applied_scale>100;float padding[4];memcpy(padding,base->padding,16);
            // Tabs extend above their pane; reserve their full touch row inside the backplate.
            if(mobile){padding[0]=342; padding[2]=36;
                {static float logged=-1;if(fabsf(base->padding[3]-logged)>.01f){logged=base->padding[3];fprintf(stderr,"FP_SELECTION_PADDING bottom=%.1f\n",logged);}}
                padding[3]=fminf(padding[3],12);}  /* the PC column's empty foot would cost the phone page a fifth of its room */
            float body_height=padding[2],drawn=0;unsigned body_count=0,budget=3000;
            count=0;
            for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
                unsigned flags=rd32(child+0x1e0);
                if(child==header)flags=(flags&~4u)|(mobile?4u:base->header_ignore);
                if((flags&6)||(rd8(child+0x34)&2)||(rd8(node+0x2aa)&&(rd32(child+0x30)&2)))continue;
                if(body_count++)body_height+=rdss(node+0x294);
                float b=rdss(child+0xf4);body_height+=rdss(child+0x104)*b;
                // Pages draw past their measured box (the generator's power column) or leave stretched
                // containers empty (steam distribution); the frame follows what is actually drawn.
                float edge=rdss(child+0xc4)+rdss(child+0xd4)*b+visual_extent(child,0,&budget)*b;
                if(isfinite(b)&&b>0&&isfinite(edge)&&edge<2000)drawn=fmaxf(drawn,edge);
                if(mobile){float workers=selection_workers_edge(child);if(isfinite(workers)&&workers<2000)drawn=fmaxf(drawn,workers);}
            }
            if(mobile&&drawn>0)body_height=drawn;
            selection_pad_bottom=padding[3];
            *height=fmaxf(*height,fmaxf(mobile?rdss(header+0x104)*rdss(header+0xf4):0,body_height+padding[3]));
            if(apply){
                unsigned ignore=mobile?4u:base->header_ignore,changed=0;
                if(!mobile&&base->mobile)call(cpu,UINT64_C(0x140EC4C70),node,base->requested,0,0,0);
                for(unsigned i=0;i<child_count;++i){
                    mobile_selection_child *item=children[i];uint64_t child=item->node;
                    unsigned anchors=mobile?0u:item->anchors,dock=mobile?0u:item->dock;
                    if(rd32(child+0x13c)!=anchors)call_value(cpu,UINT64_C(0x140EC30A0),child,anchors);
                    if(rd32(child+0x138)!=dock)call_value(cpu,UINT64_C(0x140EC39C0),child,dock);
                    /* Stretch anchors otherwise feed the padded parent width back into its next measure. */
                    if(mobile&&(item->anchors&6u)==6u&&!rd32(child+0x13c)&&fabsf(rdss(child+0x100)-item->width)>.01f){
                        float size[4]={item->width,rdss(child+0x104),rdss(child+0x108),rdss(child+0x10c)};
                        call(cpu,UINT64_C(0x140F99FB0),child,size,0,0,0);changed=1;
                    }
                }
                unsigned anchors=mobile?0u:base->header_anchors,dock=mobile?0u:base->header_dock;
                if(rd32(header+0x13c)!=anchors)call_value(cpu,UINT64_C(0x140EC30A0),header,anchors);
                if(rd32(header+0x138)!=dock)call_value(cpu,UINT64_C(0x140EC39C0),header,dock);
                layout_position(cpu,header,mobile?0:base->header_x,mobile?0:base->header_y);
                if((rd32(header+0x1e0)&4u)!=ignore){call_value(cpu,ignore?UINT64_C(0x140EC0F60):UINT64_C(0x140EBE470),header,4);changed=1;}
                for(unsigned i=0;i<4;++i)if(fabsf(rdss(node+0x280+i*4)-padding[i])>.01f){wrss(node+0x280+i*4,padding[i]);changed=1;}
                unsigned wrap=mobile?0u:base->wrap,shrink=mobile?1u:base->shrink;
                if(rd8(node+0x2a8)!=wrap){wr8(node+0x2a8,(uint8_t)wrap);changed=1;}
                if(rd8(node+0x2a9)!=shrink){wr8(node+0x2a9,(uint8_t)shrink);changed=1;}
                if(changed)call_value(cpu,UINT64_C(0x141096AE0),node,0);
                base->mobile=(unsigned char)mobile;
            }
            return 1;
        }
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240))found|=selection_columns(cpu,child,depth+1,height,apply);
    return found;
}
