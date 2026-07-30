package org.mettle.clion.debug;

import com.intellij.execution.ExecutionResult;
import com.intellij.execution.process.ProcessHandler;
import com.intellij.execution.ui.ConsoleView;
import com.intellij.execution.ui.ConsoleViewContentType;
import com.intellij.execution.ui.ExecutionConsole;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.xdebugger.XDebugProcess;
import com.intellij.xdebugger.XDebugSession;
import com.intellij.xdebugger.XDebuggerUtil;
import com.intellij.xdebugger.XSourcePosition;
import com.intellij.xdebugger.breakpoints.XBreakpointHandler;
import com.intellij.xdebugger.breakpoints.XBreakpointProperties;
import com.intellij.xdebugger.breakpoints.XLineBreakpoint;
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider;
import com.intellij.xdebugger.frame.XSuspendContext;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Drives a program built with {@code --debug-hooks}.
 *
 * <p>The runtime holds at the first function until the adapter has sent its breakpoints and
 * resumed, so nothing is sent before the {@code entry} stop arrives: a control command that
 * reaches the runtime while it is not paused is applied on the spot and never wakes the pause
 * loop again.
 */
public class MettleDebugProcess extends XDebugProcess {

    private final MettleDebugProtocol protocol;
    private final ExecutionResult executionResult;
    private final File workingDirectory;
    private final boolean stopAtEntry;

    private final Map<Integer, String> filePaths = new LinkedHashMap<>();
    private final Map<String, Integer> fileIds = new LinkedHashMap<>();
    private final Map<Integer, int[]> functionFile = new LinkedHashMap<>();
    private final Map<Integer, String> functionNames = new LinkedHashMap<>();

    private final BreakpointHandler breakpointHandler = new BreakpointHandler();
    private final CountDownLatch entryStop = new CountDownLatch(1);
    private final AtomicBoolean ready = new AtomicBoolean();
    private final AtomicBoolean detached = new AtomicBoolean();

    protected MettleDebugProcess(@NotNull XDebugSession session, @NotNull MettleDebugProtocol protocol,
                                 @NotNull ExecutionResult executionResult,
                                 @Nullable File workingDirectory, boolean stopAtEntry) {
        super(session);
        this.protocol = protocol;
        this.executionResult = executionResult;
        this.workingDirectory = workingDirectory;
        this.stopAtEntry = stopAtEntry;
        protocol.start(new ProtocolListener());
    }

    // ---------------------------------------------------------- plumbing

    @Override
    public @NotNull XDebuggerEditorsProvider getEditorsProvider() {
        return new MettleDebuggerEditorsProvider();
    }

    @Override
    protected @Nullable ProcessHandler doGetProcessHandler() {
        return executionResult.getProcessHandler();
    }

    @Override
    public @NotNull ExecutionConsole createConsole() {
        return executionResult.getExecutionConsole();
    }

    @Override
    public XBreakpointHandler<?> @NotNull [] getBreakpointHandlers() {
        return new XBreakpointHandler[]{breakpointHandler};
    }

    @Override
    public void sessionInitialized() {
        ApplicationManager.getApplication().executeOnPooledThread(this::handshake);
    }

    // ---------------------------------------------------------- handshake

    private void handshake() {
        String hello = protocol.awaitLine(20000);
        if (hello == null) {
            fail("The debugged program never connected to the debug pipe. "
                    + "Was it built with --debug-hooks?");
            return;
        }
        for (;;) {
            String line = protocol.awaitLine(20000);
            if (line == null) {
                fail("The debug runtime stopped sending its function tables.");
                return;
            }
            if (line.equals("tablesdone")) break;
            readTableLine(line);
        }
        try {
            if (!entryStop.await(20, TimeUnit.SECONDS)) {
                fail("The debug runtime never reached its entry stop.");
                return;
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return;
        }

        ready.set(true);
        breakpointHandler.resend();
        if (stopAtEntry) {
            reportStop(null);
        } else {
            protocol.send("go");
        }
    }

    private void readTableLine(@NotNull String line) {
        String[] fields = line.split("\t", -1);
        if (fields.length >= 3 && "file".equals(fields[0])) {
            int id = MettleDebugProtocol.number(fields, 1);
            String path = fields[2];
            filePaths.put(id, path);
            fileIds.put(normalize(path), id);
        } else if (fields.length >= 4 && "fn".equals(fields[0])) {
            int id = MettleDebugProtocol.number(fields, 1);
            functionFile.put(id, new int[]{MettleDebugProtocol.number(fields, 2)});
            functionNames.put(id, fields[3]);
        }
    }

    private void fail(@NotNull String message) {
        print(message + "\n", ConsoleViewContentType.ERROR_OUTPUT);
        protocol.send("detach");
        getSession().stop();
    }

    private void print(@NotNull String text, @NotNull ConsoleViewContentType type) {
        ExecutionConsole console = executionResult.getExecutionConsole();
        if (console instanceof ConsoleView) {
            ((ConsoleView) console).print(text, type);
        }
    }

    // ------------------------------------------------------------ control

    @Override
    public void resume(@Nullable XSuspendContext context) {
        protocol.send("go");
    }

    @Override
    public void startStepOver(@Nullable XSuspendContext context) {
        protocol.send("next");
    }

    @Override
    public void startStepInto(@Nullable XSuspendContext context) {
        protocol.send("stepin");
    }

    @Override
    public void startStepOut(@Nullable XSuspendContext context) {
        protocol.send("stepout");
    }

    @Override
    public void startPausing() {
        protocol.send("pause");
    }

    @Override
    public void stop() {
        if (detached.compareAndSet(false, true)) protocol.close();
    }

    @Override
    public void runToPosition(@NotNull XSourcePosition position, @Nullable XSuspendContext context) {
        // The runtime has no run-to-cursor command; a one-shot breakpoint would linger, so the
        // honest behaviour is to continue and let the user's real breakpoints decide.
        print("Run to cursor is not supported by the Mettle debug runtime; continuing.\n",
                ConsoleViewContentType.SYSTEM_OUTPUT);
        protocol.send("go");
    }

    // ------------------------------------------------------------- stops

    private class ProtocolListener implements MettleDebugProtocol.Listener {
        @Override
        public void stopped(MettleDebugProtocol.@NotNull StopEvent event) {
            if ("entry".equals(event.reason)) {
                entryStop.countDown();
                return;
            }
            ApplicationManager.getApplication().executeOnPooledThread(() -> reportStop(event));
        }

        @Override
        public void disconnected() {
            if (detached.compareAndSet(false, true)) {
                ApplicationManager.getApplication().invokeLater(() -> getSession().stop());
            }
        }
    }

    /** Collects the frames for the current stop and hands them to the session. */
    private void reportStop(MettleDebugProtocol.@Nullable StopEvent event) {
        List<MettleStackFrame> frames = collectFrames();
        MettleSuspendContext context = new MettleSuspendContext(frames);

        if (event != null && "exception".equals(event.reason)) {
            print("Stopped on " + describeException(event) + "\n", ConsoleViewContentType.ERROR_OUTPUT);
            getSession().positionReached(context);
            return;
        }
        if (event != null && "breakpoint".equals(event.reason)) {
            XLineBreakpoint<XBreakpointProperties> breakpoint =
                    breakpointHandler.find(event.fileId, event.line);
            if (breakpoint != null) {
                getSession().breakpointReached(breakpoint, null, context);
                return;
            }
        }
        getSession().positionReached(context);
    }

    private @NotNull List<MettleStackFrame> collectFrames() {
        List<MettleStackFrame> frames = new ArrayList<>();
        for (String line : protocol.query("stack", "framesdone", 5000)) {
            String[] fields = line.split("\t", -1);
            if (fields.length < 4 || !"frame".equals(fields[0])) continue;
            int index = MettleDebugProtocol.number(fields, 1);
            int functionId = MettleDebugProtocol.number(fields, 2);
            int line1Based = MettleDebugProtocol.number(fields, 3);
            String name = functionNames.getOrDefault(functionId, "fn#" + functionId);
            frames.add(new MettleStackFrame(protocol, index, name, position(functionId, line1Based)));
        }
        return frames;
    }

    private @Nullable XSourcePosition position(int functionId, int line1Based) {
        int[] fileId = functionFile.get(functionId);
        String path = fileId == null ? null : filePaths.get(fileId[0]);
        VirtualFile file = path == null ? null : resolve(path);
        if (file == null) return null;
        return XDebuggerUtil.getInstance().createPosition(file, Math.max(0, line1Based - 1));
    }

    private static @NotNull String describeException(MettleDebugProtocol.@NotNull StopEvent event) {
        String code = event.exceptionCode.toLowerCase(Locale.ROOT);
        String name;
        switch (code) {
            case "0xc0000005": name = "an access violation"; break;
            case "0xc0000094": name = "an integer division by zero"; break;
            case "0xc000001d": name = "an illegal instruction"; break;
            case "0xc000008e": name = "a floating-point division by zero"; break;
            case "0xc000008c": name = "an array bounds violation"; break;
            default: name = "a hardware exception";
        }
        return name + " (" + event.exceptionCode + ") at " + event.exceptionAddress;
    }

    // -------------------------------------------------------- file lookup

    private @Nullable VirtualFile resolve(@NotNull String reportedPath) {
        Path path = Paths.get(reportedPath);
        if (!path.isAbsolute() && workingDirectory != null) {
            path = workingDirectory.toPath().resolve(reportedPath);
        }
        return LocalFileSystem.getInstance().findFileByNioFile(path.normalize());
    }

    private @NotNull String normalize(@NotNull String reportedPath) {
        Path path = Paths.get(reportedPath);
        if (!path.isAbsolute() && workingDirectory != null) {
            path = workingDirectory.toPath().resolve(reportedPath);
        }
        return path.normalize().toString().replace('\\', '/').toLowerCase(Locale.ROOT);
    }

    /** Breakpoints, kept as one set per file because {@code setbp} replaces a file's whole set. */
    private class BreakpointHandler extends XBreakpointHandler<XLineBreakpoint<XBreakpointProperties>> {

        private final Map<String, XLineBreakpoint<XBreakpointProperties>> byLocation = new LinkedHashMap<>();

        BreakpointHandler() {
            super(MettleLineBreakpointType.class);
        }

        @Override
        public void registerBreakpoint(@NotNull XLineBreakpoint<XBreakpointProperties> breakpoint) {
            Integer fileId = fileIdOf(breakpoint);
            if (fileId == null) {
                if (ready.get()) {
                    getSession().setBreakpointInvalid(breakpoint,
                            "This file is not part of the debugged program.");
                }
                return;
            }
            byLocation.put(fileId + ":" + (breakpoint.getLine() + 1), breakpoint);
            resend();
        }

        @Override
        public void unregisterBreakpoint(@NotNull XLineBreakpoint<XBreakpointProperties> breakpoint,
                                         boolean temporary) {
            Integer fileId = fileIdOf(breakpoint);
            if (fileId == null) return;
            byLocation.remove(fileId + ":" + (breakpoint.getLine() + 1));
            resend();
        }

        @Nullable XLineBreakpoint<XBreakpointProperties> find(int fileId, int line) {
            return byLocation.get(fileId + ":" + line);
        }

        /** Replays the whole set: {@code clearall}, then the plain lines, then the conditional ones. */
        void resend() {
            if (!ready.get()) return;
            Map<Integer, StringBuilder> plain = new LinkedHashMap<>();
            List<String> conditional = new ArrayList<>();
            for (Map.Entry<String, XLineBreakpoint<XBreakpointProperties>> entry : byLocation.entrySet()) {
                String[] key = entry.getKey().split(":");
                int fileId = Integer.parseInt(key[0]);
                String line = key[1];
                String condition = entry.getValue().getConditionExpression() == null
                        ? null : entry.getValue().getConditionExpression().getExpression().trim();
                if (condition != null && !condition.isEmpty()) {
                    conditional.add("bpadd\t" + fileId + "\t" + line + "\t" + condition);
                    plain.computeIfAbsent(fileId, id -> new StringBuilder());
                } else {
                    StringBuilder lines = plain.computeIfAbsent(fileId, id -> new StringBuilder());
                    if (lines.length() > 0) lines.append(',');
                    lines.append(line);
                }
            }
            protocol.send("clearall");
            for (Map.Entry<Integer, StringBuilder> entry : plain.entrySet()) {
                protocol.send("setbp\t" + entry.getKey() + "\t" + entry.getValue());
            }
            for (String command : conditional) {
                protocol.send(command);
            }
        }

        private @Nullable Integer fileIdOf(@NotNull XLineBreakpoint<XBreakpointProperties> breakpoint) {
            XSourcePosition position = breakpoint.getSourcePosition();
            String path = position != null ? position.getFile().getPath() : null;
            if (path == null) return null;
            return fileIds.get(path.replace('\\', '/').toLowerCase(Locale.ROOT));
        }
    }
}
