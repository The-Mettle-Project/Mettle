package org.mettle.clion.run;

import com.intellij.execution.Executor;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.execution.configurations.LocatableConfigurationBase;
import com.intellij.execution.configurations.RunConfiguration;
import com.intellij.execution.configurations.RunProfileState;
import com.intellij.execution.configurations.RuntimeConfigurationError;
import com.intellij.execution.configurations.RuntimeConfigurationException;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.JDOMExternalizerUtil;
import com.intellij.openapi.util.InvalidDataException;
import com.intellij.openapi.util.WriteExternalException;
import org.jdom.Element;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;

/** Builds one {@code .mettle} file and runs it, tests it, or reports what the optimizer did. */
public class MettleRunConfiguration extends LocatableConfigurationBase<Object> {

    public enum Mode {
        RUN("Build and run"),
        BUILD("Build only"),
        TEST("Run @test functions"),
        EXPLAIN("Optimization report (--explain)");

        private final String label;

        Mode(String label) {
            this.label = label;
        }

        public String label() {
            return label;
        }

        static Mode of(@Nullable String name) {
            for (Mode mode : values()) {
                if (mode.name().equals(name)) return mode;
            }
            return RUN;
        }
    }

    private String filePath = "";
    private String outputPath = "";
    private String workingDirectory = "";
    private String programArguments = "";
    private String compilerArguments = "";
    private boolean release = true;
    private boolean stopAtEntry = false;
    private Mode mode = Mode.RUN;

    public MettleRunConfiguration(@NotNull Project project, @NotNull ConfigurationFactory factory,
                                  @NotNull String name) {
        super(project, factory, name);
    }

    // ------------------------------------------------------------ settings

    public String getFilePath() {
        return filePath;
    }

    public void setFilePath(String filePath) {
        this.filePath = filePath == null ? "" : filePath;
    }

    public String getOutputPath() {
        return outputPath;
    }

    public void setOutputPath(String outputPath) {
        this.outputPath = outputPath == null ? "" : outputPath;
    }

    public String getWorkingDirectory() {
        return workingDirectory;
    }

    public void setWorkingDirectory(String workingDirectory) {
        this.workingDirectory = workingDirectory == null ? "" : workingDirectory;
    }

    public String getProgramArguments() {
        return programArguments;
    }

    public void setProgramArguments(String programArguments) {
        this.programArguments = programArguments == null ? "" : programArguments;
    }

    public String getCompilerArguments() {
        return compilerArguments;
    }

    public void setCompilerArguments(String compilerArguments) {
        this.compilerArguments = compilerArguments == null ? "" : compilerArguments;
    }

    public boolean isRelease() {
        return release;
    }

    public void setRelease(boolean release) {
        this.release = release;
    }

    /** Debug only: hold at the first line instead of running to the first breakpoint. */
    public boolean isStopAtEntry() {
        return stopAtEntry;
    }

    public void setStopAtEntry(boolean stopAtEntry) {
        this.stopAtEntry = stopAtEntry;
    }

    public Mode getMode() {
        return mode;
    }

    public void setMode(Mode mode) {
        this.mode = mode == null ? Mode.RUN : mode;
    }

    // ------------------------------------------------------------- derived

    public @Nullable Path source() {
        return filePath.isBlank() ? null : Paths.get(filePath);
    }

    /** Where {@code --build} writes: the configured path, or {@code <source stem>.exe} beside it. */
    public @NotNull Path executable() {
        if (!outputPath.isBlank()) return Paths.get(outputPath);
        Path source = source();
        if (source == null) return Paths.get("a.exe");
        String name = MettleBuild.stem(source) + (com.intellij.openapi.util.SystemInfo.isWindows ? ".exe" : "");
        Path directory = source.getParent();
        return directory == null ? Paths.get(name) : directory.resolve(name);
    }

    public @Nullable File resolvedWorkingDirectory() {
        if (!workingDirectory.isBlank()) return new File(workingDirectory);
        Path source = source();
        if (source != null && source.getParent() != null) return source.getParent().toFile();
        return getProject().getBasePath() == null ? null : new File(getProject().getBasePath());
    }

    public @NotNull List<String> compilerArgumentList() {
        return org.mettle.clion.util.MettleProcess.splitArguments(compilerArguments);
    }

    public @NotNull List<String> programArgumentList() {
        return org.mettle.clion.util.MettleProcess.splitArguments(programArguments);
    }

    // ------------------------------------------------------- run machinery

    @Override
    public @NotNull SettingsEditor<? extends RunConfiguration> getConfigurationEditor() {
        return new MettleRunConfigurationEditor(getProject());
    }

    @Override
    public @Nullable RunProfileState getState(@NotNull Executor executor,
                                              @NotNull ExecutionEnvironment environment) {
        return new MettleCommandLineState(environment, this);
    }

    @Override
    public void checkConfiguration() throws RuntimeConfigurationException {
        Path source = source();
        if (source == null) throw new RuntimeConfigurationError("No Mettle file selected");
        if (!Files.isRegularFile(source)) {
            throw new RuntimeConfigurationError("File not found: " + source);
        }
        if (MettleBuild.compilerFor(getProject(), source) == null) {
            throw new RuntimeConfigurationError(MettleBuild.missingCompilerMessage());
        }
    }

    @Override
    public @Nullable String suggestedName() {
        Path source = source();
        if (source == null) return null;
        String stem = MettleBuild.stem(source);
        switch (mode) {
            case TEST: return "Test " + stem;
            case BUILD: return "Build " + stem;
            case EXPLAIN: return "Explain " + stem;
            default: return stem;
        }
    }

    @Override
    public void readExternal(@NotNull Element element) throws InvalidDataException {
        super.readExternal(element);
        filePath = JDOMExternalizerUtil.readField(element, "filePath", "");
        outputPath = JDOMExternalizerUtil.readField(element, "outputPath", "");
        workingDirectory = JDOMExternalizerUtil.readField(element, "workingDirectory", "");
        programArguments = JDOMExternalizerUtil.readField(element, "programArguments", "");
        compilerArguments = JDOMExternalizerUtil.readField(element, "compilerArguments", "");
        release = Boolean.parseBoolean(JDOMExternalizerUtil.readField(element, "release", "true"));
        stopAtEntry = Boolean.parseBoolean(JDOMExternalizerUtil.readField(element, "stopAtEntry", "false"));
        mode = Mode.of(JDOMExternalizerUtil.readField(element, "mode", Mode.RUN.name()));
    }

    @Override
    public void writeExternal(@NotNull Element element) throws WriteExternalException {
        super.writeExternal(element);
        JDOMExternalizerUtil.writeField(element, "filePath", filePath);
        JDOMExternalizerUtil.writeField(element, "outputPath", outputPath);
        JDOMExternalizerUtil.writeField(element, "workingDirectory", workingDirectory);
        JDOMExternalizerUtil.writeField(element, "programArguments", programArguments);
        JDOMExternalizerUtil.writeField(element, "compilerArguments", compilerArguments);
        JDOMExternalizerUtil.writeField(element, "release", Boolean.toString(release));
        JDOMExternalizerUtil.writeField(element, "stopAtEntry", Boolean.toString(stopAtEntry));
        JDOMExternalizerUtil.writeField(element, "mode", mode.name());
    }
}
