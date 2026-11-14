package com.frostpunk.staticrecomp.launcher;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.os.SystemClock;
import java.util.concurrent.ThreadLocalRandom;

final class FrostpunkSnow {
    private final float[] x = new float[150], y = new float[150],
        vx = new float[150], vy = new float[150], size = new float[150];
    private final Path flakes = new Path();
    private int width, height;
    private float density;
    private long lastFrame = -1;

    void draw(Canvas canvas, Paint paint, int width, int height,
              float density, boolean motion, int colour) {
        if (this.width != width || this.height != height || this.density != density) {
            this.width = width;
            this.height = height;
            this.density = density;
            for (int i = 0; i < x.length; ++i) reset(i, true, 0);
            lastFrame = -1;
        }
        long now = SystemClock.uptimeMillis();
        long elapsed = lastFrame < 0 ? 0 : now - lastFrame;
        // Resume in place instead of catching up time spent outside the launcher.
        float seconds = motion && elapsed > 0 && elapsed <= 100 ? elapsed / 1000f : 0;
        float gust = gust(seconds > 0 ? (now + lastFrame) / 2000.0 : now / 1000.0);
        lastFrame = motion ? now : -1;
        float step = seconds * density * 15;
        paint.setColor(colour);
        paint.setAlpha(77);
        flakes.rewind();
        for (int i = 0; i < x.length; ++i) {
            x[i] += (vx[i] + gust) * step;
            y[i] += vy[i] * step;
            float radius = size[i] * density;
            if (x[i] < -radius || x[i] > width + radius || y[i] > height + radius)
                reset(i, false, gust);
            flakes.addCircle(x[i], y[i], size[i] * density, Path.Direction.CW);
        }
        canvas.drawPath(flakes, paint);
    }

    // The supplied website's tld/Blizzard gust, with speeds scaled for a short button.
    static float gust(double time) {
        double storm = Math.sin(time * 0.5);
        return (float) (Math.sin(time * 2) * 0.75 + storm * 0.5
            + Math.sin(time * 3.5) * 0.25 + Math.max(0, storm - 0.5) * 8);
    }

    private void reset(int i, boolean initial, float gust) {
        ThreadLocalRandom random = ThreadLocalRandom.current();
        vx[i] = random.nextFloat() * 2 + 1;
        vy[i] = random.nextFloat() * 2.5f + 1.5f;
        size[i] = random.nextFloat() * 1.5f + 0.5f;
        x[i] = random.nextFloat() * width;
        y[i] = random.nextFloat() * height;
        if (initial) return;
        float horizontal = vx[i] + gust;
        float top = width * vy[i], side = height * Math.abs(horizontal);
        // Weight incoming edges by their flow so a wide button stays evenly filled.
        if (random.nextFloat() * (top + side) < top) y[i] = -size[i] * density;
        else x[i] = horizontal >= 0 ? -size[i] * density : width + size[i] * density;
    }
}
