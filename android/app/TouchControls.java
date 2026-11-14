package com.frostpunk.staticrecomp.probe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.RectF;
import android.graphics.Color;
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
import java.util.HashMap;
import java.util.Map;

/** Analog camera deltas are consumed by the original camera controller on its game thread. */
final class TouchControls extends View {
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
    private final Gesture gesture = new Gesture();
    private final Map<Integer, Runnable> keyReleases = new HashMap<>();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final android.content.SharedPreferences preferences;
    private static String loadingLabel="",controlsDescription="";
    private Runnable pointerRelease;
    private int buttons, timeSpeed = 1, timeBase;
    private float cursorX, cursorY, timeX, timeY, timeOrigin, wheelZoom;
    private boolean timeVisible, placementCursor, gamepadActive, loading=true, hasFrame, speedHintSeen, startupFailed;
    private long speedHintReadyAt;
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
    void presented(){hasFrame=true;}
    void failed(){startupFailed=true;loading=false;removeCallbacks(loadingPoll);cancel();hideKeyboard();if(popup!=null)popup.dismiss();}
    private void updateLoading(){
        int state=mode(getWidth()*.5f,getHeight()*.5f);
        if(loading&&hasFrame&&(state&2048)==0){loading=false;invalidate();}
        if(!loading&&(state&(1|2|256|2048))==(1|256)){
            long timestamp=android.os.SystemClock.uptimeMillis();
            if(speedHintReadyAt==0)speedHintReadyAt=timestamp+5000;
            else if(timestamp>=speedHintReadyAt)showSpeedHint();
        }else speedHintReadyAt=0;
    }
    private final Runnable loadingPoll=new Runnable(){@Override public void run(){
        if(gamepadActive&&gamepadId>=0&&InputDevice.getDevice(gamepadId)==null){
            cancel();gamepadId=-1;invalidate();
        }
        updateLoading();
        int next=mode(getWidth()*.5f,getHeight()*.5f);
        if(((next^switchAnnouncedState)&(SWITCH_MENU|SWITCH_AVAILABLE))!=0){
            invalidate();sendAccessibilityEvent(android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
        }
        switchState=switchAnnouncedState=next;
        postDelayed(this,200);
    }};
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
        loadingLabel=loading;controlsDescription=description;setContentDescription(description);invalidate();
        preferences.edit().putString("loading",loading).apply();
    }
    private void showSpeedHint(){
        if(speedHintSeen||controlsDescription.isEmpty())return;
        // shortcut: speed help is paragraph five of GestureHelpText; use its own key if that format changes.
        String[] paragraphs=controlsDescription.split("\n\n");
        if(paragraphs.length<=4)return;
        android.widget.Toast.makeText(getContext(),paragraphs[4],android.widget.Toast.LENGTH_LONG).show();
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
        zoomWheel(zoom);
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
            // The original wheel command enforces scenario zoom levels and the distant-view gate.
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
        placementCursor=false;
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
        postInvalidateDelayed(33);
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
    private boolean switchChoose(int index){return index>=0?switchActivate(index):switchMenu(false);}
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
        if(action==MotionEvent.ACTION_DOWN){
            switchTouch=false;switchState=mode(event.getX(),event.getY());
            if((switchState&SWITCH_AVAILABLE)==0)return false;
            boolean open=(switchState&SWITCH_MENU)!=0;
            if(!open&&Math.hypot(event.getX()-dp(32),event.getY()-(getHeight()-dp(32)))>dp(28))return false;
            gesture.cancel();releasePointer();stopGamepad();switchTouch=true;
            switchSelection=open?switchItem(event.getX(),event.getY()):-2;
        }
        if(!switchTouch)return false;
        if(action==MotionEvent.ACTION_MOVE&&switchSelection!=-2)switchSelection=switchItem(event.getX(),event.getY());
        if(action==MotionEvent.ACTION_UP){
            if(switchSelection==-2)switchMenu(true);
            else switchChoose(switchSelection);
            switchTouch=false;switchSelection=-1;performClick();
        } else if(action==MotionEvent.ACTION_CANCEL||action==MotionEvent.ACTION_POINTER_DOWN){
            switchMenu(false);switchSelection=-1;
        }
        invalidate();return true;
    }
    private boolean mouse(MotionEvent event) {
        if (getWidth() <= 0 || getHeight() <= 0) return true;
        return nativeMouse(event.getActionMasked(), event.getButtonState(), event.getMetaState(),
            event.getX() / getWidth(), event.getY() / getHeight(),
            event.getAxisValue(MotionEvent.AXIS_HSCROLL), event.getAxisValue(MotionEvent.AXIS_VSCROLL));
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        return event.isFromSource(InputDevice.SOURCE_MOUSE) ? mouse(event) : switchTouch(event)||gesture.touch(event);
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
    void gamepadAxes(int id,int source,float[] values){
        if(values.length!=7)return;
        if(gamepadId!=id){
            gamepadId=id;
            android.util.Log.i("FrostpunkControls","GAMEPAD_CONNECTED id="+gamepadId+" source="+source);
        }
        InputDevice device=InputDevice.getDevice(id);
        stickX=axis(values[0],device,source,MotionEvent.AXIS_X);stickY=axis(values[1],device,source,MotionEvent.AXIS_Y);
        cursorStickX=axis(values[2],device,source,MotionEvent.AXIS_Z);cursorStickY=axis(values[3],device,source,MotionEvent.AXIS_RZ);
        triggerZoom=axis(values[5],device,source,MotionEvent.AXIS_RTRIGGER)-axis(values[4],device,source,MotionEvent.AXIS_LTRIGGER);
        int hat=Math.round(values[6]);
        if(hat!=hatY&&hat!=0){int state=mode(cursorX,cursorY);if((state&256)!=0)nativeCamera(4,Math.max(0,Math.min(3,((state>>9)&3)-hat)),0,0,0);}
        hatY=hat;
        gamepadActive=true;
        if(!gamepadScheduled){gamepadScheduled=true;postOnAnimation(gamepadTick);}
    }
    boolean gamepadKey(KeyEvent event){
        if(!event.isFromSource(InputDevice.SOURCE_GAMEPAD) && !event.isFromSource(InputDevice.SOURCE_JOYSTICK))return false;
        boolean down=event.getAction()==KeyEvent.ACTION_DOWN;
        int state=mode(cursorX,cursorY),key=event.getKeyCode();
        android.util.Log.d("FrostpunkControls","GAMEPAD_KEY key="+key+" down="+down+" state="+(state&8191));
        if((state & 2048)!=0)return true;
        gamepadActive=true;
        gamepadId=event.getDeviceId();
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
                placementCursor=(context & 20)!=0 && (context & 2)==0;
                if ((context & 2048) != 0) { cancel(); return true; }
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
                else {
                    if (!drag && !held && !pan && !((context & 20) != 0 && moved)) {
                        long target=nativeSwipeTarget(downX/getWidth(),downY/getHeight(),2);
                        if(target!=-1L){
                            if(target!=0)pointer(0,Float.intBitsToFloat((int)(target>>>32))*getWidth(),Float.intBitsToFloat((int)target)*getHeight(),0);
                            click(1);performClick();
                            if((context&32768)==0)hideKeyboard();
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
