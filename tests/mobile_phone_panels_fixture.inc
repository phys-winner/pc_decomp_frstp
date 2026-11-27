static void phone_panel_template_return(CPU *cpu){cpu->rsp+=8;}

static void check_mobile_phone_panels(CPU *cpu){
    unsigned relayout_before=layout_relayout_calls;
    static _Alignas(16) unsigned char storage[22][0x600],rows[2][96],cells[2][80];
    uint64_t n[22];for(unsigned i=0;i<22;++i)n[i]=(uintptr_t)storage[i];
    const char *names[]={"","EXT_SEARCH_CITIZENS_PANEL","ANI","Background","TABLE_PARENT","RESULTS_TABLE","EXT_ECONOMY_PANEL","body_text","","EXT_SELECTION_PANEL","TAB_CONTROLLER","CONTENT","EXT_TAB_AMBULATORIUM","EXT_RESIDENTS_DETAILS","TABLE","SCROLL_BAR","BAR","PLACE_ROW_ELEMENT","row_background","row_underscore","row_columns_layout","CONTENT_LAYOUT"};
    const unsigned parents[]={0,0,1,2,2,4,0,6,5,0,9,10,11,12,13,13,15,21,17,17,17,6};
    for(unsigned i=0;i<22;++i){
        wr64(n[i],GVA(i==5||i==14?0x141DE15E0:i==7?0x141DED7D8:i==10?0x141EDF7C8:i==11||i==21?0x141DE0A48:i==12?0x141EDFC48:i==13?0x141EE5D30:0x141D78A60));wr64(n[i]+0x1d8,(uintptr_t)names[i]);wr32(n[i]+0x1e4,27000+i);
        wrss(n[i]+0xf0,1);wrss(n[i]+0xf4,1);
        float w=i==6?1280:i==3?360:i==5?311:342,h=i==6?720:i==3?538:i==5?513:i==4?480:596;
        wrss(n[i]+0x100,w);wrss(n[i]+0x104,h);wrss(n[i]+0x210,w);wrss(n[i]+0x214,h);
        if(i){uint64_t parent=n[parents[i]],child=rd64(parent+0x220);wr64(n[i]+0x230,parent);
            if(!child)wr64(parent+0x220,n[i]);else{while(rd64(child+0x240))child=rd64(child+0x240);wr64(child+0x240,n[i]);}}
    }
    wrss(n[7]+0x3a0,10);wrss(n[7]+0x37c,120);
    wr64(n[15],GVA(0x141EDB0F8));wrss(n[15]+0x320,140);
    wr64(n[15]+0x2e0,(uintptr_t)&n[8]);wr64(n[15]+0x318,(uintptr_t)&n[16]);wrss(n[8]+0x104,121);wrss(n[16]+0x100,140);
    unsigned scroll_before=scrollbar_refresh_calls;
    uint64_t row_list[]={(uintptr_t)rows[0],(uintptr_t)rows[1]},cell_list[]={(uintptr_t)cells[0],(uintptr_t)cells[1]};
    wr64(n[5]+0x2f0,(uintptr_t)row_list);wr32(n[5]+0x2f8,2);wrss(n[5]+0x460,5);wrss(n[5]+0x464,500);
    for(unsigned i=0;i<2;++i){uint64_t row=row_list[i];wr64(row+24,n[5]);wrss(row+52,i?40:12);wr64(row+72,(uintptr_t)&cell_list[i]);wr32(row+80,1);wrss(cell_list[i]+52,i?40:12);}
    wrss(n[17]+0x104,41);wrss(n[17]+0x214,41);wrss(n[18]+0x104,41);wrss(n[18]+0x214,41);
    wrss(n[19]+0xc4,38);wrss(n[20]+0xc4,0);
    uint64_t saved=cpu->rcx;cpu->rcx=n[0];scale=200;unsigned calls=table_row_calls;
    fp_mobile_ui_before(cpu);
    assert(rdss(n[1]+0x104)==240&&rdss(n[1]+0xf0)==1);
    assert(rdss(n[4]+0x104)==180&&rdss(n[5]+0x104)==180&&rdss(n[3]+0x104)==182);
    assert(rdss(row_list[0]+52)==26&&rdss(row_list[1]+52)==40&&rdss(row_list[1]+48)==26&&rdss(n[5]+0x324)==66);
    assert(rdss(n[7]+0x3a0)==21&&table_row_calls==calls+1&&rdss(n[17]+0x104)==56&&rdss(n[18]+0x104)==56);
    assert(rdss(n[19]+0xc4)==53&&rdss(n[20]+0xc4)==7.5f);
    assert(rd8(n[10]+0x2a9)==1&&rd8(n[11]+0x2a9)==1&&rd8(n[12]+0x2a9)==1);
    assert(rd8(n[10]+0x2aa)&&rd8(n[11]+0x2aa)&&rd8(n[12]+0x2aa));
    assert(rd8(n[13]+0x2a9)==1&&rdss(n[14]+0x104)==154&&fabsf(rdss(n[15]+0x100)-(154/.9f+20))<.01f);
    assert(layout_relayout_calls==relayout_before+5);
    assert(rd64(n[5]+0x2f0)==(uintptr_t)row_list&&rd32(n[5]+0x2f8)==2);
    fp_mobile_ui_before(cpu);assert(table_row_calls==calls+1&&scrollbar_refresh_calls==scroll_before+2);
    assert(rdss(n[15]+0x2f4)==121&&rdss(n[15]+0x320)==140);
    unsigned stable_flow=layout_relayout_calls;
    fp_mobile_ui_before(cpu);assert(layout_relayout_calls==stable_flow);
    wrss(n[16]+0x100,90);fp_mobile_ui_before(cpu);assert(scrollbar_refresh_calls==scroll_before+4&&rdss(n[15]+0x320)==90);
    // A newly inserted short row is enlarged even when the table settings did not change.
    wrss(row_list[0]+52,12);fp_mobile_ui_before(cpu);assert(table_row_calls==calls+2&&rdss(row_list[0]+52)==26);
    scale=150;fp_mobile_ui_before(cpu);assert(rdss(n[5]+0x104)==300&&rdss(n[1]+0xc4)+rdss(n[1]+0x104)<480);
    wrss(n[12]+0x104,94);wrss(n[13]+0x104,200);
    fp_mobile_ui_before(cpu);assert(layout_relayout_calls==stable_flow+2);
    fp_mobile_ui_before(cpu);assert(layout_relayout_calls==stable_flow+2);
    scale=100;fp_mobile_ui_before(cpu);
    assert(rdss(n[5]+0x104)==513&&rdss(n[4]+0x104)==480&&rdss(n[3]+0x104)==538&&rdss(n[5]+0x460)==5);
    assert(rdss(row_list[0]+52)==12&&rdss(row_list[1]+52)==40&&rdss(n[7]+0x3a0)==10);
    assert(rdss(n[17]+0x104)==41&&rdss(n[18]+0x104)==41&&rdss(n[19]+0xc4)==38&&rdss(n[20]+0xc4)==0);
    assert(!rd8(n[10]+0x2a9)&&!rd8(n[11]+0x2a9)&&!rd8(n[12]+0x2a9));
    assert(!rd8(n[10]+0x2aa)&&!rd8(n[11]+0x2aa)&&!rd8(n[12]+0x2aa));
    assert(!rd8(n[13]+0x2a9)&&rdss(n[14]+0x104)==596&&rdss(n[15]+0x100)==342);
    assert(rdss(n[12]+0x104)==596&&rdss(n[13]+0x104)==596);
    assert(layout_relayout_calls==stable_flow+7);
    wr64(n[6]+0x1d8,(uintptr_t)"EXT_BUILDING_HEADER");wr64(n[7]+0x1d8,(uintptr_t)"SFX_MASK_BLUR");
    scale=200;fp_mobile_ui_scale();CPU drawing=*cpu;drawing.rcx=n[7];CPU before=drawing;
    fp_mobile_ui_template(&drawing,phone_panel_template_return);
    before.rsp+=8;assert(!memcmp(&before,&drawing,sizeof(drawing)));
    wr64(n[6]+0x1d8,(uintptr_t)"EXT_TECHNOLOGY_PANEL");wr64(n[7]+0x1d8,(uintptr_t)"TECH_NAME");
    fp_mobile_ui_before(cpu);assert(rdss(n[7]+0x3a0)==12);
    scale=100;fp_mobile_ui_before(cpu);assert(rdss(n[7]+0x3a0)==10);
    cpu->rcx=saved;layout_relayout_calls=relayout_before;
}

static void check_phone_hud_cover(CPU *cpu){
    static _Alignas(16) unsigned char storage[7][0x600];uint64_t n[7];
    const char *names[]={"","EXT_SEARCH_CITIZENS_PANEL","EXT_NOTIFICATION_PANEL","EXT_EXPEDITIONS_PANEL","EXT_QUEST_PANEL","FRAME","HUD"};
    for(unsigned i=0;i<7;++i){n[i]=(uintptr_t)storage[i];wr64(n[i],GVA(i?0x141D78A60:0x141D7B0B0));wr64(n[i]+0x1d8,(uintptr_t)names[i]);wr32(n[i]+0x1e4,28000+i);
        wrss(n[i]+0xf0,1);wrss(n[i]+0xf4,1);wrss(n[i]+0x100,342);wrss(n[i]+0x104,220);wrss(n[i]+0x210,342);wrss(n[i]+0x214,220);
        if(i){wr64(n[i]+0x230,n[0]);if(i>1)wr64(n[i-1]+0x240,n[i]);else wr64(n[0]+0x220,n[i]);}
    }
    wr64(n[0]+0x220,n[5]);wr64(n[5]+0x220,n[6]);wr64(n[6]+0x220,n[1]);
    for(unsigned i=1;i<5;++i)wr64(n[i]+0x230,n[6]);
    wr64(n[6]+0x230,n[5]);for(unsigned i=4;i<7;++i)wr64(n[i]+0x240,0);
    uint64_t saved=cpu->rcx;cpu->rcx=n[0];scale=200;fp_mobile_ui_before(cpu);
    for(unsigned i=2;i<5;++i)assert(rdss(n[i]+0xf0)==0&&rdss(n[i]+0xf4)==0);
    wr32(n[1]+0x30,2);fp_mobile_ui_before(cpu);
    for(unsigned i=2;i<5;++i)assert(rdss(n[i]+0xf0)==1&&rdss(n[i]+0xf4)==1);
    wr32(n[1]+0x30,0);fp_mobile_ui_before(cpu);assert(rdss(n[2]+0xf0)==0);
    scale=100;fp_mobile_ui_before(cpu);for(unsigned i=2;i<5;++i)assert(rdss(n[i]+0xf0)==1&&rdss(n[i]+0xf4)==1);
    cpu->rcx=saved;
}
