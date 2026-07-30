package org.mettle.clion.run;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.Executor;
import com.intellij.execution.configurations.CommandLineState;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.process.KillableColoredProcessHandler;
import com.intellij.execution.process.ProcessHandler;
import com.intellij.execution.process.ProcessOutput;
import com.intellij.execution.process.ProcessTerminatedListener;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.execution.ui.ConsoleView;
import com.intellij.execution.ui.ConsoleViewContentType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.util.MettleProcess;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Runs one Mettle configuration.
 *
 * <p>"Build and run" compiles first and then starts the executable, so the console shows the
 * program, not the compiler. The compiler's own output is replayed into the same console above it,
 * which keeps warnings visible without burying the program's output.
 */
public class MettleCommandLineState extends CommandLineState {

    private final MettleRunConfiguration configuration;
    private String compilerOutput = "";
    private String header = "";

    protected MettleCommandLineState(@NotNull ExecutionEnvironment environment,
                                     @NotNull MettleRunConfiguration configuration) {
        super(environment);
        this.configuration = configuration;
    }

    @Override
    protected @NotNull ProcessHandler startProcess() throws ExecutionException {
        Path source = configuration.source();
        if (source == null) throw new ExecutionException("No Mettle file selected");

        switch (configuration.getMode()) {
            case TEST:
                return start(MettleBuild.test(getEnvironment().getProject(), source,
                        configuration.compilerArgumentList()).commandLine());
            case EXPLAIN:
                return start(MettleBuild.explain(getEnvironment().getProject(), source,
                        configuration.compilerArgumentList(), false).commandLine());
            case BUILD:
                return start(buildPlan(source).commandLine());
            case RUN:
            default:
                return compileThenRun(source);
        }
    }

    private MettleBuild.Plan buildPlan(@NotNull Path source) throws ExecutionException {
        return MettleBuild.executable(getEnvironment().getProject(), source, configuration.executable(),
                configuration.isRelease(), configuration.compilerArgumentList(), false);
    }

    private @NotNull ProcessHandler compileThenRun(@NotNull Path source) throws ExecutionException {
        MettleBuild.Plan plan = buildPlan(source);
        if (plan.output.getParent() != null) {
            try {
                Files.createDirectories(plan.output.getParent());
            } catch (Exception ignored) {
                // the compiler reports its own I/O error if the directory is unusable
            }
        }
        header = plan.describe();
        ProcessOutput output = MettleProcess.run(plan.commandLine(), 10 * 60 * 1000);
        if (output == null) throw new ExecutionException("Could not start " + plan.compiler);
        compilerOutput = (output.getStdout() + output.getStderr()).trim();
        if (output.getExitCode() != 0) {
            throw new ExecutionException("Compilation failed:\n"
                    + (compilerOutput.isEmpty() ? "exit code " + output.getExitCode() : compilerOutput));
        }
        if (!Files.isRegularFile(plan.output)) {
            throw new ExecutionException("The compiler reported success but produced no "
                    + plan.output.getFileName());
        }

        GeneralCommandLine commandLine = MettleProcess.commandLine(plan.output.toString(),
                configuration.programArgumentList(), configuration.resolvedWorkingDirectory());
        return start(commandLine);
    }

    private @NotNull ProcessHandler start(@NotNull GeneralCommandLine commandLine) throws ExecutionException {
        if (header.isEmpty()) header = commandLine.getCommandLineString();
        KillableColoredProcessHandler handler = new KillableColoredProcessHandler(commandLine);
        ProcessTerminatedListener.attach(handler);
        return handler;
    }

    @Override
    protected @Nullable ConsoleView createConsole(@NotNull Executor executor) throws ExecutionException {
        ConsoleView console = super.createConsole(executor);
        if (console == null) return null;
        if (!header.isEmpty()) {
            console.print(header + "\n", ConsoleViewContentType.SYSTEM_OUTPUT);
        }
        if (!compilerOutput.isEmpty()) {
            console.print(compilerOutput + "\n", ConsoleViewContentType.NORMAL_OUTPUT);
        }
        return console;
    }
}
