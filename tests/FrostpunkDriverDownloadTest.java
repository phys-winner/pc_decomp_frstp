package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import java.io.*;
import java.net.*;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.*;

public final class FrostpunkDriverDownloadTest {
    static void need(boolean value, String why) { if (!value) throw new AssertionError(why); }
    static final class TestContext extends ContextWrapper {
        final File root;
        TestContext(File root) { super(null); this.root = root; root.mkdirs(); getCacheDir().mkdirs(); }
        @Override public Context getApplicationContext() { return this; }
        @Override public File getFilesDir() { return root; }
        @Override public File getCacheDir() { return new File(root, "cache"); }
        @Override public String getPackageName() { return "com.frostpunk.staticrecomp"; }
    }
    static byte[] elf(int variant) {
        byte[] bytes = new byte[64];
        bytes[0]=127; bytes[1]='E'; bytes[2]='L'; bytes[3]='F'; bytes[4]=2; bytes[5]=1;
        bytes[16]=3; bytes[18]=(byte)183; bytes[63]=(byte)variant;
        return bytes;
    }
    static final class Server implements AutoCloseable {
        final ServerSocket server = new ServerSocket(0, 4, InetAddress.getByName("127.0.0.1"));
        final CountDownLatch waiting = new CountDownLatch(1), release = new CountDownLatch(1);
        volatile int requests;
        Server(byte[] body, int mode) throws IOException {
            Thread thread = new Thread(() -> {
                try (Socket socket = server.accept()) {
                    requests++;
                    BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), "US-ASCII"));
                    for (String line; (line=in.readLine())!=null && !line.isEmpty();) { }
                    OutputStream out = socket.getOutputStream();
                    if (mode != 1) {
                        out.write(("HTTP/1.1 " + (mode==3?"500 Error":"200 OK") + "\r\nContent-Length: " + body.length + "\r\nConnection: close\r\n\r\n").getBytes("US-ASCII"));
                        out.write(body, 0, mode==2?1:body.length); out.flush();
                    }
                    waiting.countDown();
                    if (mode==1 || mode==2) release.await(10, TimeUnit.SECONDS);
                } catch (Exception ignored) { waiting.countDown(); }
            }, "driver-test-server");
            thread.setDaemon(true); thread.start();
        }
        String url() { return "http://127.0.0.1:" + server.getLocalPort() + "/driver.so"; }
        @Override public void close() throws IOException { release.countDown(); server.close(); }
    }
    static void good(FrostpunkDriverDownload job) {
        job.run(); need(job.failure==null, "download failed: " + (job.failure==null?"":job.failure.detail));
        need(!job.target.exists(), "download part was left behind");
    }
    static void cancel(TestContext context, int mode, String chosen) throws Exception {
        try (Server server = new Server(elf(2), mode)) {
            FrostpunkDriverDownload job = new FrostpunkDriverDownload(context, "cancel", server.url());
            Thread worker = new Thread(job, "driver-test-worker"); worker.setDaemon(true); worker.start();
            need(server.waiting.await(5, TimeUnit.SECONDS), "server was not reached");
            long start=System.nanoTime(); job.cancel(); worker.join(3000);
            need(!worker.isAlive(), "cancel did not interrupt the connection within 3 seconds");
            need(job.failure!=null && job.failure.code==Failure.CANCELLED, "cancel reported as a network failure");
            need(!job.target.exists(), "cancelled part was left behind");
            need(VulkanDriver.TURNIP.chosen(context).getName().equals(chosen), "cancel changed the selected driver");
            System.out.println("CANCEL mode="+mode+" elapsed_ms="+(System.nanoTime()-start)/1000000+" PASS");
        }
    }
    public static void main(String[] args) {
        try { run(args); } catch (Throwable failure) { failure.printStackTrace(); System.exit(1); }
    }
    static void run(String[] args) throws Exception {
        TestContext context = new TestContext(new File(args[0], "synthetic"));
        String url;
        try (Server server = new Server(elf(1), 0)) {
            url=server.url();
            FrostpunkDriverDownload firstJob=new FrostpunkDriverDownload(context, "first", url);
            good(firstJob);
            need(firstJob.done==64 && firstJob.total==64,"download progress counters");
            need(server.requests==1, "first driver was not downloaded");
        }
        File first=VulkanDriver.TURNIP.chosen(context);
        good(new FrostpunkDriverDownload(context, "cached", url));
        need(VulkanDriver.TURNIP.chosen(context).equals(first), "offline cache did not restore the same driver");
        need(VulkanDriver.TURNIP.unused(context).isEmpty(), "active driver considered unused");
        cancel(context,1,first.getName()); cancel(context,2,first.getName());
        FrostpunkDriverDownload pre=new FrostpunkDriverDownload(context,"pre-cancel",url); pre.cancel(); pre.run();
        need(pre.failure!=null && pre.failure.code==Failure.CANCELLED && !pre.target.exists(),"pre-cancel failed");
        try (Server server = new Server(elf(2), 3)) {
            FrostpunkDriverDownload failed=new FrostpunkDriverDownload(context,"HTTP error",server.url()); failed.run();
            need(failed.failure!=null && failed.failure.code!=Failure.CANCELLED && !failed.target.exists(),"HTTP failure cleanup");
            need(VulkanDriver.TURNIP.chosen(context).equals(first),"HTTP error changed the driver");
        }
        try (Server server = new Server(elf(2), 0)) { good(new FrostpunkDriverDownload(context,"second",server.url())); }
        File second=VulkanDriver.TURNIP.chosen(context); need(!second.equals(first),"different driver replaced the cache entry");
        need(VulkanDriver.TURNIP.unused(context).equals(Arrays.asList(first)),"unused driver list");
        VulkanDriver.TURNIP.removeUnused(context); need(!first.exists() && second.isFile(),"cleanup removed active driver");
        need(VulkanDriver.TURNIP.unused(context).isEmpty(),"cleanup button should disappear");
        VulkanDriver.TURNIP.reset(context); need(second.isFile() && VulkanDriver.TURNIP.unused(context).size()==1,"reset did not preserve cached external driver");
        VulkanDriver.TURNIP.removeUnused(context); need(VulkanDriver.TURNIP.unused(context).isEmpty() && !second.exists(),"cleanup after reset");
        System.out.println("DRIVER_CACHE offline_reuse=PASS active_preserved=PASS cleanup_visibility=PASS HTTP_failure=PASS");
        if (args.length>1) {
            TestContext live=new TestContext(new File(args[0],"live"));
            int releases = FrostpunkGithub.releases(live,"MrPurple666/purple-turnip").length();
            need(releases>0,"GitHub release list is empty");
            System.out.println("REAL_GITHUB_RELEASES count="+releases+" PASS");
            good(new FrostpunkDriverDownload(live,"purple-turnip T30",args[1]));
            need(VulkanDriver.sha(VulkanDriver.TURNIP.chosen(live)).equals("1d80dfa019659b008e4669311db5b1e4a02af59ff1d5458a98e2f5fe18ed013b"),"real T30 digest mismatch");
            good(new FrostpunkDriverDownload(live,"cached T30",args[1]));
            System.out.println("REAL_GITHUB_DOWNLOAD title="+VulkanDriver.TURNIP.title(live)+" sha256=PASS cached_repeat=PASS");
        }
    }
}
