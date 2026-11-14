#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define FP_LAW_LAYOUT_GEOMETRY_ONLY 1
#include "../android/host/mobile_law_layout.inc"

int main(void) {
    const float sizes[][2]={{640,360},{808,360},{1280,720},{960,540},{600,400}};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
        float width=sizes[i][0],height=sizes[i][1];law_photo_geometry g=law_photo_measure(width,height);
        assert(g.tree_top+g.tree_height<=height&&g.tree_height>0);
        assert(g.table_scale>0&&g.table_scale<=1);
        assert(g.table_left+412*g.table_scale<=width-11.9f);
        assert(g.table_top+363*g.table_scale<=height-11.9f);
        assert(g.table_top-95*g.table_scale>=11.9f);
        assert(g.art_size<=g.table_left-24);
        assert(108*g.action_scale<width/4&&25*g.action_scale<height-16);
        float law_action_height=40*fminf(1.2f,(g.table_left-16)/220);
        assert(60+g.art_size<g.law_action_top);
        assert(g.law_action_top+law_action_height<g.law_selected_y-24);
        assert(g.law_selected_y+24<height-42);
    }
    assert(law_photo_measure(NAN,360).table_scale==0);
    assert(law_photo_measure(640,0).table_scale==0);
    assert(law_photo_full_canvas(1,"ANIM","EXT_SOCIOTECH_PANEL","FRAME"));
    assert(law_photo_full_canvas(1,"TREE_VIEW","ANIM","EXT_SOCIOTECH_PANEL"));
    assert(!law_photo_full_canvas(1,"ANIM","INFO_PANEL","CONTENT"));
    assert(!law_photo_full_canvas(1,"FRAME","STATE_ELEM","TECH_ENTRY"));
    assert(law_photo_full_canvas(2,"SETTINGS_DETAILED","SETTINGS_PANEL","ANIM"));
    assert(!law_photo_full_canvas(2,"SETTINGS_DETAILED","INFO_PANEL","Table"));
    puts("law/photo layout geometry: 5 viewports and invalid inputs passed");
    return 0;
}
