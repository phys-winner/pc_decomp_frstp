package com.frostpunk.staticrecomp.probe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.RectF;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.drawable.ColorDrawable;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

/** Analog camera deltas are consumed by the original camera controller on its game thread. */
final class TouchControls extends View {
    private static native long nativeFrameTimes(int[] intervalsUs);
    /** Presented-frame intervals for the launcher's FPS overlay; the constructor has loaded the library. */
    static long frameTimes(int[] intervalsUs) { return nativeFrameTimes(intervalsUs); }
    private static native boolean nativePointer(int buttons, float x, float y, float wheel);
    private static native boolean nativeKey(int key, boolean down);
    private static native boolean nativeText(String text);
    private static native boolean nativeDelete(int before,int after,boolean codePoints);
    private static native boolean nativeEditorKey(int key);
    private static native boolean nativeEditorActive();
    private static native void nativeImeViewport(float visibleFraction);
    private static native long nativeClickToken();
    private static native boolean nativeClickReady(long ticket);
    private static native boolean nativeMouse(int action, int buttons, int meta, float x, float y, float scrollX, float scrollY);
    private static native boolean nativeCamera(int action, float dx, float dy, float zoom, float angle);
    private static native int nativeContext(float x, float y);
    private static native long nativeSwipeTarget(float x,float y,int direction);
    private static native int nativeConstructionView(float[] corners,char[] detail,int painter);
    private final Gesture gesture = new Gesture();
    private final Map<Integer, Runnable> keyReleases = new HashMap<>();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final android.content.SharedPreferences preferences;
    private static String loadingLabel="",controlsDescription="";
    private Runnable pointerRelease;
    private int buttons, timeSpeed = 1, timeBase;
    private float cursorX, cursorY, timeX, timeY, timeOrigin, wheelZoom;
    private boolean timeVisible, placementCursor, gamepadActive, loading=true, hasFrame, speedHintSeen, startupFailed;
    private static final int SWITCH_MENU=1<<25, SWITCH_AVAILABLE=1<<26;
    /* The next zoom-out step leaves the city for the world map (native map gate). */
    private static final int MAP_NEXT=1<<27;
    private int mapAttempts;
    private long mapAttemptAt;
    private boolean mapGestureCounted, mapAllowed;
    private static final int SWITCH_ACTION=0x01020000;
    private int switchState,switchAnnouncedState,switchSelection=-1;
    private boolean switchTouch,switchImagesTried;
    private Bitmap switchRing,switchHalo;
    private final Bitmap[] switchIcons=new Bitmap[5];
    /* Holding the corner button opens the five actions as a fan around it; sliding onto one and
     * lifting the finger chooses it (a quick tap still opens the full ring). */
    private boolean switchFan;
    private final Runnable switchFanHold=()->{if(switchTouch&&switchSelection==-2)openSwitchFan();};
    /* Touch construction aims at the world below the screen centre and pans the camera under it. */
    private static final int CONSTRUCTION_OPEN=1<<28;
    private static final int BUILD_ROAD=0,REMOVE_ROAD=1,CONFIRM=2,CANCEL=3;
    private static final Path CONSTRUCTION_OUTLINE=constructionOutline();
    private static final float[] CONSTRUCTION_CORNERS={-1,-1,1,-1,1,1,-1,1};
    private final Matrix constructionProjection=new Matrix();
    private final Path projectedOutline=new Path();
    private final float[] constructionQuad=new float[8],nextConstructionQuad=new float[8];
    private final char[] streetDetailText=new char[256];
    private String streetDetail="";
    private boolean centerMode,centerTouch,mouseConstruction,touchAim,constructionProjected;
    private int painterKind,constructionPressed=-1;
    /* Game localization (native): construction, economy, search, laws, technology, build street,
     * dismantle street, OK, cancel, dismantle building, building streets, dismantling streets, building pipes. */
    private static String[] gameLabels=new String[0];
    void presented(){hasFrame=true;}
    void failed(){startupFailed=true;loading=false;removeCallbacks(loadingPoll);cancel();hideKeyboard();if(popup!=null)popup.dismiss();}
    private final Runnable loadingPoll=new Runnable(){@Override public void run(){
        if(gamepadActive&&gamepadId>=0&&InputDevice.getDevice(gamepadId)==null){
            cancel();gamepadId=-1;invalidate();
        }
        int next=mode(centerX(),centerY());
        if(loading&&hasFrame&&(next&2048)==0){loading=false;invalidate();}
        updateCenterMode(next);
        if(((next^switchAnnouncedState)&(SWITCH_MENU|SWITCH_AVAILABLE))!=0){
            invalidate();sendAccessibilityEvent(android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
        }
        switchState=switchAnnouncedState=next;
        postDelayed(this,centerMode?32:200);
    }};
    /* Construction state (native context bits 28-31) without position: the menu is open or one of the
     * construction tools is active, the city camera works and no menu, loading or ring covers the city. */
    private void updateCenterMode(int state){
        int painter=(state>>>29)&7;
        boolean touch=!loading&&!startupFailed&&!gamepadActive&&!mouseConstruction&&((Activity)getContext()).hasWindowFocus();
        if(touch!=touchAim&&nativeCamera(8,touch?1:0,0,0,0))touchAim=touch;
        boolean next=touch&&(state&2048)==0&&(state&1)!=0&&(state&SWITCH_MENU)==0
            &&((state&CONSTRUCTION_OPEN)!=0||painter!=0);
        int view=next?nativeConstructionView(nextConstructionQuad,streetDetailText,painter):0;
        boolean projected=(view&1)!=0;
        boolean projectionChanged=projected!=constructionProjected
            ||projected&&!Arrays.equals(constructionQuad,nextConstructionQuad);
        if(projected&&projectionChanged)System.arraycopy(nextConstructionQuad,0,constructionQuad,0,constructionQuad.length);
        constructionProjected=projected;
        int units=view>>>1;
        boolean detailChanged=streetDetail.length()!=units;
        for(int i=0;!detailChanged&&i<units;++i)detailChanged=streetDetail.charAt(i)!=streetDetailText[i];
        if(detailChanged)streetDetail=new String(streetDetailText,0,units);
        boolean changed=next!=centerMode||painter!=painterKind;
        // A tool picked in the construction menu goes back to that menu when it is cancelled (the
        // Switch layout hides the menu's buttons as soon as the tool starts).
        if(painterKind==0&&painter!=0)toolFromMenu=(lastConstructionState&CONSTRUCTION_OPEN)!=0;
        else if(painter==0&&(state&CONSTRUCTION_OPEN)==0&&!reopening)toolFromMenu=false;
        lastConstructionState=state;
        centerMode=next;painterKind=painter;
        if(!next){constructionPressed=-1;centerTouch=false;}
        // The tool follows the mouse: park it on the centre cursor unless a finger or click owns it.
        if(changed&&next&&!gesture.active())recenter();
        if(changed||projectionChanged||detailChanged)invalidate();
    }
    private float centerX(){return getWidth()*.5f;}
    private float centerY(){return getHeight()*.5f;}
    private void recenter(){
        if(centerMode&&painterKind!=0&&getWidth()>0&&getHeight()>0&&pointerRelease==null)pointer(0,centerX(),centerY(),0);
    }
    private PopupWindow popup;
    private View keyboard;
    private volatile boolean keyboardVisible;
    boolean textInputActive(){return keyboardVisible;}
    private static float imeVisibleFraction(boolean editor,boolean visible,int height,int bottom){
        if(!editor||!visible||height<=0)return 1;
        return Math.max(0,Math.min(1,1-bottom/(float)height));
    }
    private void updateImeViewport(android.view.WindowInsets insets){
        boolean visible=insets!=null&&insets.isVisible(android.view.WindowInsets.Type.ime());
        int bottom=visible?insets.getInsets(android.view.WindowInsets.Type.ime()).bottom:0;
        nativeImeViewport(imeVisibleFraction(keyboardVisible,visible,getHeight(),bottom));
    }
    private final class GameInputConnection extends android.view.inputmethod.BaseInputConnection {
        private String composing="";
        GameInputConnection(View view){super(view,false);}
        @Override public boolean setComposingText(CharSequence text,int cursor){composing=text.toString();return true;}
        @Override public boolean finishComposingText(){if(!composing.isEmpty()&&!nativeText(composing))return false;composing="";return true;}
        @Override public boolean commitText(CharSequence text,int cursor){if(!nativeText(text.toString()))return false;composing="";return true;}
        @Override public boolean deleteSurroundingText(int before,int after){return deleteText(before,after,false);}
        @Override public boolean deleteSurroundingTextInCodePoints(int before,int after){return deleteText(before,after,true);}
        private boolean deleteText(int before,int after,boolean points){
            if(before<0||after<0||before>256||after>256)return false;
            String previous=composing;
            if(!composing.isEmpty()){
                int removed=Math.min(before,points?composing.codePointCount(0,composing.length()):composing.length());
                int remaining=points?composing.offsetByCodePoints(composing.length(),-removed):composing.length()-removed;
                composing=composing.substring(0,remaining);before-=removed;
            }
            if((before!=0||after!=0)&&!nativeDelete(before,after,points)){composing=previous;return false;}
            return true;
        }
        @Override public boolean sendKeyEvent(KeyEvent event){
            int key=event.getKeyCode();
            if(key==KeyEvent.KEYCODE_DEL||key==KeyEvent.KEYCODE_FORWARD_DEL){
                return event.getAction()==KeyEvent.ACTION_UP||deleteText(key==KeyEvent.KEYCODE_DEL?1:0,key==KeyEvent.KEYCODE_FORWARD_DEL?1:0,true);
            }
            if(key==KeyEvent.KEYCODE_ENTER)return event.getAction()==KeyEvent.ACTION_UP||performEditorAction(0);
            if(key==KeyEvent.KEYCODE_DPAD_LEFT||key==KeyEvent.KEYCODE_DPAD_RIGHT||key==KeyEvent.KEYCODE_MOVE_HOME||key==KeyEvent.KEYCODE_MOVE_END)
                return event.getAction()==KeyEvent.ACTION_UP||finishComposingText()&&nativeEditorKey(key);
            int unicode=event.getUnicodeChar();
            if(event.getAction()==KeyEvent.ACTION_DOWN&&unicode>0&&Character.isValidCodePoint(unicode))
                return nativeText(new String(Character.toChars(unicode)));
            return event.getAction()==KeyEvent.ACTION_UP;
        }
        @Override public boolean performEditorAction(int action){if(!finishComposingText()||!nativeEditorKey(KeyEvent.KEYCODE_ENTER))return false;hideKeyboard();return true;}
    }
    private void showKeyboard(){
        if(keyboard==null)return;
        keyboard.setFocusableInTouchMode(true);
        keyboardVisible=true;keyboard.requestFocus();
        keyboard.requestApplyInsets();
        android.view.inputmethod.InputMethodManager input=(android.view.inputmethod.InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        input.restartInput(keyboard);input.showSoftInput(keyboard,android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
    }
    boolean hideKeyboard(){
        if(!keyboardVisible)return false;
        android.view.WindowInsets insets=keyboard.getRootWindowInsets();
        boolean visible=insets==null||insets.isVisible(android.view.WindowInsets.Type.ime());
        keyboardVisible=false;
        nativeImeViewport(1);
        ((android.view.inputmethod.InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(keyboard.getWindowToken(),0);
        keyboard.clearFocus();keyboard.setFocusableInTouchMode(false);keyboard.setFocusable(false);return visible;
    }

    TouchControls(Context context) {
        super(context);
        preferences=context.getSharedPreferences("touch_controls",Context.MODE_PRIVATE);
        String language=System.getenv("PORT_LANGUAGE");
        if(language==null)language=java.util.Locale.getDefault().getLanguage();
        String initial=language.equals("ru")?"Идёт загрузка...":language.equals("de")?"Wird geladen...":language.equals("pl")?"Ładowanie...":"Loading...";
        loadingLabel=preferences.getString("loading",initial);
        speedHintSeen=preferences.getBoolean("speed_hint_seen",false);
        System.loadLibrary("frostpunk_app");
        setContentDescription(controlsDescription);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
    }
    void localize(String loading,String description){
        // The runtime appends the game's own button names: help text, U+001E, labels split by U+001F.
        int split=description.indexOf('\u001e');
        if(split>=0){gameLabels=description.substring(split+1).split("\u001f",-1);description=description.substring(0,split);}
        loadingLabel=loading;controlsDescription=description;setContentDescription(description);invalidate();
        preferences.edit().putString("loading",loading).apply();
    }
    private static String gameLabel(int index){return index>=0&&index<gameLabels.length?gameLabels[index]:"";}
    /* Once, after the first tap on the time bar: its speed is changed by swiping along it. */
    private void showSpeedHint(){
        if(speedHintSeen||controlsDescription.isEmpty())return;
        // shortcut: speed help is paragraph five of GestureHelpText; use its own key if that format changes.
        String[] paragraphs=controlsDescription.split("\n\n");
        if(paragraphs.length<=4)return;
        android.widget.Toast.makeText(getContext(),paragraphs[4],android.widget.Toast.LENGTH_LONG).show();
        speedHintSeen=true;preferences.edit().putBoolean("speed_hint_seen",true).apply();
    }
    private void speedHintKnown(){
        if(speedHintSeen)return;
        speedHintSeen=true;preferences.edit().putBoolean("speed_hint_seen",true).apply();
    }
    private int dp(float value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    void attach(Activity activity) {
        post(loadingPoll);
        keyboard=new View(activity){
            @Override public boolean onCheckIsTextEditor(){return true;}
            @Override public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo info){
                info.inputType=android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
                info.imeOptions=android.view.inputmethod.EditorInfo.IME_ACTION_DONE|android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI;
                return new GameInputConnection(this);
            }
        };
        keyboard.setFocusable(false);
        keyboard.setOnApplyWindowInsetsListener((view,insets)->{updateImeViewport(insets);return insets;});
        activity.addContentView(keyboard,new FrameLayout.LayoutParams(1,1));
        activity.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
        FrameLayout layer = new FrameLayout(activity);
        layer.addView(this, new FrameLayout.LayoutParams(-1, -1));
        // Vulkan owns NativeActivity's surface; the transparent subwindow receives touch input.
        popup = new PopupWindow(layer, -1, -1, false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setInputMethodMode(PopupWindow.INPUT_METHOD_NEEDED);
        popup.setIsLaidOutInScreen(true);
        popup.setClippingEnabled(false);
        layer.setSystemUiVisibility(activity.getWindow().getDecorView().getSystemUiVisibility());
        activity.getWindow().getDecorView().post(() -> {
            if (startupFailed || activity.isFinishing() || activity.isDestroyed()) return;
            popup.showAtLocation(activity.getWindow().getDecorView(), Gravity.TOP | Gravity.LEFT, 0, 0);
            View root = popup.getContentView().getRootView();
            if (root.getLayoutParams() instanceof WindowManager.LayoutParams) {
                WindowManager.LayoutParams params = (WindowManager.LayoutParams) root.getLayoutParams();
                params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
                activity.getWindowManager().updateViewLayout(root, params);
            }
        });
    }
    private void pointer(int pressed, float x, float y, float wheel) {
        if (getWidth() <= 0 || getHeight() <= 0) return;
        cursorX = Math.max(0, Math.min(getWidth() - 1, x));
        cursorY = Math.max(0, Math.min(getHeight() - 1, y));
        buttons = pressed;
        nativePointer(pressed, cursorX / getWidth(), cursorY / getHeight(), wheel);
        if(placementCursor || gamepadActive) invalidate();
    }
    private int mode(float x, float y) {
        return getWidth() > 0 && getHeight() > 0 ? nativeContext(x / getWidth(), y / getHeight()) : 0;
    }
    private void camera(float dx, float dy, float zoom, float turn) {
        if (getHeight() > 0) nativeCamera(0, dx / getHeight(), dy / getHeight(), 0, turn);
        smoothZoom(zoom);
    }
    private static final int ZOOM_LIMIT=1<<7;
    /* Pinch zoom follows the fingers between the city zoom levels (native action 7) and ends on the
     * nearest level.  At the farthest city level a zoom-out takes the original wheel step with the
     * world-map gate; the world map and camera transitions keep the wheel as well. */
    private void smoothZoom(float zoom){
        if(zoom==0)return;
        int state=mode(cursorX,cursorY);
        if(zoom<0&&(state&MAP_NEXT)!=0){zoomWheel(zoom);return;}
        if(!nativeCamera(7,0,0,zoom,0)){zoomWheel(zoom);return;}
        if(zoom<0&&(state&ZOOM_LIMIT)!=0&&!mapGestureCounted){
            // Pushed past the farthest level: the first of the two map gestures, as the wheel counts it.
            mapGestureCounted=true;
            long now=android.os.SystemClock.uptimeMillis();
            mapAttempts=now-mapAttemptAt<6000?mapAttempts+1:1;mapAttemptAt=now;
            if(mapAttempts<2){performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);mapHint();}
        }
    }
    private long zoomOutAt, zoomOutTicket;
    private void zoomWheel(float zoom){
        wheelZoom+=zoom;
        while(Math.abs(wheelZoom)>=.14f){
            float direction=wheelZoom>0?1:-1;
            if(direction<0){
                // A burst of queued notches outran the map gate's level check (one pinch went 2->3->4 straight
                // into the map). Send the next zoom-out notch only after the game took the previous one
                // (input ticket) and had a frame to apply it, so the context reports the real level.
                long now=android.os.SystemClock.uptimeMillis();
                boolean waiting=zoomOutTicket!=0&&!nativeClickReady(zoomOutTicket)&&now-zoomOutAt<400;
                if(waiting||now-zoomOutAt<40){wheelZoom=Math.max(wheelZoom,-.28f);return;}
                zoomOutAt=now;
            }
            if(direction>0)mapAttempts=0;
            else if(!mapAllowed&&(mode(cursorX,cursorY)&MAP_NEXT)!=0){
                // At the farthest city zoom one more step opens the world map. A pinch that merely runs
                // into the limit must not leave the city: the map opens on a second, separate pinch-out.
                if(!mapGestureCounted){
                    mapGestureCounted=true;
                    long now=android.os.SystemClock.uptimeMillis();
                    mapAttempts=now-mapAttemptAt<6000?mapAttempts+1:1;mapAttemptAt=now;
                    android.util.Log.i("FrostpunkTouch","map gate attempt "+mapAttempts);
                    if(mapAttempts>=2){mapAttempts=0;mapAllowed=true;}
                    else{performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);mapHint();}
                }
                if(!mapAllowed){wheelZoom=0;return;}
            }
            // The original wheel command enforces scenario zoom levels and the distant-view gate.  It is
            // ignored over UI (a building marker left under the cursor by the first finger kept a
            // pinch from opening the map): the step then uses the screen centre.
            if((mode(cursorX,cursorY)&2)!=0&&(mode(getWidth()*.5f,getHeight()*.5f)&2)==0){cursorX=getWidth()*.5f;cursorY=getHeight()*.5f;}
            nativePointer(buttons,cursorX/getWidth(),cursorY/getHeight(),direction);
            if(direction<0)zoomOutTicket=nativeClickToken();
            wheelZoom-=direction*.14f;
        }
    }
    private void mapHint(){
        String language=java.util.Locale.getDefault().getLanguage();
        String text=language.equals("ru")?"Отдалите камеру ещё раз, чтобы открыть карту"
            :language.equals("de")?"Noch einmal herauszoomen, um die Karte zu öffnen"
            :language.equals("pl")?"Oddal jeszcze raz, aby otworzyć mapę"
            :"Zoom out once more to open the map";
        android.widget.Toast.makeText(getContext(),text,android.widget.Toast.LENGTH_SHORT).show();
    }
    private void releasePointer() {
        if (pointerRelease != null) { removeCallbacks(pointerRelease); pointerRelease = null; }
        if (buttons != 0) pointer(0, cursorX, cursorY, 0);
    }
    private void click(int button) {
        releasePointer();
        pointer(0,cursorX,cursorY,0);
        final boolean editor=(mode(cursorX,cursorY)&32768)!=0;
        final boolean radial=(mode(cursorX,cursorY)&SWITCH_MENU)!=0;
        // The original UI must process hover before Down, and Down before Up.
        pointerRelease = new Runnable(){
            private long ticket=nativeClickToken();
            private int phase;
            @Override public void run(){
                if(!nativeClickReady(ticket)){postDelayed(this,16);return;}
                if(phase==0){pointer(button,cursorX,cursorY,0);ticket=nativeClickToken();phase=1;postDelayed(this,160);return;}
                if(phase==1){pointer(0,cursorX,cursorY,0);if(editor||radial){ticket=nativeClickToken();phase=2;postDelayed(this,16);return;}}
                pointerRelease=null;if(radial)switchMenu(false);if(editor&&nativeEditorActive())showKeyboard();
                // A menu click left the mouse on a card or button: the active tool returns to the centre cursor.
                if(centerMode&&!gesture.active())postDelayed(TouchControls.this::recenter,16);
            }
        };
        pointerRelease.run();
    }
    void pulse(int key) {
        Runnable previous = keyReleases.remove(key);
        if (previous != null) removeCallbacks(previous);
        else nativeKey(key, true);
        Runnable release = () -> { keyReleases.remove(key); nativeKey(key, false); };
        keyReleases.put(key, release);
        postDelayed(release, 160);
    }
    void cancel() {
        gesture.cancel();
        releasePointer();
        nativeCamera(2, 0, 0, 0, 0);
        timeEnd();
        placementCursor=false;centerTouch=false;
        wheelZoom=0;
        mapGestureCounted=mapAllowed=false;
        stopGamepad();
        for (Map.Entry<Integer, Runnable> entry : keyReleases.entrySet()) {
            removeCallbacks(entry.getValue());
            nativeKey(entry.getKey(), false);
        }
        keyReleases.clear();
    }
    void close() { cancel(); removeCallbacks(loadingPoll); if (popup != null) popup.dismiss(); }
    private void timeStart(float x, float y) {
        speedHintKnown();
        int state = mode(x, y);
        if ((state & 256) != 0) timeSpeed = (state >> 9) & 3;
        timeVisible = true;
        timeOrigin = x;
        timeBase = timeSpeed;
        float width = dp(252);
        timeX = Math.max(dp(16), Math.min(getWidth() - width - dp(16), x - width * .5f));
        timeY = Math.min(getHeight() - dp(80), y + dp(42));
        invalidate();
    }
    private void timeMove(float x) {
        int selected = Math.max(0, Math.min(3, timeBase + Math.round((x - timeOrigin) / dp(64))));
        if (selected == timeSpeed) return;
        nativeCamera(4, selected, 0, 0, 0);
        timeSpeed = selected;
        performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);
        invalidate();
    }
    private void timeEnd() { timeVisible = false; invalidate(); }
    @Override protected void onDraw(Canvas canvas) {
        if(loading){
            float left=getWidth()*.3f,right=getWidth()*.7f,y=getHeight()-dp(36);
            paint.setColor(0xdd14232d);canvas.drawRoundRect(left-dp(16),y-dp(38),right+dp(16),y+dp(16),dp(8),dp(8),paint);
            paint.setColor(0xffd7edf6);paint.setTextSize(dp(15));paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(loadingLabel,getWidth()*.5f,y-dp(12),paint);
            paint.setColor(0xff475864);canvas.drawRect(left,y,right,y+dp(4),paint);
            float phase=(android.os.SystemClock.uptimeMillis()%1400)/1400f;
            paint.setColor(0xffffb04c);float start=left+(right-left)*phase;
            canvas.drawRect(start,y,Math.min(right,start+(right-left)*.2f),y+dp(4),paint);
            postInvalidateDelayed(40);return;
        }
        drawSwitchMenu(canvas);
        if(centerMode)drawConstruction(canvas);
        if (placementCursor || gamepadActive) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(4)); paint.setColor(0xcc000000);
            canvas.drawCircle(cursorX,cursorY,dp(9),paint);
            paint.setStrokeWidth(dp(2)); paint.setColor(0xffffd68c);
            canvas.drawCircle(cursorX,cursorY,dp(9),paint);
            canvas.drawLine(cursorX-dp(15),cursorY,cursorX+dp(15),cursorY,paint);
            canvas.drawLine(cursorX,cursorY-dp(15),cursorX,cursorY+dp(15),paint);
            paint.setStyle(Paint.Style.FILL);
        }
        if (!timeVisible) return;
        paint.setColor(0xee14232d);
        canvas.drawRoundRect(timeX - dp(12), timeY - dp(14), timeX + dp(264), timeY + dp(50), dp(10), dp(10), paint);
        paint.setStrokeWidth(dp(2));
        paint.setColor(0xff82909a);
        canvas.drawLine(timeX, timeY, timeX + dp(252), timeY, paint);
        paint.setTextSize(dp(13));
        paint.setTextAlign(Paint.Align.CENTER);
        for (int i = 0; i < 4; i++) {
            float x = timeX + dp(84) * i;
            paint.setColor(i == timeSpeed ? 0xffffb04c : 0xffd7edf6);
            canvas.drawCircle(x, timeY, dp(i == timeSpeed ? 7 : 4), paint);
            float y=timeY+dp(26);
            if(i==0)canvas.drawRect(x-dp(6),y-dp(6),x+dp(6),y+dp(6),paint);
            else for(int arrow=0;arrow<i;++arrow){
                float left=x+dp((arrow-(i-1)*.5f)*10-4);
                Path triangle=new Path();triangle.moveTo(left,y-dp(7));
                triangle.lineTo(left+dp(8),y);triangle.lineTo(left,y+dp(7));triangle.close();
                canvas.drawPath(triangle,paint);
            }
        }
    }
    /* Right edge, under the world-map button and over the population box (scouts fold away meanwhile). */
    private float constructionX(){return getWidth()-dp(46);}
    private float constructionY(int index){
        float top=Math.max(dp(88),getHeight()*.23f);
        return top+index*dp(58)+(index>=CONFIRM?dp(14):0);
    }
    private int constructionButtonAt(float x,float y){
        if(!centerMode)return -1;
        for(int i=0;i<4;i++)if((i!=CONFIRM||painterKind!=0)&&Math.hypot(x-constructionX(),y-constructionY(i))<=dp(29))return i;
        return -1;
    }
    private void drawLabel(Canvas canvas,String text,float x,float y,boolean alignRight){
        if(text==null||text.isEmpty())return;
        paint.setStyle(Paint.Style.FILL);paint.setTextSize(dp(12));paint.setTextAlign(Paint.Align.LEFT);
        float width=paint.measureText(text),left=alignRight?x-width-dp(16):x,top=y-dp(12);
        paint.setColor(0xd914232d);canvas.drawRoundRect(left,top,left+width+dp(16),top+dp(24),dp(6),dp(6),paint);
        paint.setColor(0xffe8eef2);canvas.drawText(text,left+dp(8),y+dp(4.5f),paint);
    }
    private static Path constructionOutline(){
        Path outline=new Path();outline.moveTo(0,-1);
        outline.cubicTo(.25f,-1,.37f,-.84f,.53f,-.79f);
        outline.cubicTo(.71f,-.84f,.88f,-.60f,.86f,-.45f);
        outline.cubicTo(.9f,-.32f,1,-.25f,1,0);
        outline.cubicTo(1,.25f,.9f,.32f,.86f,.45f);
        outline.cubicTo(.88f,.60f,.71f,.84f,.53f,.79f);
        outline.cubicTo(.37f,.84f,.25f,1,0,1);
        outline.cubicTo(-.25f,1,-.37f,.84f,-.53f,.79f);
        outline.cubicTo(-.71f,.84f,-.88f,.60f,-.86f,.45f);
        outline.cubicTo(-.9f,.32f,-1,.25f,-1,0);
        outline.cubicTo(-1,-.25f,-.9f,-.32f,-.86f,-.45f);
        outline.cubicTo(-.88f,-.60f,-.71f,-.84f,-.53f,-.79f);
        outline.cubicTo(-.37f,-.84f,-.25f,-1,0,-1);
        outline.close();return outline;
    }
    private void drawConstruction(Canvas canvas){
        if(constructionProjected&&constructionProjection.setPolyToPoly(CONSTRUCTION_CORNERS,0,constructionQuad,0,4)){
            constructionProjection.postScale(getWidth(),getHeight());
            CONSTRUCTION_OUTLINE.transform(constructionProjection,projectedOutline);
            // Composite after the scene so a building cannot occlude the mark on the ground.
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(4.5f));paint.setColor(0xb8000000);canvas.drawPath(projectedOutline,paint);
            paint.setStrokeWidth(dp(2.3f));paint.setColor(0xfffafcff);canvas.drawPath(projectedOutline,paint);
        }
        for(int i=0;i<4;i++){
            if(i==CONFIRM&&painterKind==0)continue;
            float x=constructionX(),y=constructionY(i);
            boolean active=(i==BUILD_ROAD&&(painterKind==2||painterKind==3))||(i==REMOVE_ROAD&&painterKind==5);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(i==constructionPressed?0xf0526f7c:i==CONFIRM?0xeb27563a:i==CANCEL?0xeb5c2626:0xe614232d);
            canvas.drawCircle(x,y,dp(25),paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(active?3:1.5f));
            paint.setColor(active?0xffffb04c:0xb4d7edf6);canvas.drawCircle(x,y,dp(25),paint);
            drawConstructionIcon(canvas,i,x,y);
            String hint=gameLabel(5+i), status=active?gameLabel(painterKind==3?12:10+i):"";
            drawLabel(canvas,status.isEmpty()?hint:status,x-dp(31),y,true);
            if(active&&!streetDetail.isEmpty()){
                String resources=streetDetail.replace("|_|"," ").replace('\r',' ').replace('\n',' ');
                drawLabel(canvas,resources,x-dp(31),y+dp(25),true);
            }
        }
        paint.setStyle(Paint.Style.FILL);
    }
    private void drawRoadIcon(Canvas canvas,float x,float y,float size){
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeWidth(dp(2.2f));paint.setColor(0xfff2eee4);
        canvas.drawLine(x-size,y+size,x-size*.35f,y-size,paint);canvas.drawLine(x+size,y+size,x+size*.35f,y-size,paint);
        paint.setStrokeWidth(dp(1.8f));
        for(int i=0;i<3;i++){float a=y+size-(i+.25f)*size*.7f;canvas.drawLine(x,a,x,a-size*.35f,paint);}
        paint.setStrokeCap(Paint.Cap.BUTT);
    }
    private void drawConstructionIcon(Canvas canvas,int index,float x,float y){
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeCap(Paint.Cap.ROUND);paint.setColor(0xfff2eee4);
        if(index==BUILD_ROAD)drawRoadIcon(canvas,x,y,dp(11));
        else if(index==REMOVE_ROAD){
            drawRoadIcon(canvas,x-dp(3),y+dp(2),dp(9));
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeWidth(dp(3));paint.setColor(0xffe2574a);
            canvas.drawLine(x+dp(4),y-dp(12),x+dp(13),y-dp(3),paint);canvas.drawLine(x+dp(13),y-dp(12),x+dp(4),y-dp(3),paint);
        } else if(index==CONFIRM){
            paint.setStrokeWidth(dp(3.5f));Path check=new Path();
            check.moveTo(x-dp(11),y+dp(1));check.lineTo(x-dp(3),y+dp(9));check.lineTo(x+dp(12),y-dp(8));canvas.drawPath(check,paint);
        } else {
            paint.setStrokeWidth(dp(3.5f));
            canvas.drawLine(x-dp(9),y-dp(9),x+dp(9),y+dp(9),paint);canvas.drawLine(x+dp(9),y-dp(9),x-dp(9),y+dp(9),paint);
        }
        paint.setStrokeCap(Paint.Cap.BUTT);paint.setStyle(Paint.Style.FILL);
    }
    private boolean closeConstructionMenu(){
        long target=nativeSwipeTarget(.5f,.5f,11);if(target==0)return false;
        pointer(0,Float.intBitsToFloat((int)(target>>>32))*getWidth(),Float.intBitsToFloat((int)target)*getHeight(),0);
        click(1);return true;
    }
    /* Ends the active tool through the game's input system (it leaves the paint mode as the game does);
     * a right click is the fallback when the tool is still active. */
    private boolean toolFromMenu,reopening;private int lastConstructionState;
    private void cancelConstructionTool(){
        final boolean menu=toolFromMenu||(mode(getWidth()*.5f,getHeight()*.5f)&CONSTRUCTION_OPEN)!=0;
        reopening=menu;
        if(!nativeCamera(6,0,0,0,0)){recenter();click(2);return;}
        postDelayed(()->{
            if(painterKind!=0){reopening=false;recenter();click(2);return;}
            if(menu)reopenConstruction();else reopening=false;
        },350);
    }
    /* The Switch layout closes the construction menu when the paint mode ends: open it again through
     * the radial menu (its construction item, target 7), then continue once the menu reports open. */
    private void reopenConstruction(){
        if((mode(centerX(),centerY())&CONSTRUCTION_OPEN)!=0){reopening=false;recenter();return;}
        if((mode(getWidth()*.5f,getHeight()*.5f)&SWITCH_AVAILABLE)==0||!switchMenu(true)){reopening=false;recenter();return;}
        final long started=android.os.SystemClock.uptimeMillis();
        post(new Runnable(){
            private boolean chosen;
            @Override public void run(){
                if(android.os.SystemClock.uptimeMillis()-started>2500){reopening=false;if(!chosen)switchMenu(false);recenter();return;}
                if(!chosen){
                    if(nativeSwipeTarget(.5f,.5f,7)!=0&&switchActivate(4))chosen=true;
                    postDelayed(this,50);return;
                }
                if((mode(getWidth()*.5f,getHeight()*.5f)&CONSTRUCTION_OPEN)!=0){
                    reopening=false;
                    recenter();
                    return;
                }
                postDelayed(this,50);
            }
        });
    }
    private void constructionAction(int index){
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        if(index==BUILD_ROAD){
            if(painterKind==2||painterKind==3)cancelConstructionTool();
            else nativeCamera(9,2,0,0,0);
        } else if(index==REMOVE_ROAD){
            if(painterKind==5)cancelConstructionTool();
            else nativeCamera(9,4,0,0,0);
        }
        else if(index==CONFIRM){if(getWidth()>0){pointer(0,centerX(),centerY(),0);click(1);}}
        else if(painterKind!=0)cancelConstructionTool();
        else if(!closeConstructionMenu())pulse(KeyEvent.KEYCODE_B);
    }
    private boolean constructionTouch(MotionEvent event){
        int action=event.getActionMasked();
        if(action==MotionEvent.ACTION_DOWN){
            constructionPressed=-1;
            int button=constructionButtonAt(event.getX(),event.getY());if(button<0)return false;
            gesture.cancel();releasePointer();constructionPressed=button;invalidate();return true;
        }
        if(constructionPressed<0)return false;
        if(action==MotionEvent.ACTION_UP){
            int button=constructionPressed;constructionPressed=-1;
            if(constructionButtonAt(event.getX(),event.getY())==button)constructionAction(button);
            performClick();
        } else if(action==MotionEvent.ACTION_CANCEL||action==MotionEvent.ACTION_POINTER_DOWN)constructionPressed=-1;
        invalidate();return true;
    }
    private void switchImages(){
        if(switchImagesTried)return;
        switchImagesTried=true;
        try {
            try(java.io.InputStream input=com.frostpunk.staticrecomp.launcher.FrostpunkFiles.open(getContext(),"switch-ui.ring.png")){switchRing=BitmapFactory.decodeStream(input);}
            try(java.io.InputStream input=com.frostpunk.staticrecomp.launcher.FrostpunkFiles.open(getContext(),"switch-ui.halo.png")){switchHalo=BitmapFactory.decodeStream(input);}
            for(int i=0;i<5;i++)try(java.io.InputStream input=com.frostpunk.staticrecomp.launcher.FrostpunkFiles.open(getContext(),"switch-ui.action"+i+".png")){switchIcons[i]=BitmapFactory.decodeStream(input);}
        } catch(java.io.IOException error){android.util.Log.e("FrostpunkControls","Switch UI artwork unavailable",error);}
    }
    private void switchBitmap(Canvas canvas,Bitmap bitmap,float x,float y,float size){
        if(bitmap!=null)canvas.drawBitmap(bitmap,null,new RectF(x-size*.5f,y-size*.5f,x+size*.5f,y+size*.5f),paint);
    }
    private void drawSwitchMenu(Canvas canvas){
        switchState=mode(getWidth()*.5f,getHeight()*.5f);
        if((switchState&SWITCH_AVAILABLE)==0)return;
        switchImages();paint.setStyle(Paint.Style.FILL);paint.setAlpha(255);
        if((switchState&SWITCH_MENU)==0){
            float x=dp(32),y=getHeight()-dp(32);
            paint.setColor(0xe614232d);canvas.drawCircle(x,y,dp(23),paint);
            paint.setColor(Color.WHITE);switchBitmap(canvas,switchRing,x,y,dp(72));
            switchBitmap(canvas,switchIcons[4],x,y,dp(30));return;
        }
        if(switchFan){drawSwitchFan(canvas);postInvalidateDelayed(33);return;}
        canvas.drawColor(0xb0000b12);
        float x=getWidth()*.5f,y=getHeight()*.48f,radius=getHeight()*.29f;
        paint.setColor(Color.WHITE);
        switchBitmap(canvas,switchHalo,x,y,radius*3.2f);
        switchBitmap(canvas,switchRing,x,y,radius*3.2f);
        for(int i=0;i<5;i++){
            long target=nativeSwipeTarget(.5f,.5f,i+3);if(target==0)continue;
            float cx=switchX(i),cy=switchY(i);
            float size=getHeight()*.17f;
            paint.setColor(i==switchSelection?0xee526f7c:0xee14232d);canvas.drawCircle(cx,cy,size*.58f,paint);
            paint.setColor(Color.WHITE);switchBitmap(canvas,switchIcons[i],cx,cy,size);
        }
        if(switchSelection>=0){
            String label=switchLabel(switchSelection);
            paint.setTextSize(dp(12));
            drawLabel(canvas,label,x-(paint.measureText(label)+dp(16))*.5f,y,false);
        }
        postInvalidateDelayed(33);
    }
    /* Radial order: economy, search, laws, technology, construction (switch_button_index). */
    private static String switchLabel(int index){return gameLabel(index==4?0:index+1);}
    private float fanX(int index){return dp(32)+(float)Math.cos(Math.toRadians(-90+index*22.5))*dp(142);}
    private float fanY(int index){return getHeight()-dp(32)+(float)Math.sin(Math.toRadians(-90+index*22.5))*dp(142);}
    private int fanItem(float x,float y){
        int result=-1;float nearest=dp(40);
        for(int i=0;i<5;i++){
            float distance=(float)Math.hypot(x-fanX(i),y-fanY(i));
            if(distance<nearest){nearest=distance;result=i;}
        }
        return result;
    }
    private void drawSwitchFan(Canvas canvas){
        float ox=dp(32),oy=getHeight()-dp(32);
        paint.setStyle(Paint.Style.FILL);paint.setColor(0x9a000b12);canvas.drawCircle(ox,oy,dp(178),paint);
        paint.setColor(0xe614232d);canvas.drawCircle(ox,oy,dp(23),paint);
        paint.setColor(Color.WHITE);switchBitmap(canvas,switchRing,ox,oy,dp(72));switchBitmap(canvas,switchIcons[4],ox,oy,dp(30));
        for(int i=0;i<5;i++){
            float cx=fanX(i),cy=fanY(i);
            paint.setColor(i==switchSelection?0xf0526f7c:0xee14232d);canvas.drawCircle(cx,cy,dp(24),paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(i==switchSelection?2.5f:1.2f));
            paint.setColor(i==switchSelection?0xffffd68c:0x99d7edf6);canvas.drawCircle(cx,cy,dp(24),paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);switchBitmap(canvas,switchIcons[i],cx,cy,dp(38));
        }
        if(switchSelection>=0)drawLabel(canvas,switchLabel(switchSelection),fanX(switchSelection)+dp(32),fanY(switchSelection),false);
    }
    private void openSwitchFan(){
        if(switchFan||!switchMenu(true))return;
        switchFan=true;switchSelection=-1;
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);invalidate();
    }
    private int switchItem(float x,float y){
        int result=-1;float nearest=getHeight()*.1f;
        for(int i=0;i<5;i++){
            long target=nativeSwipeTarget(.5f,.5f,i+3);if(target==0)continue;
            float cx=switchX(i),cy=switchY(i);
            float distance=(float)Math.hypot(x-cx,y-cy);
            if(distance<nearest){nearest=distance;result=i;}
        }
        return result;
    }
    private float switchX(int index){return getWidth()*.5f+(float)Math.cos(-Math.PI/2+index*2*Math.PI/5)*getHeight()*.29f;}
    private float switchY(int index){return getHeight()*.48f+(float)Math.sin(-Math.PI/2+index*2*Math.PI/5)*getHeight()*.29f;}
    private boolean switchChoose(int index){
        if(index>=0&&switchActivate(index))return true;
        switchMenu(false);return false;
    }
    private boolean switchActivate(int index){
        long target=nativeSwipeTarget(.5f,.5f,index+3);if(target==0)return false;
        pointer(0,Float.intBitsToFloat((int)(target>>>32))*getWidth(),Float.intBitsToFloat((int)target)*getHeight(),0);
        click(1);return true;
    }
    private boolean switchMenu(boolean open){
        return getWidth()>0&&getHeight()>0&&nativeCamera(5,open?1:0,0,0,0);
    }
    @Override public void onInitializeAccessibilityNodeInfo(android.view.accessibility.AccessibilityNodeInfo info){
        super.onInitializeAccessibilityNodeInfo(info);
        int state=mode(getWidth()*.5f,getHeight()*.5f);if((state&SWITCH_AVAILABLE)==0)return;
        String language=getResources().getConfiguration().getLocales().get(0).getLanguage();
        String[] labels=language.equals("ru")?new String[]{"Меню города","Экономика","Поиск жителей","Книга законов","Исследования","Строительство"}
            :language.equals("de")?new String[]{"Stadtmenü","Wirtschaft","Bürger suchen","Gesetzbuch","Forschung","Bauen"}
            :language.equals("pl")?new String[]{"Menu miasta","Ekonomia","Szukaj mieszkańców","Księga praw","Badania","Budowa"}
            :new String[]{"City menu","Economy","Find citizens","Book of laws","Research","Construction"};
        int count=(state&SWITCH_MENU)!=0?6:1;
        for(int i=0;i<count;i++)info.addAction(new android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction(SWITCH_ACTION+i,labels[i]));
    }
    @Override public boolean performAccessibilityAction(int action,android.os.Bundle arguments){
        int state=mode(getWidth()*.5f,getHeight()*.5f);
        if((state&SWITCH_AVAILABLE)!=0&&action==SWITCH_ACTION){
            gesture.cancel();releasePointer();stopGamepad();boolean accepted=switchMenu((state&SWITCH_MENU)==0);invalidate();return accepted;
        }
        if((state&(SWITCH_MENU|SWITCH_AVAILABLE))==(SWITCH_MENU|SWITCH_AVAILABLE)&&action>SWITCH_ACTION&&action<=SWITCH_ACTION+5)return switchActivate(action-SWITCH_ACTION-1);
        return super.performAccessibilityAction(action,arguments);
    }
    private boolean switchTouch(MotionEvent event){
        int action=event.getActionMasked();
        float opener=(float)Math.hypot(event.getX()-dp(32),event.getY()-(getHeight()-dp(32)));
        if(action==MotionEvent.ACTION_DOWN){
            switchTouch=false;switchFan=false;removeCallbacks(switchFanHold);switchState=mode(event.getX(),event.getY());
            if((switchState&SWITCH_AVAILABLE)==0)return false;
            boolean open=(switchState&SWITCH_MENU)!=0;
            if(!open&&opener>dp(28))return false;
            gesture.cancel();releasePointer();stopGamepad();switchTouch=true;
            switchSelection=open?switchItem(event.getX(),event.getY()):-2;
            if(!open)postDelayed(switchFanHold,260);
        }
        if(!switchTouch)return false;
        if(action==MotionEvent.ACTION_MOVE){
            // Holding the corner button or sliding off it opens the fan; the finger then picks an action.
            if(switchSelection==-2&&opener>dp(40)){removeCallbacks(switchFanHold);openSwitchFan();}
            if(switchFan)switchSelection=fanItem(event.getX(),event.getY());
            else if(switchSelection!=-2)switchSelection=switchItem(event.getX(),event.getY());
        }
        if(action==MotionEvent.ACTION_UP){
            removeCallbacks(switchFanHold);
            if(switchSelection==-2)switchMenu(true);
            else switchChoose(switchSelection);
            switchTouch=false;switchFan=false;switchSelection=-1;performClick();
        } else if(action==MotionEvent.ACTION_CANCEL||action==MotionEvent.ACTION_POINTER_DOWN){
            removeCallbacks(switchFanHold);switchMenu(false);switchSelection=-1;switchFan=false;switchTouch=false;
        }
        invalidate();return true;
    }
    private boolean mouse(MotionEvent event) {
        if (getWidth() <= 0 || getHeight() <= 0) return true;
        if(!mouseConstruction){mouseConstruction=true;updateCenterMode(mode(centerX(),centerY()));}
        return nativeMouse(event.getActionMasked(), event.getButtonState(), event.getMetaState(),
            event.getX() / getWidth(), event.getY() / getHeight(),
            event.getAxisValue(MotionEvent.AXIS_HSCROLL), event.getAxisValue(MotionEvent.AXIS_VSCROLL));
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if(event.isFromSource(InputDevice.SOURCE_MOUSE))return mouse(event);
        if(event.getActionMasked()==MotionEvent.ACTION_DOWN){
            stopGamepad();mouseConstruction=false;updateCenterMode(mode(centerX(),centerY()));
        }
        return constructionTouch(event)||switchTouch(event)||gesture.touch(event);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (gamepadMotion(event)) return true;
        return event.isFromSource(InputDevice.SOURCE_MOUSE) ? mouse(event) : super.onGenericMotionEvent(event);
    }
    @Override public boolean performClick() { super.performClick(); return true; }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (w != oldw || h != oldh) { cancel(); cursorX = w * .5f; cursorY = h * .5f; }
    }
    private float stickX,stickY,cursorStickX,cursorStickY,triggerZoom;
    private int gamepadId=-1,hatY;
    private long gamepadFrame;
    private boolean gamepadScheduled;
    private final Runnable gamepadTick = new Runnable(){@Override public void run(){
        gamepadScheduled=false;
        long now=android.os.SystemClock.uptimeMillis();
        float dt=gamepadFrame==0 ? 1f/60 : Math.min(.05f,(now-gamepadFrame)/1000f);
        gamepadFrame=now;
        int state=mode(cursorX,cursorY);
        if((state & 2048)==0){
            boolean world=(state&1)!=0&&(state&SWITCH_MENU)==0;
            // A released trigger ends one zoom "gesture" for the world-map gate.
            if(triggerZoom==0)mapGestureCounted=mapAllowed=false;
            if(world){nativeCamera(0,stickX*dt*.6f,stickY*dt*.6f,0,0);zoomWheel(triggerZoom*dt);}
            float cx=world?cursorStickX:stickX,cy=world?cursorStickY:stickY;
            pointer(buttons,cursorX+cx*dp(650)*dt,cursorY+cy*dp(650)*dt,0);
        }
        if(stickX!=0||stickY!=0||cursorStickX!=0||cursorStickY!=0||triggerZoom!=0){
            gamepadScheduled=true;postOnAnimation(this);
        }
    }};
    private void stopGamepad(){
        removeCallbacks(gamepadTick);gamepadScheduled=false;gamepadFrame=0;
        stickX=stickY=cursorStickX=cursorStickY=triggerZoom=0;gamepadActive=false;
    }
    private float axis(float value,InputDevice device,int source,int axis){
        if(!Float.isFinite(value))return 0;
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,source);
        float dead=Math.max(.15f,range==null?0:range.getFlat());
        return Math.abs(value)<=dead?0:Math.copySign(Math.min(1,(Math.abs(value)-dead)/(1-dead)),value);
    }
    boolean gamepadMotion(MotionEvent event){
        if(!event.isFromSource(InputDevice.SOURCE_JOYSTICK))return false;
        gamepadAxes(event.getDeviceId(),event.getSource(),new float[]{event.getAxisValue(MotionEvent.AXIS_X),event.getAxisValue(MotionEvent.AXIS_Y),event.getAxisValue(MotionEvent.AXIS_Z),event.getAxisValue(MotionEvent.AXIS_RZ),event.getAxisValue(MotionEvent.AXIS_LTRIGGER),event.getAxisValue(MotionEvent.AXIS_RTRIGGER),event.getAxisValue(MotionEvent.AXIS_HAT_Y)});
        return true;
    }
    private void noteGamepad(int id){
        if(gamepadId==id)return;
        gamepadId=id;
        android.util.Log.i("FrostpunkControls","GAMEPAD_CONNECTED id="+gamepadId);
        if(getContext() instanceof MainActivity)((MainActivity)getContext()).onGamepadAttached();
    }
    void gamepadAxes(int id,int source,float[] values){
        if(values.length!=7)return;
        noteGamepad(id);
        InputDevice device=InputDevice.getDevice(id);
        stickX=axis(values[0],device,source,MotionEvent.AXIS_X);stickY=axis(values[1],device,source,MotionEvent.AXIS_Y);
        cursorStickX=axis(values[2],device,source,MotionEvent.AXIS_Z);cursorStickY=axis(values[3],device,source,MotionEvent.AXIS_RZ);
        triggerZoom=axis(values[5],device,source,MotionEvent.AXIS_RTRIGGER)-axis(values[4],device,source,MotionEvent.AXIS_LTRIGGER);
        int hat=Math.round(values[6]);
        if(hat!=hatY&&hat!=0){int state=mode(cursorX,cursorY);if((state&256)!=0)nativeCamera(4,Math.max(0,Math.min(3,((state>>9)&3)-hat)),0,0,0);}
        hatY=hat;
        if(!gamepadActive){gamepadActive=true;updateCenterMode(mode(centerX(),centerY()));}
        if(!gamepadScheduled){gamepadScheduled=true;postOnAnimation(gamepadTick);}
    }
    boolean gamepadKey(KeyEvent event){
        if(!event.isFromSource(InputDevice.SOURCE_GAMEPAD) && !event.isFromSource(InputDevice.SOURCE_JOYSTICK))return false;
        boolean down=event.getAction()==KeyEvent.ACTION_DOWN;
        int state=mode(cursorX,cursorY),key=event.getKeyCode();
        android.util.Log.d("FrostpunkControls","GAMEPAD_KEY key="+key+" down="+down+" state="+(state&8191));
        if((state & 2048)!=0)return true;
        if(!gamepadActive){gamepadActive=true;updateCenterMode(mode(centerX(),centerY()));}
        noteGamepad(event.getDeviceId());
        if(key==KeyEvent.KEYCODE_BUTTON_A){
            if(down&&event.getRepeatCount()>0)return true;
            if((state&SWITCH_MENU)!=0){if(down&&event.getRepeatCount()==0)switchChoose(switchItem(cursorX,cursorY));}
            else pointer(down?1:0,cursorX,cursorY,0);
            return true;
        }
        if(!down||event.getRepeatCount()>0)return true;
        switch(key){
            case KeyEvent.KEYCODE_BUTTON_B:if((state&SWITCH_MENU)!=0)switchMenu(false);else if((state&20)!=0)click(2);else pulse(KeyEvent.KEYCODE_ESCAPE);break;
            case KeyEvent.KEYCODE_BUTTON_START:pulse(KeyEvent.KEYCODE_ESCAPE);break;
            case KeyEvent.KEYCODE_BUTTON_X:pulse(KeyEvent.KEYCODE_B);break;
            case KeyEvent.KEYCODE_BUTTON_Y:if((state&SWITCH_AVAILABLE)!=0)switchMenu((state&SWITCH_MENU)==0);else pulse(KeyEvent.KEYCODE_V);break;
            case KeyEvent.KEYCODE_BUTTON_L1:case KeyEvent.KEYCODE_BUTTON_R1:
                if((state&4)!=0)nativeCamera(3,0,0,0,key==KeyEvent.KEYCODE_BUTTON_L1?-.36f:.36f);
                else nativeCamera(0,0,0,0,key==KeyEvent.KEYCODE_BUTTON_L1?-.15f:.15f);break;
            case KeyEvent.KEYCODE_DPAD_UP:case KeyEvent.KEYCODE_DPAD_DOWN:
                if((state&256)!=0)nativeCamera(4,Math.max(0,Math.min(3,((state>>9)&3)+(key==KeyEvent.KEYCODE_DPAD_UP?1:-1))),0,0,0);break;
            default:return false;
        }
        invalidate();return true;
    }
    private final class Gesture {
        private int first = -1, second = -1, maximum, context;
        private boolean multi, moved, drag, held, pan, timeline, building, settingSwipe, settingsScroll, controlDragging, controlEnding;
        private float downX, downY, lastX, lastY, span, angle, turn, offset, settingX, settingY, controlX, controlY;
        private int settingWanted,settingSent;
        private long started;
        private Runnable hold;
        private final Map<Integer, float[]> contactStarts = new HashMap<>();
        boolean active(){return first>=0;}
        void cancel() {
            stopHold();
            first = second = -1;
            maximum = context = 0;
            multi = moved = drag = held = pan = timeline = building = settingSwipe = settingsScroll = controlDragging = false;
            turn = 0;
            contactStarts.clear();
        }
        private void stopHold() {
            if (hold != null) { removeCallbacks(hold); hold = null; }
        }
        private void rebase(MotionEvent event, int skip) {
            first = second = -1;
            for (int i = 0; i < event.getPointerCount(); i++) if (i != skip) {
                if (first < 0) first = event.getPointerId(i);
                else if (second < 0) second = event.getPointerId(i);
            }
            if (second < 0) return;
            int a = event.findPointerIndex(first), b = event.findPointerIndex(second);
            span = (float)Math.hypot(event.getX(b) - event.getX(a), event.getY(b) - event.getY(a));
            angle = (float)Math.atan2(event.getY(b) - event.getY(a), event.getX(b) - event.getX(a));
        }
        private void settingMove(float x) {
            settingWanted=Math.round((x-downX)/dp(32));
            if(pointerRelease!=null||settingWanted==settingSent)return;
            pointerRelease=new Runnable(){
                private int phase,step;
                private long ticket;
                @Override public void run(){
                    if((mode(settingX,settingY)&8192)==0){releasePointer();return;}
                    if(phase!=0&&!nativeClickReady(ticket)){postDelayed(this,16);return;}
                    if(phase==4){
                        if(settingSent==settingWanted){pointerRelease=null;return;}
                        step=settingWanted>settingSent?1:-1;
                        if(nativeSwipeTarget(settingX/getWidth(),settingY/getHeight(),step)==0){settingSent=settingWanted;pointerRelease=null;return;}
                        phase=0;run();return;
                    }
                    if(phase==0){
                        if(settingSent==settingWanted){pointerRelease=null;return;}
                        step=settingWanted>settingSent?1:-1;
                        long target=nativeSwipeTarget(settingX/getWidth(),settingY/getHeight(),step);
                        if(target==0){
                            // Refresh a possible stale boundary snapshot through a completed hover update.
                            pointer(0,settingX,settingY,0);ticket=nativeClickToken();phase=4;postDelayed(this,16);return;
                        }
                        pointer(0,Float.intBitsToFloat((int)(target>>>32))*getWidth(),Float.intBitsToFloat((int)target)*getHeight(),0);
                    } else if(phase==1){
                        if(settingSent==settingWanted){pointerRelease=null;return;}
                        if((settingWanted>settingSent?1:-1)!=step){phase=0;run();return;}
                        // Hover acknowledgement follows the previous render's updated selector limits.
                        if(nativeSwipeTarget(settingX/getWidth(),settingY/getHeight(),step)==0){settingSent=settingWanted;pointerRelease=null;return;}
                        pointer(1,cursorX,cursorY,0);
                    }
                    else if(phase==2)pointer(0,cursorX,cursorY,0);
                    else {settingSent+=step;performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);phase=0;run();return;}
                    ticket=nativeClickToken();phase++;postDelayed(this,16);
                }
            };
            pointerRelease.run();
        }
        private void startSetting(float x) {
            stopHold();settingSwipe=true;moved=true;
            settingX=downX;settingY=downY;settingMove(x);
        }
        private void startControlDrag(int button,float originX,float originY,float x,float y) {
            stopHold();controlDragging=true;moved=true;controlX=x;controlY=y;
            pointer(0,originX,originY,0);
            pointerRelease=new Runnable(){
                private int phase;
                private long ticket=nativeClickToken();
                private float sentX=originX,sentY=originY;
                @Override public void run(){
                    if((mode(downX,downY)&(button==2?16384:64))==0){releasePointer();return;}
                    if(!nativeClickReady(ticket)){postDelayed(this,16);return;}
                    if(phase==0){pointer(button,originX,originY,0);phase=1;}
                    else if(controlX!=sentX||controlY!=sentY){sentX=controlX;sentY=controlY;pointer(button,sentX,sentY,0);}
                    else if(controlEnding&&phase==1){pointer(0,sentX,sentY,0);phase=2;}
                    else if(phase==2){pointerRelease=null;return;}
                    ticket=nativeClickToken();postDelayed(this,16);
                }
            };
            postDelayed(pointerRelease,16);
        }
        private void startSlider(float x) {
            long target=nativeSwipeTarget(downX/getWidth(),downY/getHeight(),0);
            float origin=target==0?downX:Float.intBitsToFloat((int)(target>>>32))*getWidth();
            float y=target==0?downY:Float.intBitsToFloat((int)target)*getHeight();
            startControlDrag(1,origin,y,origin+x-downX,y);
        }
        private void moveSingle(float x, float y) {
            if(centerTouch){
                // Construction: the finger moves the camera; the tool stays on the centre cursor.
                if(!pan&&Math.hypot(x-downX,y-downY)>dp(10)){moved=true;pan=(context&1)!=0;}
                if(pan)camera(x-lastX,y-lastY,0,0);
                lastX=x;lastY=y;return;
            }
            // A sideways swipe along the time bar changes the speed at once (holding it still works).
            if(!timeline&&!held&&(context&8)!=0){
                float dx=Math.abs(x-downX),dy=Math.abs(y-downY);
                if(dx>dp(12)&&dx>1.2f*dy){stopHold();held=true;moved=true;timeline=true;timeStart(downX,downY);}
            }
            if (timeline) { timeMove(x); lastX = x; lastY = y; return; }
            if(settingSwipe){settingMove(x);lastX=x;lastY=y;return;}
            if(controlDragging){if(settingsScroll&&(context&32)==0)controlY+=y-lastY;else controlX+=x-lastX;lastX=x;lastY=y;return;}
            if((context&(8192|16384|64))!=0){
                float dx=Math.abs(x-downX),dy=Math.abs(y-downY);
                if(!settingsScroll&&dx>dp(10)&&dx>1.2f*dy&&(context&(8192|64))!=0){
                    if((context&64)!=0)startSlider(x);else startSetting(x);
                } else if((context&16384)!=0&&(settingsScroll||((context&32)!=0?dx>dp(10)&&dx>1.2f*dy:dy>dp(10)&&dy>1.2f*dx))){
                    settingsScroll=true;
                    startControlDrag(2,downX,downY,(context&32)!=0?x:downX,(context&32)!=0?downY:y);
                }
                lastX=x;lastY=y;return;
            }
            if ((context & 20) != 0 && (context & 2) == 0) {
                if (Math.hypot(x - downX, y - downY) > dp(10)) {
                    moved=true;
                    if((context & 16)!=0 && !drag){stopHold();drag=true;pointer(1,downX,downY-offset,0);}
                    else if((context & 16)==0)stopHold();
                }
                pointer(drag ? 1 : 0, x, y - offset, 0);
                lastX = x; lastY = y; return;
            }
            if (!drag && !pan && !held && Math.hypot(x - downX, y - downY) > dp(10)) {
                stopHold(); moved = true;
                pan = (context & 1) != 0 && (context & 6) == 0;
                if (!pan) { drag = true; pointer((context & 4096) != 0 ? 2 : 1, downX, downY, 0); }
            }
            if (pan) camera(x - lastX, y - lastY, 0, 0);
            else pointer(drag ? ((context & 4096) != 0 ? 2 : 1) : 0, x, held && (context & 2) == 0 ? y - dp(42) : y, 0);
            lastX = x; lastY = y;
        }
        private void moveMulti(MotionEvent event) {
            if (second < 0 || maximum > 2) return;
            int a = event.findPointerIndex(first), b = event.findPointerIndex(second);
            if (a < 0 || b < 0) return;
            float distance = (float)Math.hypot(event.getX(b) - event.getX(a), event.getY(b) - event.getY(a));
            float radians = (float)Math.atan2(event.getY(b) - event.getY(a), event.getX(b) - event.getX(a));
            float delta = radians - angle;
            while (delta > Math.PI) delta -= Math.PI * 2;
            while (delta < -Math.PI) delta += Math.PI * 2;
            float zoom = span > dp(16) && distance > dp(16) ? (float)Math.log(distance / span) : 0;
            if (building) {
                nativeCamera(3, 0, 0, 0, delta);
                if(centerMode)smoothZoom(zoom);
            } else if ((context & 3) == 1) camera(0, 0, zoom, delta);
            span = distance; angle = radians;
        }
        boolean touch(MotionEvent event) {
            int action = event.getActionMasked(), index = event.getActionIndex();
            for (int i = 0; i < event.getPointerCount(); i++)
                if (!Float.isFinite(event.getX(i)) || !Float.isFinite(event.getY(i))) { TouchControls.this.cancel(); return true; }
            if (action != MotionEvent.ACTION_DOWN) for (int i = 0; i < event.getPointerCount(); i++) {
                float[] start = contactStarts.get(event.getPointerId(i));
                if (start != null && Math.hypot(event.getX(i) - start[0], event.getY(i) - start[1]) > dp(10)) moved = true;
            }
            if (action == MotionEvent.ACTION_CANCEL) { TouchControls.this.cancel(); return true; }
            if (action == MotionEvent.ACTION_DOWN) {
                TouchControls.this.cancel();
                first = event.getPointerId(index); maximum = 1;
                started = android.os.SystemClock.uptimeMillis();
                downX = lastX = event.getX(index); downY = lastY = event.getY(index);
                contactStarts.put(first, new float[]{downX, downY});
                context = mode(downX, downY);
                settingWanted=settingSent=0;controlEnding=false;
                offset=dp((context >>> 16)&255);
                centerTouch=centerMode&&(context&2)==0;
                placementCursor=!centerTouch&&(context & 20)!=0 && (context & 2)==0;
                if ((context & 2048) != 0) { cancel(); return true; }
                if(centerTouch)return true;
                pointer(0, downX, placementCursor ? downY-offset : downY, 0);
                hold = () -> {
                    hold = null;
                    if (first >= 0 && !multi && !drag && !pan) {
                        held = true;
                        if ((context & 8) != 0) { timeline = true; timeStart(lastX, lastY); }
                        else if((context&64)!=0)startSlider(lastX);
                        else if((context&8192)!=0)startSetting(lastX);
                        else if((context & 16)!=0 && (context & 2)==0){drag=true;pointer(1,lastX,lastY-offset,0);}
                        // Over the interface the hover goes exactly under the finger: it focuses that item and opens
                        // the tooltip a mouse hover shows (choices, laws, buildings); over the world it stays above it.
                        else pointer(0, lastX, placementCursor ? lastY-offset : (context & 2) != 0 ? lastY : lastY-dp(42), 0);
                        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
                    }
                };
                postDelayed(hold, (context & (8|64|8192))!=0 ? 100 : (context & 16)!=0 ? 160 : 450);
            } else if (first < 0) return true;
            else if (action == MotionEvent.ACTION_POINTER_DOWN) {
                stopHold(); releasePointer();
                maximum = Math.max(maximum, event.getPointerCount());
                contactStarts.put(event.getPointerId(index), new float[]{event.getX(index), event.getY(index)});
                building = (mode(lastX, lastY) & 6) == 4;
                multi = true;
                settingSwipe=controlDragging=settingsScroll=false;
                timeEnd(); timeline = false;
                rebase(event, -1);
            } else if (action == MotionEvent.ACTION_MOVE) {
                if (maximum > 2) {
                    int i = event.findPointerIndex(first);
                    if (i >= 0 && Math.hypot(event.getX(i) - downX, event.getY(i) - downY) > dp(10)) moved = true;
                } else if (multi) moveMulti(event);
                else {
                    int i = event.findPointerIndex(first);
                    if (i < 0) TouchControls.this.cancel();
                    else moveSingle(event.getX(i), event.getY(i));
                }
            } else if (action == MotionEvent.ACTION_POINTER_UP) {
                if (multi) { moveMulti(event); releasePointer(); rebase(event, index); }
            } else if (action == MotionEvent.ACTION_UP) {
                stopHold();
                if(!multi)moveSingle(event.getX(index),event.getY(index));
                if(settingSwipe)settingMove(event.getX(index));
                else if(controlDragging)controlEnding=true;
                else if(settingsScroll)releasePointer();
                else if (maximum == 3 && !moved && android.os.SystemClock.uptimeMillis() - started <= 300)
                    nativeCamera(1, 0, 0, 0, 0);
                else if (multi) { releasePointer(); if (maximum == 2 && !moved) click(2); }
                else if(centerTouch){
                    // With only the menu open a tap still selects in the city; a tool acts on the centre
                    // cursor through the buttons on the right, so a stray tap never places or demolishes.
                    if(!moved&&!pan&&painterKind==0){pointer(0,downX,downY,0);click(1);performClick();}
                }
                else {
                    if (!drag && !held && !pan && !((context & 20) != 0 && moved)) {
                        long target=nativeSwipeTarget(downX/getWidth(),downY/getHeight(),2);
                        if(target!=-1L){
                            if(target!=0)pointer(0,Float.intBitsToFloat((int)(target>>>32))*getWidth(),Float.intBitsToFloat((int)target)*getHeight(),0);
                            click(1);performClick();
                            if((context&32768)==0)hideKeyboard();
                            if((context&8)!=0)postDelayed(TouchControls.this::showSpeedHint,450);
                        }
                    }
                    else {
                        releasePointer();
                        // The original road painter commits on its second click, not on button-up.
                        if(drag&&(context&16)!=0&&(context&2)==0)click(1);
                    }
                }
                timeEnd(); cancel(); placementCursor=false; invalidate();
            }
            return true;
        }
    }
}
