package com.frostpunk.staticrecomp.probe;

public final class CrashReportTest {
    public static void main(String[] args) throws Exception {
        byte[] trace = {48, 42, 82, 3, 114, 1, 'x', 114, 3, 'b', 'a', 'd'};
        if (!"bad".equals(CrashReport.nativeAbortMessage(trace)))
            throw new AssertionError("Expected the top-level abort message");
        if (!CrashReport.nativeAbortMessage(java.util.Arrays.copyOf(trace, trace.length - 1)).isEmpty())
            throw new AssertionError("Expected truncated abort messages to be ignored");
    }
}
