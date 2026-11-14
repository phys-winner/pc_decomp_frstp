class SwitchMenuBase {public boolean performAccessibilityAction(int action,Object arguments){return false;}}
final class SwitchMenuCheck extends SwitchMenuBase {
    private static final int SWITCH_MENU=1<<25,SWITCH_AVAILABLE=1<<26;
    private static final int SWITCH_ACTION=0x01020000;
    private int switchState,switchSelection=-1,state,opens,closes,clicks,cancels,releases,performed;
    private boolean switchTouch;
    private float pointerX,pointerY;
    private float stickX,stickY,cursorStickX,cursorStickY,triggerZoom;
    private boolean gamepadActive,gamepadScheduled;
    private long gamepadFrame;
    private int gamepadStops;
    private final Runnable gamepadTick=()->{};
    private void removeCallbacks(Runnable callback){assert callback==gamepadTick;gamepadStops++;}
    private final long[] targets=new long[5];
    private final Gesture gesture=new Gesture();
    private final class Gesture {void cancel(){cancels++;}}
    private static final class MotionEvent {
        static final int ACTION_DOWN=0,ACTION_UP=1,ACTION_MOVE=2,ACTION_CANCEL=3,ACTION_POINTER_DOWN=5;
        final int action;final float x,y;
        MotionEvent(int action,float x,float y){this.action=action;this.x=x;this.y=y;}
        int getActionMasked(){return action;}float getX(){return x;}float getY(){return y;}
    }
    private int getWidth(){return 800;}private int getHeight(){return 360;}private int dp(float v){return Math.round(v);}
    private int mode(float x,float y){return state;}private void invalidate(){}
    private long nativeSwipeTarget(float x,float y,int direction){assert direction>=3&&direction<=7;return targets[direction-3];}
    private boolean nativeCamera(int action,float open,float y,float z,float angle){assert action==5&&z==0&&angle==0;if(open==1){assert y==0;opens++;}else{assert open==0&&y==0;closes++;}return true;}
    private void releasePointer(){releases++;}
    private void pointer(int buttons,float x,float y,float wheel){assert buttons==0&&wheel==0;pointerX=x;pointerY=y;}
    private void click(int button){assert button==1;clicks++;}private boolean performClick(){performed++;return true;}
    private static long point(float x,float y){return ((long)Float.floatToIntBits(x)<<32)|(Float.floatToIntBits(y)&0xffffffffL);}
    private void event(int action,float x,float y){assert switchTouch(new MotionEvent(action,x,y));}
    /* PRODUCTION_METHODS */
    public static void main(String[] args){
        SwitchMenuCheck f=new SwitchMenuCheck();
        assert !f.switchTouch(new MotionEvent(0,32,328));
        f.state=SWITCH_AVAILABLE;
        assert !f.switchTouch(new MotionEvent(0,400,180));
        f.gamepadActive=f.gamepadScheduled=true;f.gamepadFrame=50;
        f.stickX=f.stickY=f.cursorStickX=f.cursorStickY=f.triggerZoom=1;
        f.event(0,32,328);f.event(1,32,328);
        assert !f.gamepadActive&&!f.gamepadScheduled&&f.gamepadFrame==0;
        assert f.stickX==0&&f.stickY==0&&f.cursorStickX==0&&f.cursorStickY==0&&f.triggerZoom==0;
        assert f.opens==1&&f.closes==0&&f.clicks==0&&!f.switchTouch&&f.performed==1;
        f.state=SWITCH_AVAILABLE|SWITCH_MENU;
        f.targets[0]=point(.5f,.19f);f.targets[1]=point(.7f,.39f);
        f.event(0,404,70);f.event(1,404,70);
        assert f.clicks==1&&f.pointerX==400&&Math.abs(f.pointerY-68.4f)<.001f;
        f.event(0,400,68);f.event(2,500,140);f.event(1,500,140);
        assert f.clicks==2&&f.pointerX==560&&Math.abs(f.pointerY-140.4f)<.001f;
        f.targets[1]=0;f.event(0,500,140);f.event(1,500,140);
        assert f.clicks==2&&f.closes==1;
        f.event(0,400,68);f.event(2,20,20);f.event(1,20,20);
        assert f.clicks==2&&f.closes==2;
        f.event(0,400,68);f.event(3,400,68);assert f.closes==3&&f.clicks==2;
        f.event(0,400,68);f.event(5,400,68);f.event(1,400,68);assert f.closes==5&&f.clicks==2;
        f.state=0;assert !f.switchTouch(new MotionEvent(0,400,68));assert !f.switchTouch;
        assert f.cancels==7&&f.releases==7&&f.gamepadStops==7;
        assert !f.performAccessibilityAction(SWITCH_ACTION,null);
        f.state=SWITCH_MENU;assert !f.performAccessibilityAction(SWITCH_ACTION+1,null);
        f.gamepadActive=true;
        f.state=SWITCH_AVAILABLE;assert f.performAccessibilityAction(SWITCH_ACTION,null)&&f.opens==2;
        assert !f.gamepadActive&&f.gamepadStops==8;
        f.state=SWITCH_AVAILABLE|SWITCH_MENU;assert f.performAccessibilityAction(SWITCH_ACTION+1,null)&&f.clicks==3;
        assert !f.performAccessibilityAction(SWITCH_ACTION+2,null)&&!f.performAccessibilityAction(SWITCH_ACTION+7,null);
        for(int i=0;i<5;i++)assert Math.abs(Math.hypot(f.switchX(i)-400,f.switchY(i)-172.8)-104.4)<.001;
        f.targets[1]=point(.1f,.9f);assert f.switchChoose(f.switchItem(f.switchX(1),f.switchY(1)));
        assert f.clicks==4&&f.pointerX==80&&f.pointerY==324;
        assert f.switchChoose(-1)&&f.closes==6;
        System.out.println("{\"success\":true,\"switch_radial_touch_sequences\":7,\"original_button_centers\":true,\"fixed_radial_layout\":true,\"gamepad_selection\":true,\"drag_selection\":true,\"disabled_actions\":true,\"multitouch_cancel\":true,\"off_mode_passthrough\":true,\"accessibility_actions\":true}");
    }
}
