import java.util.*;
public final class TouchControls {
    static long now;
    static final Map<Runnable, Long> queue = new LinkedHashMap<>();
    final Map<Integer, Runnable> keyReleases = new HashMap<>();
    final List<float[]> events = new ArrayList<>(), cameraEvents = new ArrayList<>();
    static final List<String> keys = new ArrayList<>();
    final Gesture gesture = new Gesture();
    Runnable pointerRelease;
    static final int SWITCH_MENU=1<<25;
    int buttons, clicks, haptics, context = 1, resets, timeSpeed=1, timeBase, scenarios,switchCloses;
    boolean switchMenu(boolean open){assert !open;switchCloses++;context&=~SWITCH_MENU;return true;}
    float cursorX = 640, cursorY = 400, timeX,timeY,timeOrigin,wheelZoom;
    boolean timeVisible,placementCursor;
    boolean loading=true,hasFrame;
    long speedHintReadyAt;
    boolean speedHintSeen;
    static String controlsDescription="";
    final Preferences preferences=new Preferences();
    static class Preferences {
        boolean seen;
        Preferences edit(){return this;}
        Preferences putBoolean(String key,boolean value){assert key.equals("speed_hint_seen");seen=value;return this;}
        void apply(){}
    }
    static class Toast {
        static final int LENGTH_LONG=1;
        static int shows;
        static String text;
        static Toast makeText(Object context,String body,int duration){assert duration==LENGTH_LONG;text=body;return new Toast();}
        void show(){shows++;}
    }
    Object getContext(){return this;}
    boolean keyboardVisible;
    boolean editorActive=true;
    boolean nativeEditorActive(){return editorActive;}
    boolean clickReady=true;
    boolean remapTap,consumeTap;
    boolean limitSettings,expireSettingTarget;
    int settingIndex=5,settingCount=11,targetDirection,pressedSettingDirection,refreshSettingIndex=-1;
    final List<Integer> settingDirections=new ArrayList<>();
    long nativeClickToken(){return 1;}
    boolean nativeClickReady(long token){return clickReady;}
    void showKeyboard(){keyboardVisible=true;}
    boolean hideKeyboard(){boolean visible=keyboardVisible;keyboardVisible=false;return visible;}
    void stopGamepad(){}
    long nativeSwipeTarget(float x,float y,int d){
        if(d!=1&&d!=-1)targetDirection=0;
        if(d==2&&consumeTap)return -1L;
        if(d==2&&!remapTap)return 0;
        if(d==1||d==-1){
            if(limitSettings&&(settingCount<=0||settingIndex<0||settingIndex>=settingCount||d<0&&settingIndex==0||d>0&&settingIndex==settingCount-1)){targetDirection=0;return 0;}
            targetDirection=d;
        }
        return ((long)Float.floatToRawIntBits(d==0?.5f:d>0?.8f:.6f)<<32)|(Float.floatToRawIntBits(.5f)&0xffffffffL);
    }
    void nativePointer(int b,float x,float y,float w){pointer(b,x*getWidth(),y*getHeight(),w);}
    static final class KeyEvent { static final int KEYCODE_Q=45, KEYCODE_E=33; }
    static final class HapticFeedbackConstants { static final int LONG_PRESS=0,CLOCK_TICK=1; }
    static final class MotionEvent {
        static final int ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,ACTION_POINTER_DOWN=5,ACTION_POINTER_UP=6;
        final int action,index;final int[] ids;final float[] x,y;
        MotionEvent(int a,int i,int[] p,float[] xx,float[] yy){action=a;index=i;ids=p;x=xx;y=yy;}
        int getActionMasked(){return action;}int getActionIndex(){return index;}int getPointerCount(){return ids.length;}
        int getPointerId(int i){return ids[i];}float getX(int i){return x[i];}float getY(int i){return y[i];}
        int findPointerIndex(int id){for(int i=0;i<ids.length;i++)if(ids[i]==id)return i;return -1;}
    }
    int dp(float n){return Math.round(n);}int mode(float x,float y){return context;}
    int getWidth(){return 1280;}int getHeight(){return 800;}void invalidate(){}
    void postDelayed(Runnable r,long ms){queue.put(r,now+ms);}void removeCallbacks(Runnable r){queue.remove(r);}
    void pointer(int b,float x,float y,float w){
        assert Float.isFinite(x)&&Float.isFinite(y);
        if(b==1&&buttons==0&&targetDirection!=0){settingDirections.add(targetDirection);pressedSettingDirection=targetDirection;targetDirection=0;}
        if(b==0&&buttons==1&&pressedSettingDirection!=0){if(limitSettings)settingIndex=(settingIndex+pressedSettingDirection+settingCount)%settingCount;pressedSettingDirection=0;}
        if(b==0&&targetDirection!=0&&expireSettingTarget){settingIndex=0;expireSettingTarget=false;}
        if(b==0&&refreshSettingIndex>=0){settingIndex=refreshSettingIndex;refreshSettingIndex=-1;}
        buttons=b;cursorX=x;cursorY=y;events.add(new float[]{b,x,y,w});
    }
    final List<Float> buildingTurns=new ArrayList<>();
    final List<Integer> timeSelections=new ArrayList<>();
    boolean nativeCamera(int a,float x,float y,float z,float r){if(a==0)cameraEvents.add(new float[]{x,y,z,r});if(a==1)resets++;if(a==3)buildingTurns.add(r);if(a==4)timeSelections.add((int)x);return true;}
    static void nativeKey(int k,boolean d){keys.add(k+":"+d);}void performHapticFeedback(int k){haptics++;}void performClick(){clicks++;}
    /* PRODUCTION_METHODS */
    static void advance(long ms){long end=now+ms;for(;;){Map.Entry<Runnable,Long> n=null;for(Map.Entry<Runnable,Long> e:queue.entrySet())if(n==null||e.getValue()<n.getValue())n=e;if(n==null||n.getValue()>end)break;Runnable r=n.getKey();now=n.getValue();queue.remove(r);r.run();}now=end;}
    void one(int a,int id,float x,float y){gesture.touch(new MotionEvent(a,0,new int[]{id},new float[]{x},new float[]{y}));}
    void two(int a,int i,int p,float x,float y,int q,float xx,float yy){gesture.touch(new MotionEvent(a,i,new int[]{p,q},new float[]{x,xx},new float[]{y,yy}));}
    void pair(){one(0,7,400,400);two(5,1,7,400,400,19,800,400);}
    void end(){two(6,0,7,400,400,19,800,400);one(1,19,800,400);}
    boolean has(int b){return events.stream().anyMatch(e->e[0]==b);}
    void clear(){scenarios++;cancel();advance(1000);assert buttons==0&&queue.isEmpty();events.clear();cameraEvents.clear();buildingTurns.clear();timeSelections.clear();settingDirections.clear();keys.clear();clicks=haptics=resets=0;context=1;remapTap=consumeTap=limitSettings=expireSettingTarget=false;settingIndex=5;settingCount=11;targetDirection=pressedSettingDirection=0;refreshSettingIndex=-1;}
    void check(){
        context=2048;updateLoading();assert loading&&!hasFrame;
        presented();updateLoading();assert loading;
        context=0;updateLoading();assert !loading&&Toast.shows==0;
        context=2048;updateLoading();assert !loading&&Toast.shows==0;
        context=257;updateLoading();assert !loading&&Toast.shows==0&&!speedHintSeen;
        controlsDescription="select\n\nzoom\n\ncancel\n\nbuild\n\nHold the time bar, then drag to change game speed.\n\nsettings\n\nkeyboard";
        updateLoading();assert Toast.shows==0;
        now+=4999;updateLoading();assert Toast.shows==0;
        context=259;updateLoading();assert speedHintReadyAt==0;
        context=257;updateLoading();now+=5000;updateLoading();assert Toast.shows==1&&speedHintSeen&&preferences.seen&&Toast.text.startsWith("Hold the time bar");
        updateLoading();assert Toast.shows==1;
        context=2048|257;updateLoading();assert !loading&&Toast.shows==1;
        context=1;
        clickReady=false;one(0,7,300,200);one(1,7,300,200);advance(1000);assert buttons==0&&!has(1);
        clickReady=true;advance(16);assert buttons==1;
        clickReady=false;advance(1000);assert buttons==1;
        clickReady=true;advance(16);assert buttons==0;clear();
        context=SWITCH_MENU|2;click(1);advance(160);clickReady=false;advance(1000);
        assert buttons==0&&switchCloses==0;clickReady=true;advance(16);
        assert switchCloses==1&&(context&SWITCH_MENU)==0&&queue.isEmpty();clear();
        context=32770;one(0,7,300,200);assert !keyboardVisible;one(1,7,300,200);assert !keyboardVisible&&clicks==1;
        advance(176);assert keyboardVisible;
        context=2;one(0,7,500,200);one(1,7,500,200);assert !keyboardVisible;clear();
        context=32770;editorActive=false;one(0,7,300,200);one(1,7,300,200);advance(176);
        assert !keyboardVisible&&buttons==0&&has(1);clear();
        context=32770;editorActive=true;one(0,7,300,200);one(1,7,300,200);advance(160);editorActive=false;advance(16);
        assert !keyboardVisible&&buttons==0;editorActive=true;clear();
        one(0,7,300,200);assert !has(1);one(1,7,300,200);assert clicks==1&&has(1);advance(159);assert buttons==1;advance(1);assert buttons==0;clear();
        context=2048;one(0,7,300,200);one(1,7,300,200);advance(1000);assert events.isEmpty()&&cameraEvents.isEmpty()&&clicks==0;clear();
        one(0,7,300,200);one(2,7,380,200);one(1,7,400,200);assert !has(1)&&!has(2)&&clicks==0;assert Math.abs(cameraEvents.stream().mapToDouble(e->e[0]).sum()-.125)<.00001;clear();
        context=3;one(0,7,300,200);one(2,7,380,200);one(1,7,400,200);assert has(1)&&cameraEvents.isEmpty()&&buttons==0;clear();
        context=3|4096;one(0,7,300,200);one(2,7,380,200);one(1,7,400,200);assert has(2)&&!has(1)&&cameraEvents.isEmpty()&&buttons==0;clear();
        context=0;one(0,7,300,200);one(2,7,380,200);one(1,7,400,200);assert has(1)&&cameraEvents.isEmpty();clear();
        one(0,7,300,200);advance(450);one(2,7,350,250);assert cursorY==208;one(1,7,350,250);assert !has(1)&&haptics==1;clear();
        pair();two(2,0,7,350,400,19,850,400);assert events.stream().anyMatch(e->e[3]>0)&&keys.isEmpty()&&!has(2);end();clear();
        pair();two(2,0,7,450,400,19,750,400);assert events.stream().anyMatch(e->e[3]<0);end();clear();
        pair();two(2,0,7,400,350,19,800,450);assert cameraEvents.get(0)[3]>0&&keys.isEmpty();end();clear();
        context=3;pair();two(2,0,7,400,350,19,800,450);assert cameraEvents.isEmpty();end();clear();
        context=5;pair();two(2,0,7,400,350,19,800,450);assert buildingTurns.get(0)>0&&!has(4)&&cameraEvents.isEmpty();end();clear();
        context=5;pair();two(2,0,7,400,450,19,800,350);assert buildingTurns.get(0)<0&&cameraEvents.isEmpty();end();clear();
        context=5;one(0,7,300,400);one(2,7,500,400);one(1,7,500,400);assert !has(1)&&cameraEvents.isEmpty()&&cursorX==500;clear();
        context=5;one(0,7,300,400);one(1,7,300,400);assert has(1)&&clicks==1;clear();
        context=17|(64<<16);one(0,7,300,400);advance(159);assert !has(1);advance(1);assert buttons==1&&cursorY==336;
        one(2,7,600,500);assert buttons==1&&cursorY==436;one(1,7,600,500);assert buttons==1&&clicks==0;
        assert events.get(events.size()-2)[0]==0&&events.get(events.size()-1)[0]==1;advance(160);assert buttons==0;clear();
        context=17;one(0,7,300,400);advance(160);one(2,7,600,400);long starts=events.stream().filter(e->e[0]==1).count();
        one(3,7,600,400);assert buttons==0&&events.stream().filter(e->e[0]==1).count()==starts;clear();
        context=5|(96<<16);one(0,7,300,400);one(1,7,300,400);assert has(1)&&cursorY==304;clear();
        context=5|(96<<16)|(1<<24);one(0,7,300,400);one(1,7,300,400);assert has(1)&&cursorY==304;clear();
        context=17|(64<<16)|(1<<24);one(0,7,300,400);advance(160);assert buttons==1&&cursorY==336;
        one(3,7,300,400);assert buttons==0;clear();
        context=8194;one(0,7,300,400);one(2,7,400,400);one(1,7,400,400);advance(200);assert has(1)&&cursorX==1024&&cursorY==400&&settingDirections.equals(Arrays.asList(1,1,1));clear();
        context=8194;one(0,7,300,400);one(2,7,200,400);one(1,7,200,400);advance(200);assert has(1)&&cursorX==768&&settingDirections.equals(Arrays.asList(-1,-1,-1));clear();
        context=16386|4096;one(0,7,300,400);one(2,7,300,280);one(1,7,300,280);advance(64);assert !has(1)&&has(2)&&cursorY==280&&clicks==0;clear();
        context=16386|8192;one(0,7,300,400);one(2,7,400,400);one(1,7,400,400);advance(200);assert has(1)&&events.stream().allMatch(e->e[3]==0);clear();
        context=16386|8192;one(0,7,300,400);for(int x=302;x<=400;x+=2)one(2,7,x,400);one(1,7,400,400);advance(200);assert has(1)&&cursorX==1024&&settingDirections.size()==3;clear();
        context=16386|4096;one(0,7,300,400);for(int y=398;y>=280;y-=2){one(2,7,300,y);advance(16);}one(1,7,300,280);advance(64);
        assert has(2)&&!has(1)&&cursorY==280&&events.stream().allMatch(e->e[3]==0&&e[1]==300);
        assert events.stream().filter(e->e[0]==2).count()>20&&queue.isEmpty();clear();
        context=16386|8192;one(0,7,300,400);one(2,7,332,400);advance(64);assert settingDirections.size()==1;
        one(2,7,332,650);one(2,7,268,650);one(1,7,268,650);advance(200);
        assert settingDirections.equals(Arrays.asList(1,-1,-1))&&events.stream().allMatch(e->e[3]==0);clear();
        context=16386|8192;one(0,7,300,400);advance(100);one(2,7,364,650);one(1,7,364,650);advance(200);
        assert settingDirections.size()==2&&events.stream().allMatch(e->e[3]==0);clear();
        context=8194;limitSettings=true;settingIndex=10;one(0,7,500,400);one(2,7,116,400);advance(1000);
        assert settingIndex==0&&settingDirections.size()==10&&gesture.settingSent==-12&&buttons==0;
        one(2,7,148,400);advance(64);one(1,7,148,400);advance(1000);assert settingIndex==1&&settingDirections.size()==11;clear();
        context=8194;limitSettings=true;settingIndex=0;one(0,7,300,400);one(2,7,684,400);one(1,7,684,400);advance(1000);
        assert settingIndex==10&&settingDirections.size()==10&&buttons==0;clear();
        context=8194;limitSettings=true;settingIndex=0;settingCount=1;one(0,7,300,400);one(2,7,684,400);one(1,7,684,400);advance(1000);
        assert settingIndex==0&&settingDirections.isEmpty()&&!has(1);clear();
        context=8194;limitSettings=true;settingIndex=1;expireSettingTarget=true;one(0,7,300,400);one(2,7,268,400);one(1,7,268,400);advance(1000);
        assert settingIndex==0&&settingDirections.isEmpty()&&!has(1)&&queue.isEmpty();clear();
        context=8194;limitSettings=true;settingIndex=1;expireSettingTarget=true;one(0,7,300,400);one(2,7,268,400);one(2,7,332,400);one(1,7,332,400);advance(1000);
        assert settingIndex==1&&settingDirections.equals(Arrays.asList(1))&&queue.isEmpty();clear();
        context=8194;limitSettings=true;settingIndex=0;one(0,7,300,400);refreshSettingIndex=1;one(2,7,268,400);one(1,7,268,400);advance(1000);
        assert settingIndex==0&&settingDirections.equals(Arrays.asList(-1))&&queue.isEmpty();clear();
        context=8194;limitSettings=true;settingIndex=1;one(0,7,300,400);one(2,7,268,400);one(2,7,300,400);one(1,7,300,400);advance(1000);
        assert settingIndex==1&&settingDirections.isEmpty()&&!has(1)&&queue.isEmpty();clear();
        context=16386|64;one(0,7,300,400);one(2,7,350,400);assert !has(1);advance(32);assert buttons==1&&cursorX==690&&cursorY==400;
        one(2,7,410,620);advance(16);assert buttons==1&&cursorX==750&&cursorY==400;
        one(1,7,410,620);advance(32);assert buttons==0&&queue.isEmpty()&&events.stream().allMatch(e->e[3]==0);clear();
        context=16386|64;clickReady=false;one(0,7,300,400);one(2,7,350,400);one(1,7,350,400);advance(100);assert !has(1);
        clickReady=true;advance(64);assert has(1)&&buttons==0&&cursorX==690;clear();
        context=16386|64;one(0,7,300,400);advance(100);one(2,7,340,600);one(1,7,340,600);advance(64);
        assert has(1)&&buttons==0&&cursorX==680&&cursorY==400&&events.stream().allMatch(e->e[3]==0);clear();
        context=8194;one(0,7,300,400);one(2,7,396,400);advance(16);assert buttons==1;one(3,7,396,400);advance(200);
        assert buttons==0&&queue.isEmpty()&&settingDirections.size()==1;clear();
        context=8194;remapTap=true;one(0,7,300,400);one(1,7,300,400);assert has(1)&&cursorX==1024&&cursorY==400;clear();
        context=2;consumeTap=true;one(0,7,300,400);one(1,7,300,400);advance(200);assert !has(1)&&clicks==0&&queue.isEmpty();clear();
        context=16386|8192;one(0,7,300,400);one(2,7,300,380);advance(32);one(2,7,500,340);one(1,7,500,340);advance(64);
        assert has(2)&&!has(1)&&settingDirections.isEmpty()&&cursorX==300&&cursorY==340;clear();
        context=16386|4096|32;one(0,7,700,650);one(2,7,640,652);advance(32);assert buttons==2&&cursorX==640&&cursorY==650;
        one(2,7,400,690);one(1,7,400,690);advance(64);assert buttons==0&&cursorX==400&&cursorY==650&&!has(1)&&cameraEvents.isEmpty();clear();
        context=16386|4096|32;clickReady=false;one(0,7,700,650);one(2,7,500,650);one(1,7,400,650);advance(100);assert !has(2);
        clickReady=true;advance(64);assert has(2)&&buttons==0&&cursorX==400&&queue.isEmpty();clear();
        context=8194;one(0,7,300,400);one(2,7,396,400);advance(16);two(5,1,7,396,400,19,600,400);end();advance(200);
        assert buttons==0&&settingDirections.size()==1&&queue.isEmpty();clear();
        context=8194;one(0,7,300,400);one(2,7,396,400);advance(16);assert buttons==1;context=2;advance(16);
        assert buttons==0&&queue.isEmpty()&&settingDirections.size()==1;clear();
        context=16386|64;one(0,7,300,400);one(2,7,396,400);advance(32);assert buttons==1;context=2;advance(16);
        assert buttons==0&&queue.isEmpty();clear();
        pair();end();assert has(2)&&!has(1);clear();
        pair();two(2,0,19,800,400,7,400,400);assert Math.abs(cameraEvents.get(0)[2])<.001&&Math.abs(cameraEvents.get(0)[3])<.001;end();clear();
        pair();gesture.touch(new MotionEvent(5,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));
        gesture.touch(new MotionEvent(6,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));end();assert resets==1&&!has(1)&&!has(2);clear();
        pair();gesture.touch(new MotionEvent(5,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));advance(301);
        gesture.touch(new MotionEvent(6,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));end();assert resets==0;clear();
        context=11|256|(2<<9);one(0,7,400,20);advance(99);assert !timeVisible;advance(1);assert timeVisible&&timeSpeed==2;
        one(2,7,500,20);assert timeSelections.equals(Arrays.asList(3));one(2,7,500,20);assert timeSelections.size()==1;
        one(2,7,200,20);one(2,7,500,20);assert timeSelections.equals(Arrays.asList(3,0,3));
        one(1,7,500,20);assert !timeVisible&&!has(1)&&cameraEvents.isEmpty()&&keys.isEmpty();clear();
        pair();gesture.touch(new MotionEvent(5,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));
        gesture.touch(new MotionEvent(2,0,new int[]{7,19,31},new float[]{401,799,600},new float[]{400,400,601}));
        gesture.touch(new MotionEvent(6,2,new int[]{7,19,31},new float[]{401,799,600},new float[]{400,400,601}));end();assert resets==1;clear();
        pair();gesture.touch(new MotionEvent(5,2,new int[]{7,19,31},new float[]{400,800,600},new float[]{400,400,600}));
        gesture.touch(new MotionEvent(2,0,new int[]{7,19,31},new float[]{400,850,600},new float[]{400,400,600}));
        gesture.touch(new MotionEvent(6,2,new int[]{7,19,31},new float[]{400,850,600},new float[]{400,400,600}));end();assert resets==0;clear();
        one(0,1,300,300);one(2,1,500,300);one(3,1,500,300);assert buttons==0&&queue.isEmpty();clear();
        one(0,1,300,300);one(2,2,500,300);assert gesture.first<0&&queue.isEmpty();clear();
        one(0,1,Float.NaN,300);assert gesture.first<0&&queue.isEmpty()&&events.isEmpty();clear();
        one(0,1,300,300);one(2,1,Float.POSITIVE_INFINITY,300);assert gesture.first<0&&queue.isEmpty();clear();
        one(0,1,300,300);one(1,1,300,300);pulse(45);cancel();advance(1000);assert buttons==0&&keys.contains("45:false")&&queue.isEmpty();clear();
        System.out.println("{\"success\":true,\"scenarios\":"+scenarios+",\"ime_tap_and_dismiss\":true,\"ime_requires_original_editor_after_up_ack\":true,\"production_gesture_and_release_methods\":true,\"production_time_slider_methods\":true,\"building_preview_drag_without_placement\":true,\"road_hold_and_release\":true,\"road_cancel_without_commit\":true,\"placement_offset\":true,\"settings_swipes_both_directions\":true,\"settings_incremental_swipe\":true,\"settings_vertical_scroll\":true,\"settings_continuous_adjustment\":true,\"settings_axis_lock\":true,\"slider_acknowledged_drag\":true,\"expanded_tap_remap\":true,\"consumed_law_deselect_tap\":true,\"loading_input_blocked\":true,\"ui_does_not_rotate_camera\":true,\"law_tree_right_drag\":true}");
    }
    public static void main(String[] args){new TouchControls().check();}
}
