package com.frostpunk.staticrecomp.probe;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class CrashReportTest {
    public static void main(String[] args) throws Exception {
        byte[] trace = {48, 42, 82, 3, 114, 1, 'x', 114, 3, 'b', 'a', 'd'};
        if (!"bad".equals(CrashReport.nativeAbortMessage(trace)))
            throw new AssertionError("Expected the top-level abort message");
        if (!CrashReport.nativeAbortMessage(java.util.Arrays.copyOf(trace, trace.length - 1)).isEmpty())
            throw new AssertionError("Expected truncated abort messages to be ignored");

        File directory = Files.createTempDirectory("frostpunk-report-").toFile();
        File file = new File(directory, "report.txt");
        File shared = new File(directory, "shared/report.txt");
        File sharedTombstone = new File(shared.getPath() + ".pb");
        File privateTombstone = new File(file.getPath() + ".pb");
        try {
            StringBuilder text = new StringBuilder();
            for (int i = 0; i < 3000; i++) text.append("Строка ").append(i).append("\n");
            String report = text.append("Last line\n").toString();
            CrashReport.write(file, "old report");
            CrashReport.write(file, report);
            byte[] saved = Files.readAllBytes(file.toPath());
            if (report.length() <= 20000 || !report.equals(new String(saved, StandardCharsets.UTF_8)))
                throw new AssertionError("Expected the complete UTF-8 report");
            if (!"Last line\n".equals(CrashReport.tail(file, 14)))
                throw new AssertionError("Expected a complete last line when taking the log tail");

            File tombstone = CrashReport.saveTombstone(shared, file, new ByteArrayInputStream(trace));
            if (!sharedTombstone.equals(tombstone) || privateTombstone.exists()
                    || !java.util.Arrays.equals(trace, Files.readAllBytes(tombstone.toPath())))
                throw new AssertionError("Expected the complete binary trace in shared storage");
            File blocked = new File(file, "report.txt");
            tombstone = CrashReport.saveTombstone(blocked, file, new ByteArrayInputStream(trace));
            if (!privateTombstone.equals(tombstone) || !java.util.Arrays.equals(trace, Files.readAllBytes(tombstone.toPath())))
                throw new AssertionError("Expected the binary trace to survive a failed export");

            // The crashing TID comes last and its map value comes before its key.
            byte[] stack = {
                (byte) 130, 1, 12, 8, 1, 18, 8, 34, 6, 8, 1, 50, 2, 'n', 'o',
                (byte) 130, 1, 36, 18, 32, 18, 4, 'r', 'e', 'n', 'd', 34, 24,
                8, (byte) 180, 36, 34, 3, 'f', 'o', 'o', 40, 4, 50, 5, 'l', 'i', 'b', '.', 's',
                56, 16, 66, 3, 'a', 'b', 'c', 8, 42, 48, 42
            };
            Files.write(privateTombstone.toPath(), stack);
            String expected = "tid=42 (rend)\n#00 pc 0000000000001234  lib.s (offset 0x10) (foo+4) (BuildId: abc)";
            if (!expected.equals(CrashReport.nativeStackTrace(privateTombstone)))
                throw new AssertionError("Expected only the crashed thread, regardless of protobuf field order");
            stack[stack.length - 1] = 43;
            Files.write(privateTombstone.toPath(), stack);
            if (!CrashReport.nativeStackTrace(privateTombstone).isEmpty())
                throw new AssertionError("Expected no stack when the crashed thread has no entry");
        } finally {
            Files.deleteIfExists(sharedTombstone.toPath());
            Files.deleteIfExists(shared.getParentFile().toPath());
            Files.deleteIfExists(privateTombstone.toPath());
            Files.deleteIfExists(file.toPath());
            Files.delete(directory.toPath());
        }

        StringBuilder clipboard = new StringBuilder();
        CrashReport.appendClipboardLog(clipboard, "render.log", "10 swapchain create result=0 SUCCESS\n"
            + "11 first present result=1000001003\n12 render summary ok=0 suboptimal=57630 other=0\n");
        if (clipboard.length() != 0) throw new AssertionError("Routine rendering must stay out of the clipboard");
        CrashReport.appendClipboardLog(clipboard, "render.log", "13 present result=-4 DEVICE_LOST\n"
            + "14 present result=-4 DEVICE_LOST\n15 acquire result=-1000000000 SURFACE_LOST\n");
        if (!clipboard.toString().equals("\n\n--- render.log ---\n14 present result=-4 DEVICE_LOST\n"
                + "15 acquire result=-1000000000 SURFACE_LOST\n"))
            throw new AssertionError("Expected the last occurrence of each Vulkan failure in order");

        clipboard.setLength(0);
        StringBuilder nativeLog = new StringBuilder("PRESENT_SAMPLE count=23\nFP_GEOMETRY error=0.543\n");
        for (int i = 0; i < 10; i++) nativeLog.append("error: allocation ").append(i).append("\n");
        CrashReport.appendClipboardLog(clipboard, "native.log", nativeLog.toString());
        if (clipboard.toString().contains("allocation 1\n") || !clipboard.toString().contains("allocation 2\n")
                || !clipboard.toString().endsWith("allocation 9\n") || clipboard.toString().contains("PRESENT_SAMPLE")
                || clipboard.toString().contains("FP_GEOMETRY"))
            throw new AssertionError("Expected only the last eight native diagnostics");

        clipboard.setLength(0);
        CrashReport.appendClipboardLog(clipboard, "native.log", "RDVK_CRASH signal=11\n"
            + "RDVK_CRASH fault libvulkan.so+0x123\nRDVK_CRASH #01 libvulkan.so+0x456\nRDVK_CRASH #09 libfoo.so+0x789\n");
        if (!clipboard.toString().contains("signal=11") || !clipboard.toString().contains("#01")
                || clipboard.toString().contains("#09"))
            throw new AssertionError("Expected the signal and the top of the native Vulkan stack");

        String signal = CrashReport.signalReason("Native signal 11 (SIGSEGV) code=1 addr=0x10 pc=0x7f00 tid=42\nstale tail", false);
        if (!signal.equals("Crash in native game code: 11 (SIGSEGV) code=1 addr=0x10 pc=0x7f00 tid=42"))
            throw new AssertionError("Expected the first line of the native journal: " + signal);
    }
}
