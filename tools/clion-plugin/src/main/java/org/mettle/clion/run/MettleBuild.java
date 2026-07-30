package org.mettle.clion.run;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.process.ProcessOutput;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.settings.MettleSettings;
import org.mettle.clion.settings.MettleToolchain;
import org.mettle.clion.util.MettleProcess;

import java.io.File;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

/** Builds the compiler command lines shared by the run configurations and the debugger. */
public final class MettleBuild {

    private MettleBuild() {
    }

    /** A compile that is ready to run. */
    public static class Plan {
        public final String compiler;
        public final List<String> arguments;
        public final File workingDirectory;
        public final Path output;

        Plan(String compiler, List<String> arguments, File workingDirectory, Path output) {
            this.compiler = compiler;
            this.arguments = arguments;
            this.workingDirectory = workingDirectory;
            this.output = output;
        }

        public GeneralCommandLine commandLine() {
            return MettleProcess.commandLine(compiler, arguments, workingDirectory);
        }

        /** The command as a user could retype it, for the console header. */
        public String describe() {
            StringBuilder text = new StringBuilder(compiler);
            for (String argument : arguments) {
                text.append(' ').append(argument.contains(" ") ? '"' + argument + '"' : argument);
            }
            return text.toString();
        }
    }

    public static @Nullable Path compilerFor(@NotNull Project project, @NotNull Path source) {
        VirtualFile file = LocalFileSystem.getInstance().findFileByNioFile(source);
        return MettleToolchain.findCompiler(project, file);
    }

    /**
     * {@code mettle --build source -o output}, plus the include and stdlib flags this project needs.
     *
     * @param debugHooks instruments for the interactive debugger; implies an unoptimized build,
     *                   because the optimizer would move or delete the hooks.
     */
    public static @NotNull Plan executable(@NotNull Project project, @NotNull Path source,
                                           @NotNull Path output, boolean release,
                                           @NotNull List<String> extraArguments, boolean debugHooks)
            throws ExecutionException {
        Path compiler = compilerFor(project, source);
        if (compiler == null) throw new ExecutionException(missingCompilerMessage());

        List<String> arguments = new ArrayList<>();
        arguments.add(source.toString());
        arguments.add("-o");
        arguments.add(output.toString());
        arguments.add("--build");
        if (debugHooks) {
            arguments.add("--debug-hooks");
        } else if (release) {
            arguments.add("--release");
        }
        arguments.addAll(commonArguments(project, source, compiler));
        arguments.addAll(extraArguments);
        return new Plan(compiler.toString(), arguments, workingDirectory(project, source), output);
    }

    /** {@code mettle test source} - runs the {@code @test} functions in the IR interpreter. */
    public static @NotNull Plan test(@NotNull Project project, @NotNull Path source,
                                     @NotNull List<String> extraArguments) throws ExecutionException {
        Path compiler = compilerFor(project, source);
        if (compiler == null) throw new ExecutionException(missingCompilerMessage());
        List<String> arguments = new ArrayList<>();
        arguments.add("test");
        arguments.add(source.toString());
        arguments.addAll(extraArguments);
        return new Plan(compiler.toString(), arguments, workingDirectory(project, source), source);
    }

    /**
     * {@code mettle --release --explain} - the optimization decision report.
     *
     * @param json also write the machine-readable {@code <stem>.explain.json} sidecar
     */
    public static @NotNull Plan explain(@NotNull Project project, @NotNull Path source,
                                        @NotNull List<String> extraArguments, boolean json)
            throws ExecutionException {
        Path compiler = compilerFor(project, source);
        if (compiler == null) throw new ExecutionException(missingCompilerMessage());
        Path output = explainOutput(source);
        List<String> arguments = new ArrayList<>();
        arguments.add("-i");
        arguments.add(source.toString());
        arguments.add("-o");
        arguments.add(output.toString());
        arguments.add("--release");
        arguments.add(json ? "--explain-json" : "--explain");
        arguments.addAll(commonArguments(project, source, compiler));
        arguments.addAll(extraArguments);
        Plan plan = new Plan(compiler.toString(), arguments, workingDirectory(project, source), output);
        output.getParent().toFile().mkdirs();
        return plan;
    }

    /**
     * Where an explain build writes. One stable directory per source path, so the compiler finds
     * its own {@code .explain.base} from the previous run and can report what changed.
     */
    public static @NotNull Path explainOutput(@NotNull Path source) {
        return Paths.get(System.getProperty("java.io.tmpdir"), "mettle-explain",
                Integer.toHexString(source.toString().hashCode()), stem(source) + ".obj");
    }

    /** The {@code --explain-json} sidecar that goes with {@link #explainOutput}. */
    public static @NotNull Path explainSidecar(@NotNull Path source) {
        Path output = explainOutput(source);
        return output.resolveSibling(stem(output) + ".explain.json");
    }

    private static @NotNull List<String> commonArguments(@NotNull Project project, @NotNull Path source,
                                                         @NotNull Path compiler) {
        List<String> arguments = new ArrayList<>();
        Path directory = source.getParent();
        if (directory != null) {
            arguments.add("-I");
            arguments.add(directory.toString());
        }
        if (project.getBasePath() != null) {
            arguments.add("-I");
            arguments.add(project.getBasePath());
        }
        MettleSettings settings = MettleSettings.getInstance(project);
        for (String include : settings.includePaths) {
            if (include == null || include.isBlank()) continue;
            Path path = Paths.get(include.trim());
            if (!path.isAbsolute() && project.getBasePath() != null) {
                path = Paths.get(project.getBasePath()).resolve(include.trim());
            }
            arguments.add("-I");
            arguments.add(path.toString());
        }
        Path stdlib = MettleToolchain.stdlibRoot(project, compiler);
        if (stdlib != null) {
            arguments.add("--stdlib");
            arguments.add(stdlib.toString());
        }
        return arguments;
    }

    public static @Nullable File workingDirectory(@NotNull Project project, @NotNull Path source) {
        if (project.getBasePath() != null) return new File(project.getBasePath());
        return source.getParent() == null ? null : source.getParent().toFile();
    }

    /** Runs a compile and returns its combined output; null means the process would not start. */
    public static @Nullable ProcessOutput run(@NotNull Plan plan, int timeoutMs) {
        return MettleProcess.run(plan.commandLine(), timeoutMs);
    }

    public static @NotNull String stem(@NotNull Path source) {
        String name = source.getFileName().toString();
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    public static @NotNull String missingCompilerMessage() {
        return "No Mettle compiler found. Set one in Settings | Languages & Frameworks | Mettle, "
                + "put " + MettleToolchain.executableName() + " in a bin/ directory above the source, "
                + "or add it to PATH.";
    }
}
