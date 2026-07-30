package org.mettle.clion.debug;

import com.intellij.execution.DefaultExecutionResult;
import com.intellij.execution.ExecutionException;
import com.intellij.execution.ExecutionResult;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.configurations.RunProfile;
import com.intellij.execution.configurations.RunProfileState;
import com.intellij.execution.configurations.RunnerSettings;
import com.intellij.execution.executors.DefaultDebugExecutor;
import com.intellij.execution.filters.TextConsoleBuilderFactory;
import com.intellij.execution.process.KillableColoredProcessHandler;
import com.intellij.execution.process.ProcessEvent;
import com.intellij.execution.process.ProcessListener;
import com.intellij.execution.process.ProcessOutput;
import com.intellij.execution.process.ProcessTerminatedListener;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.execution.runners.GenericProgramRunner;
import com.intellij.execution.ui.ConsoleView;
import com.intellij.execution.ui.ConsoleViewContentType;
import com.intellij.execution.ui.RunContentDescriptor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.SystemInfo;
import com.intellij.xdebugger.XDebugProcess;
import com.intellij.xdebugger.XDebugProcessStarter;
import com.intellij.xdebugger.XDebugSession;
import com.intellij.xdebugger.XDebuggerManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.run.MettleBuild;
import org.mettle.clion.run.MettleRunConfiguration;
import org.mettle.clion.util.MettleProcess;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Debugs a Mettle file: build it with {@code --debug-hooks}, host the debug pipe, start the
 * program pointed at it, and drive the session over that pipe.
 *
 * <p>Windows only, because the runtime's transport is - on other platforms the hooks compile to
 * no-ops, so offering Debug there would be a lie.
 */
public class MettleDebugRunner extends GenericProgramRunner<RunnerSettings> {

    private static final AtomicInteger NEXT_BUILD = new AtomicInteger(1);

    @Override
    public @NotNull String getRunnerId() {
        return "MettleDebugRunner";
    }

    @Override
    public boolean canRun(@NotNull String executorId, @NotNull RunProfile profile) {
        if (!DefaultDebugExecutor.EXECUTOR_ID.equals(executorId)) return false;
        if (!SystemInfo.isWindows) return false;
        return profile instanceof MettleRunConfiguration
                && ((MettleRunConfiguration) profile).getMode() == MettleRunConfiguration.Mode.RUN;
    }

    @Override
    protected @Nullable RunContentDescriptor doExecute(@NotNull RunProfileState state,
                                                       @NotNull ExecutionEnvironment environment)
            throws ExecutionException {
        MettleRunConfiguration configuration = (MettleRunConfiguration) environment.getRunProfile();
        Project project = environment.getProject();
        Path source = configuration.source();
        if (source == null) throw new ExecutionException("No Mettle file selected");

        Path executable = debugExecutable(source);
        MettleBuild.Plan plan = MettleBuild.executable(project, source, executable, false,
                configuration.compilerArgumentList(), true);
        ProcessOutput compile = MettleProcess.run(plan.commandLine(), 10 * 60 * 1000);
        if (compile == null) throw new ExecutionException("Could not start " + plan.compiler);
        String compilerOutput = (compile.getStdout() + compile.getStderr()).trim();
        if (compile.getExitCode() != 0) {
            throw new ExecutionException("Debug build failed:\n"
                    + (compilerOutput.isEmpty() ? "exit code " + compile.getExitCode() : compilerOutput));
        }

        MettleDebugProtocol protocol;
        try {
            protocol = MettleDebugProtocol.host();
        } catch (IOException failure) {
            throw new ExecutionException(failure.getMessage(), failure);
        }

        GeneralCommandLine commandLine = MettleProcess.commandLine(executable.toString(),
                configuration.programArgumentList(), configuration.resolvedWorkingDirectory());
        commandLine.withEnvironment("METTLE_DBG_PIPE", protocol.pipeName());

        KillableColoredProcessHandler processHandler;
        try {
            processHandler = new KillableColoredProcessHandler(commandLine);
        } catch (ExecutionException failure) {
            protocol.close();
            throw failure;
        }
        ProcessTerminatedListener.attach(processHandler);
        processHandler.addProcessListener(new ProcessListener() {
            @Override
            public void processTerminated(@NotNull ProcessEvent event) {
                protocol.close();
                deleteQuietly(executable);
            }
        });

        ConsoleView console = TextConsoleBuilderFactory.getInstance().createBuilder(project).getConsole();
        console.attachToProcess(processHandler);
        if (!compilerOutput.isEmpty()) {
            console.print(compilerOutput + "\n", ConsoleViewContentType.NORMAL_OUTPUT);
        }
        console.print(plan.describe() + "\n", ConsoleViewContentType.SYSTEM_OUTPUT);
        ExecutionResult executionResult = new DefaultExecutionResult(console, processHandler);

        File workingDirectory = MettleBuild.workingDirectory(project, source);
        XDebugSession session = XDebuggerManager.getInstance(project).startSession(environment,
                new XDebugProcessStarter() {
                    @Override
                    public @NotNull XDebugProcess start(@NotNull XDebugSession session) {
                        return new MettleDebugProcess(session, protocol, executionResult,
                                workingDirectory, configuration.isStopAtEntry());
                    }
                });
        return session.getRunContentDescriptor();
    }

    private static @NotNull Path debugExecutable(@NotNull Path source) throws ExecutionException {
        Path directory = Paths.get(System.getProperty("java.io.tmpdir"), "mettle-debug");
        try {
            Files.createDirectories(directory);
        } catch (IOException failure) {
            throw new ExecutionException("Could not create " + directory, failure);
        }
        return directory.resolve(MettleBuild.stem(source) + "-" + NEXT_BUILD.getAndIncrement() + ".exe");
    }

    private static void deleteQuietly(@NotNull Path path) {
        try {
            Files.deleteIfExists(path);
        } catch (IOException ignored) {
            // Windows may still hold the image briefly; the next session uses a new name
        }
    }
}
