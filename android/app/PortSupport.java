package com.frostpunk.staticrecomp.probe;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.util.AtomicFile;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

final class PortSupport {
    static void requestStorageAccess(Activity activity) {
        if(activity.isFinishing()||activity.isDestroyed())return;
        String text=java.util.Locale.getDefault().getLanguage().equals("ru")
            ?"Разрешите Frostpunk доступ к файлам и запустите игру снова. Сохранения будут доступны после переустановки."
            :"Allow Frostpunk to access files, then start the game again. Your saved games will survive reinstalling the app.";
        android.widget.Toast.makeText(activity,text,android.widget.Toast.LENGTH_LONG).show();
        android.content.Intent intent=new android.content.Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            android.net.Uri.parse("package:"+activity.getPackageName()));
        try {activity.startActivity(intent);}
        catch(android.content.ActivityNotFoundException error) {
            try {activity.startActivity(new android.content.Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));}
            catch(android.content.ActivityNotFoundException missing){Log.w("FrostpunkSupport","FP_STORAGE_PERMISSION settings_unavailable");}
        }
    }
    static void capturePreviousExit(Context context) {
        Context app=context.getApplicationContext();
        new Thread(() -> {
            try {
                ActivityManager manager=(ActivityManager)app.getSystemService(Context.ACTIVITY_SERVICE);
                if(manager==null)return;
                ApplicationExitInfo previous=null;
                String process=Application.getProcessName();
                for(ApplicationExitInfo info:manager.getHistoricalProcessExitReasons(app.getPackageName(),0,64)) {
                    if(process.equals(info.getProcessName())){previous=info;break;}
                }
                JSONObject record=new JSONObject().put("schema",1).put("available",previous!=null);
                if(previous!=null) {
                    record.put("timestamp_ms",previous.getTimestamp()).put("reason",previous.getReason())
                        .put("status",previous.getStatus()).put("importance",previous.getImportance())
                        .put("pss_kb",previous.getPss()).put("rss_kb",previous.getRss());
                    Log.i("FrostpunkSupport","FP_PREVIOUS_EXIT reason="+previous.getReason()+" status="+previous.getStatus());
                }
                AtomicFile file=new AtomicFile(new File(app.getFilesDir(),"last-exit.json"));
                FileOutputStream output=null;
                try {
                    output=file.startWrite();output.write((record.toString()+"\n").getBytes(StandardCharsets.UTF_8));
                    file.finishWrite(output);
                } catch(Exception error){if(output!=null)file.failWrite(output);throw error;}
            } catch(Exception error){Log.w("FrostpunkSupport","FP_PREVIOUS_EXIT unavailable type="+error.getClass().getSimpleName());}
        },"FrostpunkExitInfo").start();
    }

    static AlertDialog showHelp(Activity activity,String title,String body,String close) {
        if(activity.isFinishing()||activity.isDestroyed())return null;
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle(title).setMessage(body)
            .setPositiveButton(close,null).create();
        dialog.show();return dialog;
    }
}
