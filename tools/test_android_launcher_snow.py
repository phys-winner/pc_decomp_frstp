"""Run Frostpunk's actual button and snow drawing with the shared recording Canvas."""
import importlib.util
import json
import os
from pathlib import Path

import android_launcher
import fastbuild as build


def main():
    root = android_launcher.ROOT
    launcher = Path(os.environ.get('PORT_LAUNCHER', root.parent / 'launcher'))
    spec = importlib.util.spec_from_file_location('play_button_checks', launcher / 'tests/test_play_button.py')
    checks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checks)
    source = android_launcher.snow_java((launcher / 'android/src/port/launcher/Ui.java').read_text(encoding='utf-8'))
    signatures = ['static int parse(', 'static float waterLevel(',
                  'TextView button(String value, int kind, Runnable action)',
                  'TextView button(String value, int kind, JSONObject theme, Runnable action)',
                  'void attention(', 'private final class ActionButton']
    methods = next(line for line in source.splitlines() if line.startswith('    private static int colour('))
    methods += '\n' + '\n'.join(checks.extract(source, signature) for signature in signatures)
    methods = methods.replace('android.content.Context.SENSOR_SERVICE', 'Context.SENSOR_SERVICE')
    methods = methods.replace('activity.getResources().getDisplayMetrics().density', '1f')
    snow_source = root / 'android/launcher/FrostpunkSnow.java'
    snow = '\n'.join(line for line in snow_source.read_text(encoding='utf-8').splitlines() if not line.startswith(('package ', 'import ')))
    snow = snow.replace('ThreadLocalRandom', 'java.util.concurrent.ThreadLocalRandom')
    checks.STUBS = checks.STUBS.replace('frames,invalidations;', 'frames,invalidations,syncFrames;')
    checks.STUBS = checks.STUBS.replace('void invalidate(){', 'void postInvalidateOnAnimation(){frames++;syncFrames++;}void invalidate(){')
    checks.STUBS = checks.STUBS.replace('class Path {', '''class Path {
        final List<float[]> circles=new ArrayList<>();void rewind(){circles.clear();}
        void addCircle(float x,float y,float r,Direction direction){circles.add(new float[]{x,y,r});}''')
    checks.STUBS = checks.STUBS.replace('void drawPath(Path path,Paint paint){assert clipped;}',
        'void drawPath(Path path,Paint paint){assert clipped;for(float[] p:path.circles)drawCircle(p[0],p[1],p[2],paint);}')
    checks.STUBS = checks.STUBS.replace('assert x>=0&&x<=path.width&&y>=0&&y<=path.height;',
        'assert x>=-r&&x<=path.width+r&&y>=-r&&y<=path.height+r;')
    body = r'''
class Ui {
    final Context activity=new Context();final Object bold=new Object();
    static final int PRIMARY=0,SECONDARY=1,QUIET=2;
    final int accent=0xff9bd9ed,accentDark=0xff568da8,primaryFill=0xff142d3d,text=0xfff2f4f7,muted=0xffa7afbc,cell=0xff262a33;
    final int playGlow=0xff9bd9ed,playParticles=0xffa0dcf0;
    final String playParticleStyle="snow";
    int dp(float v){return Math.round(v);}
''' + methods + '\n}\n' + snow + r'''
public class ButtonEffectCheck {
    static Canvas draw(TextView button){button.frames=button.syncFrames=0;Canvas c=new Canvas();button.onDraw(c);assert c.textLast;return c;}
    static Canvas drawSnow(FrostpunkSnow snow){
        Canvas c=new Canvas();Path clip=new Path();clip.addRoundRect(0,0,720,200,14,14,Path.Direction.CW);
        c.clipPath(clip);snow.draw(c,new Paint(1),720,200,1,true,0xffa0dcf0);return c;
    }
    static float[] values(FrostpunkSnow snow,String name)throws Exception{
        java.lang.reflect.Field f=FrostpunkSnow.class.getDeclaredField(name);f.setAccessible(true);return (float[])f.get(snow);
    }
    static Canvas simulate(int fps)throws Exception{
        FrostpunkSnow snow=new FrostpunkSnow();SystemClock.now=1000;drawSnow(snow);
        for(String name:new String[]{"x","y","vx","vy","size"})
            Arrays.fill(values(snow,name),name.equals("x")?360:name.equals("y")?50:name.equals("size")?1:2);
        Canvas last=null;
        for(int i=1;i<=fps;++i){SystemClock.now=1000+Math.round(i*1000.0/fps);last=drawSnow(snow);}
        return last;
    }
    public static void main(String[] args)throws Exception{
        assert FrostpunkSnow.gust(0)==0&&Math.abs(FrostpunkSnow.gust(Math.PI)-4.25f)<.0001f;
        Canvas thirty=simulate(30),sixty=simulate(60),fast=simulate(120);
        for(Canvas other:new Canvas[]{thirty,fast})for(int i=0;i<150;++i){
            assert Math.abs(other.points.get(i)[0]-sixty.points.get(i)[0])<.05f : "wind speed is independent of frame rate";
            assert Math.abs(other.points.get(i)[1]-sixty.points.get(i)[1])<.001f;
        }
        Ui ui=new Ui();int[] clicks={0};
        TextView b=ui.button("Play",Ui.PRIMARY,()->clicks[0]++);ui.attention(b,true);
        for(int[] size:new int[][]{{48,32},{240,60},{720,54}}){
            b.resize(size[0],size[1]);
            for(long time=0;time<7200;time+=17){
                SystemClock.now=time;Canvas c=draw(b);
                assert c.circles==150&&c.lines==0&&c.sheens==0&&b.frames==1&&b.syncFrames==1;
                assert c.particle==0xa0dcf0&&c.glow==0x9bd9ed;
            }
        }
        SystemClock.now=7300;String positions=draw(b).positions;
        SystemClock.now=7317;assert !draw(b).positions.equals(positions);
        ValueAnimator.enabled=false;String frozen=draw(b).positions;assert b.frames==0;
        SystemClock.now=9000;assert draw(b).positions.equals(frozen)&&b.frames==0;
        ValueAnimator.enabled=true;assert draw(b).positions.equals(frozen) : "resume has no jump";
        SystemClock.now=9017;assert !draw(b).positions.equals(frozen);
        ValueAnimator.enabled=true;b.enabled=false;assert draw(b).circles==0&&b.frames==0;b.enabled=true;
        b.onWindowFocusChanged(false);draw(b);assert b.frames==0;
        b.onWindowFocusChanged(true);draw(b);assert b.frames==1;
        b.onWindowVisibilityChanged(8);draw(b);assert b.frames==0;b.onWindowVisibilityChanged(0);
        b.shown=false;draw(b);assert b.frames==0;b.shown=true;
        b.clickListener.click(b);assert clicks[0]==1;
        ui.attention(b,false);assert draw(b).circles==0;
        TextView other=ui.button("Other",Ui.SECONDARY,()->{});assert draw(other).circles==0&&other.frames==0;
        JSONObject theme=new JSONObject().putColour("playParticleStyle","magic");
        TextView magic=ui.button("Magic",Ui.PRIMARY,theme,()->{});magic.resize(240,60);ui.attention(magic,true);
        assert draw(magic).circles==6 : "other particle styles retain their renderer";
        System.out.println("PASS website snow: 150 circles, original gust, frame-independent speed at 30/60/120 FPS, vsync, 3 sizes, clipping, text, pause/resume, click and other styles");
    }
}
'''
    theme = json.loads((root / 'android/port.json').read_text(encoding='utf-8'))['theme']
    assert theme['playParticleStyle'] == 'snow'
    drawing = snow[snow.index('    void draw('):snow.index('    static float gust(')]
    assert 'new ' not in drawing
    os.environ['JAVA_HOME'] = str(build.JAVA.parent.parent)
    out = root / 'build/launcher-snow-test'
    checks.run(out, body)
    (out / 'result.json').write_text(json.dumps(dict(success=True, particles=150, scheduling='vsync',
        files_sha256={str(path.relative_to(root)): build.sha(path) for path in
            [snow_source, root / 'android/port.json', root / 'tools/android_launcher.py', Path(__file__)]}), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
