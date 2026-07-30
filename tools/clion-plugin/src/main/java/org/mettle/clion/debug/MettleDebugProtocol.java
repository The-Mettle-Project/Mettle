package org.mettle.clion.debug;

import com.intellij.openapi.diagnostic.Logger;
import com.sun.jna.platform.win32.Kernel32;
import com.sun.jna.platform.win32.WinBase;
import com.sun.jna.platform.win32.WinNT;
import com.sun.jna.ptr.IntByReference;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The adapter side of the compiler's {@code --debug-hooks} debug protocol.
 *
 * <p>The instrumented program opens the named pipe this class hosts and speaks a line-based,
 * tab-separated protocol (see {@code src/runtime/debug.c}). Reads poll with {@code PeekNamedPipe}
 * and only ever read bytes that have already arrived: a blocking read would serialise against our
 * own writes on the same synchronous handle and deadlock the session.
 */
public class MettleDebugProtocol {

    private static final Logger LOG = Logger.getInstance(MettleDebugProtocol.class);
    private static final AtomicInteger NEXT_ID = new AtomicInteger(1);
    private static final int BUFFER_SIZE = 64 * 1024;

    /** A {@code stopped} event: why the program paused and where. */
    public static class StopEvent {
        public final String reason;
        public final int fileId;
        public final int line;
        public final int depth;
        public final int functionId;
        public final String exceptionCode;
        public final String exceptionAddress;

        StopEvent(String[] fields) {
            reason = field(fields, 1, "step");
            fileId = number(fields, 2);
            line = number(fields, 3);
            depth = number(fields, 4);
            functionId = number(fields, 5);
            exceptionCode = field(fields, 6, "");
            exceptionAddress = field(fields, 7, "");
        }
    }

    public interface Listener {
        void stopped(@NotNull StopEvent event);

        void disconnected();
    }

    private final String pipeName;
    private final WinNT.HANDLE pipe;
    private final BlockingQueue<String> replies = new LinkedBlockingQueue<>();
    private final ExecutorService queries = Executors.newSingleThreadExecutor(
            runnable -> new Thread(runnable, "Mettle debug queries"));
    private final AtomicBoolean closed = new AtomicBoolean();
    private final Object writeLock = new Object();

    private volatile Listener listener;
    private Thread reader;

    private MettleDebugProtocol(@NotNull String pipeName, @NotNull WinNT.HANDLE pipe) {
        this.pipeName = pipeName;
        this.pipe = pipe;
    }

    /** Hosts a fresh pipe. The name goes to the program in {@code METTLE_DBG_PIPE}. */
    public static @NotNull MettleDebugProtocol host() throws IOException {
        String name = "\\\\.\\pipe\\mettle-clion-" + ProcessHandle.current().pid()
                + "-" + NEXT_ID.getAndIncrement();
        WinNT.HANDLE handle = Kernel32.INSTANCE.CreateNamedPipe(
                name,
                WinBase.PIPE_ACCESS_DUPLEX,
                0, // byte stream, blocking
                1,
                BUFFER_SIZE,
                BUFFER_SIZE,
                0,
                null);
        if (handle == null || WinBase.INVALID_HANDLE_VALUE.equals(handle)) {
            throw new IOException("Could not create the debug pipe " + name
                    + " (error " + Kernel32.INSTANCE.GetLastError() + ")");
        }
        return new MettleDebugProtocol(name, handle);
    }

    public @NotNull String pipeName() {
        return pipeName;
    }

    /** Starts the reader; it waits for the program to connect, then streams lines. */
    public void start(@NotNull Listener listener) {
        this.listener = listener;
        reader = new Thread(this::readLoop, "Mettle debug reader");
        reader.setDaemon(true);
        reader.start();
    }

    private void readLoop() {
        try {
            // ConnectNamedPipe returns false with ERROR_PIPE_CONNECTED when the client won the race.
            Kernel32.INSTANCE.ConnectNamedPipe(pipe, null);
            byte[] buffer = new byte[BUFFER_SIZE];
            ByteArrayOutputStream pending = new ByteArrayOutputStream();
            while (!closed.get()) {
                IntByReference available = new IntByReference(0);
                boolean peeked;
                synchronized (writeLock) {
                    peeked = Kernel32.INSTANCE.PeekNamedPipe(pipe, null, 0, null, available, null);
                }
                if (!peeked) break;
                if (available.getValue() <= 0) {
                    Thread.sleep(5);
                    continue;
                }
                int wanted = Math.min(available.getValue(), buffer.length);
                IntByReference read = new IntByReference(0);
                boolean ok;
                synchronized (writeLock) {
                    ok = Kernel32.INSTANCE.ReadFile(pipe, buffer, wanted, read, null);
                }
                if (!ok || read.getValue() <= 0) break;
                pending.write(buffer, 0, read.getValue());
                pending = dispatchLines(pending);
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException failure) {
            LOG.info("Mettle debug pipe closed: " + failure.getMessage());
        } finally {
            Listener current = listener;
            if (current != null && !closed.get()) current.disconnected();
        }
    }

    /** Splits complete lines out of the buffer, keeping any partial tail. */
    private ByteArrayOutputStream dispatchLines(@NotNull ByteArrayOutputStream pending) {
        byte[] bytes = pending.toByteArray();
        int start = 0;
        for (int i = 0; i < bytes.length; i++) {
            if (bytes[i] != '\n') continue;
            int end = (i > start && bytes[i - 1] == '\r') ? i - 1 : i;
            dispatch(new String(bytes, start, end - start, StandardCharsets.UTF_8));
            start = i + 1;
        }
        ByteArrayOutputStream rest = new ByteArrayOutputStream();
        rest.write(bytes, start, bytes.length - start);
        return rest;
    }

    private void dispatch(@NotNull String line) {
        if (line.isEmpty()) return;
        if (line.startsWith("stopped\t") || line.equals("stopped")) {
            Listener current = listener;
            if (current != null) current.stopped(new StopEvent(line.split("\t", -1)));
            return;
        }
        replies.add(line);
    }

    // --------------------------------------------------------------- output

    /** Sends a command that expects no reply: {@code go}, {@code next}, {@code setbp}, ... */
    public void send(@NotNull String command) {
        if (closed.get()) return;
        byte[] bytes = (command + "\n").getBytes(StandardCharsets.UTF_8);
        synchronized (writeLock) {
            Kernel32.INSTANCE.WriteFile(pipe, bytes, bytes.length, new IntByReference(0), null);
        }
    }

    /** Runs a query on the protocol thread and collects lines until the terminator. */
    public @NotNull List<String> query(@NotNull String command, @NotNull String terminator,
                                       long timeoutMs) {
        if (closed.get()) return List.of();
        try {
            return queries.submit(() -> {
                replies.clear();
                send(command);
                List<String> lines = new ArrayList<>();
                long deadline = System.currentTimeMillis() + timeoutMs;
                while (System.currentTimeMillis() < deadline) {
                    String line = replies.poll(50, TimeUnit.MILLISECONDS);
                    if (line == null) continue;
                    if (line.equals(terminator) || line.startsWith(terminator + "\t")) return lines;
                    lines.add(line);
                }
                return lines;
            }).get(timeoutMs + 2000, TimeUnit.MILLISECONDS);
        } catch (Exception failure) {
            return List.of();
        }
    }

    /** Runs a query with a single-line reply ({@code evalr} / {@code setr}). */
    public @Nullable String queryOne(@NotNull String command, long timeoutMs) {
        if (closed.get()) return null;
        try {
            return queries.submit(() -> {
                replies.clear();
                send(command);
                return replies.poll(timeoutMs, TimeUnit.MILLISECONDS);
            }).get(timeoutMs + 2000, TimeUnit.MILLISECONDS);
        } catch (Exception failure) {
            return null;
        }
    }

    /** Waits for one line of the opening handshake. */
    public @Nullable String awaitLine(long timeoutMs) {
        try {
            return replies.poll(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return null;
        }
    }

    public void close() {
        if (!closed.compareAndSet(false, true)) return;
        try {
            send("detach");
        } catch (RuntimeException ignored) {
            // the program may already be gone
        }
        queries.shutdownNow();
        try {
            Kernel32.INSTANCE.DisconnectNamedPipe(pipe);
        } catch (RuntimeException ignored) {
            // best effort
        }
        Kernel32.INSTANCE.CloseHandle(pipe);
        if (reader != null) reader.interrupt();
    }

    // -------------------------------------------------------------- helpers

    static String field(String[] fields, int index, String fallback) {
        return index < fields.length ? fields[index] : fallback;
    }

    static int number(String[] fields, int index) {
        try {
            return index < fields.length ? Integer.parseInt(fields[index].trim()) : 0;
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }
}
