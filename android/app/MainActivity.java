package com.frostpunk.staticrecomp.probe;
import android.app.NativeActivity;
import android.media.AudioManager;
import android.os.Bundle;
import android.os.Build;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.PointerIcon;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.window.OnBackInvokedDispatcher;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/** Visible diagnostics for the isolated native window probe, not a game UI. */
public final class MainActivity extends NativeActivity {
    private TextView status;
    private TouchControls controls;
    private android.app.AlertDialog gestureHelp;
    private android.app.AlertDialog startupFailure;
    private String startupError;
    private volatile java.io.File activeProfile;
    private volatile String pending="Frostpunk native window probe\nStarting native Vulkan/window fixture";
    private final AtomicLong pointerSequence=new AtomicLong();
    @Override public void onCreate(Bundle state) {
        try { CrashReport.configureLogs(getFilesDir(), getIntent().getBooleanExtra("crash.watched", false)
                && !getIntent().getBooleanExtra("crash.rendererOnly", false)); }
        catch (Exception error) { Log.w("Frostpunk", "Cannot configure logs", error); }
        allowRotation();
        installCrashHandler();
        setResult(RESULT_OK);
        super.onCreate(state);
        allowRotation();
        PortSupport.capturePreviousExit(this);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        status=new TextView(this);
        status.setTextColor(0xffffffff);
        status.setBackgroundColor(0x99101820);
        status.setTextSize(13);
        status.setPadding(20,10,20,10);
        status.setText(pending);
        FrameLayout.LayoutParams layout=new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,FrameLayout.LayoutParams.WRAP_CONTENT,Gravity.TOP);
        addContentView(status,layout);
        if(getPackageName().equals("com.frostpunk.staticrecomp")) {
            controls=new TouchControls(this);
            controls.attach(this);
            if(Build.VERSION.SDK_INT>=33)
                getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,this::onBackPressed);
        }
    }
    @Override protected void onPause() {
        if(controls!=null){controls.cancel();controls.hideKeyboard();}
        super.onPause();
    }
    @Override public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        return controls!=null&&controls.gamepadKey(event) || super.dispatchKeyEvent(event);
    }
    @Override public boolean dispatchGenericMotionEvent(android.view.MotionEvent event) {
        return controls!=null&&controls.gamepadMotion(event) || super.dispatchGenericMotionEvent(event);
    }
    /** Allow both landscape directions, including with a telescopic gamepad attached. */
    public void onGamepadAttached() { allowRotation(); }
    private void allowRotation() {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }
    private void installCrashHandler() {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                java.io.StringWriter text = new java.io.StringWriter();
                error.printStackTrace(new java.io.PrintWriter(text));
                java.io.File file = new java.io.File(getFilesDir(), "frostpunk-crash.txt");
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(file)) {
                    out.write(("Java " + thread.getName() + "\n" + text).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
            } catch (Throwable ignored) { }
            if (previous != null) previous.uncaughtException(thread, error);
        });
    }
    public void onNativeGamepad(int action,int key,int device,int source,float[] axes) {
        runOnUiThread(() -> {
            if(controls==null)return;
            if(key>=0){long now=android.os.SystemClock.uptimeMillis();
                controls.gamepadKey(new android.view.KeyEvent(now,now,action,key,0,0,device,0,0,source));
            } else if(axes!=null)controls.gamepadAxes(device,source,axes);
        });
    }
    @Override protected void onDestroy() {
        if(startupFailure!=null)startupFailure.dismiss();
        if(gestureHelp!=null)gestureHelp.dismiss();
        if(controls!=null)controls.close();
        super.onDestroy();
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        if(!focused && controls!=null)controls.cancel();
        super.onWindowFocusChanged(focused);
        if(focused)immersive();
    }
    private void immersive() {
        getWindow().getDecorView().setForceDarkAllowed(false);
        getWindow().getDecorView().setBackgroundColor(0xff000000);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        if(Build.VERSION.SDK_INT>=30){
            android.view.WindowInsetsController bars=getWindow().getInsetsController();
            if(bars!=null){bars.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);bars.hide(android.view.WindowInsets.Type.systemBars());}
            WindowManager.LayoutParams params=getWindow().getAttributes();
            params.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            getWindow().setAttributes(params);
        }
    }
    private static int[] frameRates(android.view.Display.Mode current,android.view.Display.Mode[] modes){
        java.util.TreeSet<Integer> rates=new java.util.TreeSet<>();
        for(android.view.Display.Mode mode:modes){
            int rate=Math.round(mode.getRefreshRate());
            if(mode.getPhysicalWidth()==current.getPhysicalWidth()&&mode.getPhysicalHeight()==current.getPhysicalHeight()&&rate>0&&rate<=1000)rates.add(rate);
        }
        int active=Math.round(current.getRefreshRate());
        if(active>0&&active<=1000)rates.add(active);
        int[] result=new int[rates.size()+1];result[0]=active;int at=1;
        for(int rate:rates)result[at++]=rate;
        return result;
    }
    public int[] getFrameRates(){
        android.view.Display display=getDisplay();
        if(display==null)display=getWindowManager().getDefaultDisplay();
        return frameRates(display.getMode(),display.getSupportedModes());
    }
    public void showGestureHelp(String title,String body,String close) {
        runOnUiThread(() -> {
            if(controls!=null){controls.cancel();controls.hideKeyboard();controls.setContentDescription(body);}
            if(gestureHelp!=null)gestureHelp.dismiss();
            gestureHelp=PortSupport.showHelp(this,title,body,close);
        });
    }
    public void localizeTouchControls(String loading,String description){
        runOnUiThread(() -> {if(controls!=null)controls.localize(loading,description);});
    }
    public String prepareNativeProfile() throws java.io.IOException {
        String requested=getIntent().getStringExtra("userdata");
        if(requested==null)requested=android.system.Os.getenv("PORT_USERDATA");
        requested=PortProfile.leaf(requested);
        if(!android.os.Environment.isExternalStorageManager()) {
            runOnUiThread(() -> PortSupport.requestStorageAccess(this));
            throw new java.io.IOException("Allow file access for Frostpunk, then start the game again to use persistent saved games.");
        }
        activeProfile=PortProfile.prepare(android.os.Environment.getExternalStorageDirectory(),new java.io.File(getFilesDir(),"profile"),requested);
        Log.i("FrostpunkSupport","FP_PROFILE_READY persistent=1");return activeProfile.getPath();
    }
    @Override public void onBackPressed() {
        if(controls!=null&&controls.hideKeyboard())return;
        if(controls!=null) { controls.cancel();controls.pulse(android.view.KeyEvent.KEYCODE_ESCAPE); }
        else super.onBackPressed();
    }
    public void showNativeStatus(final String text) {
        if(text.startsWith("\u0005")) {
            final String message=text.substring(1);
            runOnUiThread(() -> {
                if(isFinishing()||isDestroyed()||message.equals(startupError))return;
                startupError=message;
                if(message.startsWith("PanVK: ")&&getIntent().getBooleanExtra("crash.watched",false)){
                    setResult(RESULT_CANCELED,new android.content.Intent().putExtra("crash.exitCode",78)
                        .putExtra("crash.startupError",message));
                    finish();
                    return;
                }
                if(controls!=null)controls.failed();
                if(status!=null){status.setText(message);status.setVisibility(View.VISIBLE);}
                if(startupFailure!=null)startupFailure.dismiss();
                boolean russian=java.util.Locale.getDefault().getLanguage().equals("ru");
                startupFailure=new android.app.AlertDialog.Builder(this)
                    .setTitle(russian?"Не удалось запустить игру":"Game could not start")
                    .setMessage(message)
                    .setPositiveButton(russian?"Открыть лаунчер":"Open launcher",(dialog,which) -> {
                        android.content.Intent launcher=getPackageManager().getLaunchIntentForPackage(getPackageName());
                        if(launcher!=null){startActivity(launcher);finish();}
                    }).setNegativeButton(russian?"Закрыть":"Close",null).show();
            });
            return;
        }
        if(text.equals("\u0004")){runOnUiThread(() -> {if(controls!=null){controls.cancel();controls.hideKeyboard();}});return;}
        if(text.startsWith("\u0003")){exportPhoto(text.substring(1));return;}
        pending=text;
        runOnUiThread(new Runnable(){@Override public void run(){if(status!=null)status.setText(text);}});
    }
    public boolean keepNativeInputFocus(){return controls!=null&&controls.textInputActive();}
    public String exportPhoto(String path) {
            android.net.Uri image=null;
            try {
                java.io.File source=new java.io.File(path).getCanonicalFile();
                java.io.File profile=activeProfile;
                if(profile==null)throw new java.io.IOException("Game profile is not ready");
                java.io.File folder=new java.io.File(profile,"Pictures/Frostpunk").getCanonicalFile();
                if(!folder.equals(source.getParentFile())||!source.getName().endsWith(".png"))throw new java.io.IOException("Invalid game photo path");
                android.content.ContentValues values=new android.content.ContentValues();
                values.put(android.provider.MediaStore.Images.Media.DISPLAY_NAME,source.getName());
                values.put(android.provider.MediaStore.Images.Media.MIME_TYPE,"image/png");
                values.put(android.provider.MediaStore.Images.Media.RELATIVE_PATH,"Pictures/Frostpunk");
                values.put(android.provider.MediaStore.Images.Media.IS_PENDING,1);
                image=getContentResolver().insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI,values);
                if(image==null)throw new java.io.IOException("Cannot create gallery photo");
                try(java.io.InputStream in=new java.io.FileInputStream(source);java.io.OutputStream out=getContentResolver().openOutputStream(image)){
                    if(out==null)throw new java.io.IOException("Cannot open gallery photo");
                    byte[] buffer=new byte[65536];int count;
                    while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
                }
                values.clear();values.put(android.provider.MediaStore.Images.Media.IS_PENDING,0);
                if(getContentResolver().update(image,values,null,null)!=1)throw new java.io.IOException("Cannot publish gallery photo");
                String saved=null;
                try(android.database.Cursor row=getContentResolver().query(image,new String[]{android.provider.MediaStore.Images.Media.DATA},null,null,null)){
                    if(row!=null&&row.moveToFirst())saved=row.getString(0);
                }
                if(saved==null||saved.isEmpty())throw new java.io.IOException("Cannot resolve gallery photo path");
                Log.i("FrostpunkPhoto","PHOTO_EXPORTED "+image);
                final String visiblePath=saved;
                runOnUiThread(() -> android.widget.Toast.makeText(this,visiblePath,android.widget.Toast.LENGTH_LONG).show());
                return saved;
            } catch(java.io.IOException|RuntimeException error){
                if(image!=null)try{getContentResolver().delete(image,null,null);}catch(RuntimeException ignored){}
                Log.e("FrostpunkPhoto","Gallery export failed; original photo remains in the game profile",error);
                runOnUiThread(() -> android.widget.Toast.makeText(this,path,android.widget.Toast.LENGTH_LONG).show());
                return null;
            }
    }
    /** Return request acceptance; the applied log records actual UI-thread work. */
    public boolean showPointer(final boolean visible) {
        if(isFinishing()||isDestroyed()) {
            Log.e("FrostpunkProbe","ANDROID_POINTER_FAILED stage=activity_stopped accepted=0");
            return false;
        }
        final long sequence=pointerSequence.incrementAndGet();
        final int type=visible?PointerIcon.TYPE_ARROW:PointerIcon.TYPE_NULL;
        final AtomicBoolean accepted=new AtomicBoolean(true);
        Log.i("FrostpunkProbe","ANDROID_POINTER_REQUEST sequence="+sequence+" visible="+(visible?1:0)+" type="+type);
        try {
            runOnUiThread(new Runnable(){@Override public void run(){
                if(sequence!=pointerSequence.get()) {
                    Log.i("FrostpunkProbe","ANDROID_POINTER_SUPERSEDED sequence="+sequence+" type="+type);
                    return;
                }
                try {
                    if(isFinishing()||isDestroyed())throw new IllegalStateException("Activity stopped before pointer apply");
                    View decor=getWindow().getDecorView();
                    PointerIcon icon=PointerIcon.getSystemIcon(MainActivity.this,type);
                    if(icon==null)throw new IllegalStateException("System pointer icon is NULL");
                    decor.setPointerIcon(icon);
                    if(decor.getPointerIcon()!=icon)throw new IllegalStateException("View pointer-icon readback differs");
                    Log.i("FrostpunkProbe","ANDROID_POINTER_APPLIED sequence="+sequence+" visible="+(visible?1:0)+" type="+type+" same_object=1");
                } catch(RuntimeException failure) {
                    accepted.set(false);
                    Log.e("FrostpunkProbe","ANDROID_POINTER_FAILED stage=apply sequence="+sequence+" type="+type,failure);
                }
            }});
        } catch(RuntimeException failure) {
            Log.e("FrostpunkProbe","ANDROID_POINTER_FAILED stage=enqueue sequence="+sequence+" type="+type,failure);
            return false;
        }
        return accepted.get();
    }
}
