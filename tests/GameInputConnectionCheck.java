import java.util.*;
public final class GameInputConnectionCheck {
    final List<String> text=new ArrayList<>(),keys=new ArrayList<>(),deletions=new ArrayList<>();
    boolean nativeAccepted=true;
    boolean hidden;
    static final class Mode {
        final int width,height;final float rate;
        Mode(int w,int h,float r){width=w;height=h;rate=r;}
        int getPhysicalWidth(){return width;} int getPhysicalHeight(){return height;} float getRefreshRate(){return rate;}
    }
    /* PRODUCTION_FRAME_RATES */
    /* PRODUCTION_IME_FRACTION */
    static final class View {
        boolean visible,focus,focusable,focusableInTouchMode;boolean hasInsets=true;
        Insets getRootWindowInsets(){return hasInsets?new Insets(visible):null;}
        Object getWindowToken(){return this;}
        void clearFocus(){focus=false;}
        void setFocusableInTouchMode(boolean value){focusableInTouchMode=value;if(value)focusable=true;}
        void setFocusable(boolean value){focusable=value;if(!value)focus=false;}
        boolean requestFocus(){focus=focusable;return focus;}
        void requestApplyInsets(){}
    }
    static final class Insets {
        final boolean visible;Insets(boolean v){visible=v;}
        boolean isVisible(int type){return visible;}
        static final class Type{static int ime(){return 1;}}
    }
    static final class InputManager{
        static final int SHOW_IMPLICIT=1;
        void restartInput(View view){assert view.focus;}
        void showSoftInput(View view,int flags){assert view.focus;view.visible=true;}
        void hideSoftInputFromWindow(Object token,int flags){((View)token).visible=false;}
    }
    static final class Context{
        static final String INPUT_METHOD_SERVICE="input";
        Object getSystemService(String name){return new InputManager();}
    }
    static final class KeyboardBackCheck{
        boolean keyboardVisible;
        View keyboard=new View();
        Context getContext(){return new Context();}
        void nativeImeViewport(float fraction){assert fraction==1;}
        /* PRODUCTION_SHOW_KEYBOARD */
        /* PRODUCTION_HIDE_KEYBOARD */
        void check(){
            assert !keyboardVisible&&!keyboard.focus&&!keyboard.focusable&&!keyboard.focusableInTouchMode;
            showKeyboard();assert keyboardVisible&&keyboard.visible&&keyboard.focus&&keyboard.focusableInTouchMode;
            assert hideKeyboard()&&!keyboardVisible&&!keyboard.focus&&!keyboard.focusable&&!keyboard.focusableInTouchMode;
            showKeyboard();assert keyboardVisible&&keyboard.visible&&keyboard.focus;
            assert hideKeyboard()&&!keyboard.focus&&!keyboard.focusable;
            keyboardVisible=true;keyboard.visible=false;assert !hideKeyboard()&&!keyboardVisible&&!keyboard.focus;
            keyboardVisible=true;keyboard.visible=true;assert hideKeyboard()&&!keyboardVisible;
            assert !hideKeyboard();
        }
    }
    static class BaseInputConnection {
        BaseInputConnection(View view,boolean full){}
        public boolean setComposingText(CharSequence text,int cursor){return false;}
        public boolean finishComposingText(){return false;}
        public boolean commitText(CharSequence text,int cursor){return false;}
        public boolean deleteSurroundingText(int before,int after){return false;}
        public boolean deleteSurroundingTextInCodePoints(int before,int after){return false;}
        public boolean sendKeyEvent(KeyEvent event){return false;}
        public boolean performEditorAction(int action){return false;}
    }
    static final class KeyEvent {
        static final int ACTION_DOWN=0,ACTION_UP=1,KEYCODE_ENTER=66,KEYCODE_DEL=67,KEYCODE_FORWARD_DEL=112,
            KEYCODE_DPAD_LEFT=21,KEYCODE_DPAD_RIGHT=22,KEYCODE_MOVE_HOME=122,KEYCODE_MOVE_END=123;
        int action,key,unicode;
        KeyEvent(int a,int k,int u){action=a;key=k;unicode=u;}
        int getAction(){return action;} int getKeyCode(){return key;} int getUnicodeChar(){return unicode;}
    }
    boolean nativeText(String value){if(!nativeAccepted)return false;text.add(value);return true;}
    boolean nativeDelete(int before,int after,boolean points){if(!nativeAccepted)return false;deletions.add(before+":"+after+":"+points);return true;}
    boolean nativeEditorKey(int key){if(!nativeAccepted)return false;keys.add(Integer.toString(key));return true;}
    boolean nativeKey(int key,boolean down){keys.add(key+":"+down);return true;}
    boolean hideKeyboard(){hidden=true;return true;}
    /* PRODUCTION_INPUT_CONNECTION */
    void check(){
        new KeyboardBackCheck().check();
        assert Math.abs(imeVisibleFraction(true,true,1080,712)-368f/1080)<.00001f;
        assert imeVisibleFraction(false,true,1080,712)==1&&imeVisibleFraction(true,false,1080,0)==1;
        assert imeVisibleFraction(true,true,0,712)==1&&imeVisibleFraction(true,true,1080,1200)==0&&imeVisibleFraction(true,true,1080,-10)==1;
        assert Arrays.equals(frameRates(new Mode(2400,1080,119.88f),new Mode[]{new Mode(2400,1080,60),new Mode(2400,1080,90),new Mode(2400,1080,120),new Mode(2400,1080,119.88f),new Mode(1920,1080,144),new Mode(2400,1080,Float.NaN)}),new int[]{120,60,90,120});
        GameInputConnection input=new GameInputConnection(new View());
        assert input.setComposingText("に",1)&&input.setComposingText("日本",1)&&text.isEmpty();
        assert input.commitText("日本",1)&&text.equals(Arrays.asList("日本"));
        assert input.finishComposingText()&&text.size()==1;
        assert input.setComposingText("東京",1)&&input.deleteSurroundingText(1,0)&&input.finishComposingText();
        assert text.equals(Arrays.asList("日本","東"));
        assert input.commitText("Ж😀",1)&&text.get(2).equals("Ж😀");
        assert input.deleteSurroundingText(2,1)&&deletions.equals(Arrays.asList("2:1:false"));
        assert input.deleteSurroundingTextInCodePoints(1,2)&&deletions.get(1).equals("1:2:true");
        assert input.setComposingText("A😀B",1)&&input.deleteSurroundingTextInCodePoints(2,0)&&input.finishComposingText();
        assert text.get(text.size()-1).equals("A");
        nativeAccepted=false;assert !input.deleteSurroundingTextInCodePoints(1,0);
        assert input.setComposingText("Ж",1)&&!input.finishComposingText();nativeAccepted=true;
        assert input.finishComposingText()&&text.get(text.size()-1).equals("Ж");
        assert !input.deleteSurroundingText(-1,0)&&!input.deleteSurroundingText(257,0);
        int before=text.size();input.sendKeyEvent(new KeyEvent(0,0,0x80000001));assert text.size()==before;
        assert input.sendKeyEvent(new KeyEvent(0,0,0x1f600))&&text.get(text.size()-1).equals("😀");
        before=text.size();assert input.sendKeyEvent(new KeyEvent(1,0,0x1f600))&&text.size()==before;
        assert input.performEditorAction(6)&&hidden&&keys.get(keys.size()-1).equals("66");
        System.out.println("{\"success\":true,\"composition\":true,\"unicode\":true,\"deletion\":true,\"editor_action\":true,\"dead_key\":true,\"ime_focus_only_while_requested\":true,\"ime_repeat_open\":true}");
    }
    public static void main(String[] args){new GameInputConnectionCheck().check();}
}
