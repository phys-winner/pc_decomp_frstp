import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.LinkedHashMap;
import java.util.Map;

/** Run with adb's shell UID, like Android's input command; never instrument the game process. */
public final class TouchInjector {
    private final LinkedHashMap<Integer, float[]> fingers = new LinkedHashMap<>();
    private final Object manager;
    private final Method inject;
    private long downTime;

    private TouchInjector() throws Exception {
        Class<?> type;
        try { type = Class.forName("android.hardware.input.InputManagerGlobal"); }
        catch (ClassNotFoundException olderAndroid) { type = Class.forName("android.hardware.input.InputManager"); }
        manager = type.getMethod("getInstance").invoke(null);
        inject = type.getMethod("injectInputEvent", InputEvent.class, int.class);
    }

    private void dispatch(int action) throws Exception {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[fingers.size()];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[fingers.size()];
        int index = 0;
        for (Map.Entry<Integer, float[]> finger : fingers.entrySet()) {
            properties[index] = new MotionEvent.PointerProperties();
            properties[index].id = finger.getKey(); properties[index].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coordinates[index] = new MotionEvent.PointerCoords();
            coordinates[index].x = finger.getValue()[0]; coordinates[index].y = finger.getValue()[1];
            coordinates[index].pressure = 1; coordinates[index].size = .1f; index++;
        }
        MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, fingers.size(),
            properties, coordinates, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try { if (!Boolean.TRUE.equals(inject.invoke(manager, event, 2))) throw new AssertionError("Input injection rejected"); }
        finally { event.recycle(); }
    }

    private void down(int id, float x, float y) throws Exception {
        if (id < 0 || id > 31 || fingers.containsKey(id)) throw new IllegalArgumentException("Invalid pointer ID");
        if (fingers.isEmpty()) downTime = SystemClock.uptimeMillis();
        fingers.put(id, new float[]{x, y});
        dispatch(fingers.size() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | ((fingers.size() - 1) << 8));
    }

    private void up(int id) throws Exception {
        int index = 0;
        for (int pointer : fingers.keySet()) { if (pointer == id) break; index++; }
        if (!fingers.containsKey(id)) throw new IllegalArgumentException("Pointer already up");
        dispatch(fingers.size() == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (index << 8));
        fingers.remove(id);
    }

    private void execute(String line) throws Exception {
        String[] words = line.split("\\s+");
        switch (words[0]) {
        case "wait": SystemClock.sleep(Long.parseLong(words[1])); break;
        case "down": down(Integer.parseInt(words[1]), Float.parseFloat(words[2]), Float.parseFloat(words[3])); break;
        case "up": up(Integer.parseInt(words[1])); break;
        case "move":
            for (int i = 1; i < words.length; i += 3) {
                float[] point = fingers.get(Integer.parseInt(words[i]));
                if (point == null) throw new IllegalArgumentException("Move of absent pointer");
                point[0] = Float.parseFloat(words[i + 1]); point[1] = Float.parseFloat(words[i + 2]);
            }
            dispatch(MotionEvent.ACTION_MOVE); break;
        case "glide": {
            int frames = Math.max(1, Integer.parseInt(words[1]) / 16);
            Map<Integer, float[]> paths = new LinkedHashMap<>();
            for (int i = 2; i < words.length; i += 3) {
                int id = Integer.parseInt(words[i]); float[] start = fingers.get(id);
                if (start == null) throw new IllegalArgumentException("Glide of absent pointer");
                paths.put(id, new float[]{start[0], start[1], Float.parseFloat(words[i + 1]), Float.parseFloat(words[i + 2])});
            }
            for (int frame = 1; frame <= frames; frame++) {
                for (Map.Entry<Integer, float[]> path : paths.entrySet()) {
                    float[] p = path.getValue(), point = fingers.get(path.getKey()); float t = (float)frame / frames;
                    point[0] = p[0] + (p[2] - p[0]) * t; point[1] = p[1] + (p[3] - p[1]) * t;
                }
                dispatch(MotionEvent.ACTION_MOVE); SystemClock.sleep(16);
            }
            break;
        }
        case "tap": down(0, Float.parseFloat(words[1]), Float.parseFloat(words[2])); SystemClock.sleep(65); up(0); SystemClock.sleep(250); break;
        default: throw new IllegalArgumentException("Unknown command " + words[0]);
        }
        System.out.println(line);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Expected script path");
        TouchInjector runner = new TouchInjector();
        try {
            for (String line : Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8))
                if (!line.trim().isEmpty()) runner.execute(line.trim());
            if (!runner.fingers.isEmpty()) throw new AssertionError("Script left fingers down");
            System.out.println("PASS");
        } finally {
            if (!runner.fingers.isEmpty()) runner.dispatch(MotionEvent.ACTION_CANCEL);
        }
    }
}
