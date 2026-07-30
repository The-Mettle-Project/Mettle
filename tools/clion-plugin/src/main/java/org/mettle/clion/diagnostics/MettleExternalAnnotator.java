package org.mettle.clion.diagnostics;

import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.process.ProcessOutput;
import com.intellij.lang.annotation.AnnotationHolder;
import com.intellij.lang.annotation.ExternalAnnotator;
import com.intellij.lang.annotation.HighlightSeverity;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.TextRange;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.psi.MettleFile;
import org.mettle.clion.settings.MettleSettings;
import org.mettle.clion.settings.MettleToolchain;
import org.mettle.clion.util.MettleProcess;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Runs the real compiler for semantic diagnostics.
 *
 * <p>The compiler reads from disk, so a run only reflects the saved file. Rather than write the
 * editor buffer somewhere behind the user's back, an edited file keeps showing the last run's
 * findings until it is saved; syntax errors from the plugin's own parser stay live in the meantime.
 */
public class MettleExternalAnnotator extends ExternalAnnotator<MettleExternalAnnotator.Request,
        List<MettleDiagnostic>> {

    /** Last successful run per file, so editing does not blank the Problems view. */
    private static final Map<String, List<MettleDiagnostic>> CACHE = new ConcurrentHashMap<>();

    public static class Request {
        final String compilerPath;
        final List<String> arguments;
        final File workingDirectory;
        final String sourcePath;
        final int timeoutMs;
        final boolean stale;

        Request(String compilerPath, List<String> arguments, File workingDirectory,
                String sourcePath, int timeoutMs, boolean stale) {
            this.compilerPath = compilerPath;
            this.arguments = arguments;
            this.workingDirectory = workingDirectory;
            this.sourcePath = sourcePath;
            this.timeoutMs = timeoutMs;
            this.stale = stale;
        }
    }

    @Override
    public @Nullable Request collectInformation(@NotNull PsiFile file, @NotNull Editor editor, boolean hasErrors) {
        return collectInformation(file);
    }

    @Override
    public @Nullable Request collectInformation(@NotNull PsiFile file) {
        if (!(file instanceof MettleFile)) return null;
        Project project = file.getProject();
        MettleSettings settings = MettleSettings.getInstance(project);
        if (!settings.diagnosticsEnabled) return null;

        VirtualFile virtualFile = file.getVirtualFile();
        if (virtualFile == null || !virtualFile.isInLocalFileSystem()) return null;
        Path compiler = MettleToolchain.findCompiler(project, virtualFile);
        if (compiler == null) return null;

        Path source = Paths.get(virtualFile.getPath());
        Path sourceDirectory = source.getParent();
        boolean stale = FileDocumentManager.getInstance().isFileModified(virtualFile);

        Path output = outputPath(source);
        List<String> arguments = new ArrayList<>();
        arguments.add("--error-format=json");
        arguments.add("-i");
        arguments.add(source.toString());
        arguments.add("-o");
        arguments.add(output.toString());
        if (sourceDirectory != null) {
            arguments.add("-I");
            arguments.add(sourceDirectory.toString());
        }
        if (project.getBasePath() != null) {
            arguments.add("-I");
            arguments.add(project.getBasePath());
        }
        for (String include : settings.includePaths) {
            if (include == null || include.isBlank()) continue;
            arguments.add("-I");
            arguments.add(absolute(project, include.trim()));
        }
        Path stdlib = MettleToolchain.stdlibRoot(project, compiler);
        if (stdlib != null) {
            arguments.add("--stdlib");
            arguments.add(stdlib.toString());
        }
        arguments.addAll(MettleProcess.splitArguments(settings.extraDiagnosticArgs));

        File workingDirectory = project.getBasePath() != null
                ? new File(project.getBasePath())
                : (sourceDirectory == null ? null : sourceDirectory.toFile());
        return new Request(compiler.toString(), arguments, workingDirectory,
                source.toString(), settings.diagnosticsTimeoutMs, stale);
    }

    @Override
    public @Nullable List<MettleDiagnostic> doAnnotate(@Nullable Request request) {
        if (request == null) return null;
        if (request.stale) return CACHE.get(request.sourcePath);

        Path output = outputPath(Paths.get(request.sourcePath));
        try {
            if (output.getParent() != null) Files.createDirectories(output.getParent());
        } catch (Exception ignored) {
            // fall through: the compiler reports its own I/O error if the path is unusable
        }
        GeneralCommandLine commandLine =
                MettleProcess.commandLine(request.compilerPath, request.arguments, request.workingDirectory);
        ProcessOutput processOutput = MettleProcess.run(commandLine, request.timeoutMs);
        if (processOutput == null || processOutput.isTimeout()) return CACHE.get(request.sourcePath);

        List<MettleDiagnostic> diagnostics = new ArrayList<>();
        for (String line : (processOutput.getStderr() + "\n" + processOutput.getStdout()).split("\\R")) {
            String trimmed = line.trim();
            if (trimmed.isEmpty() || trimmed.charAt(0) != '{') continue;
            MettleDiagnostic diagnostic = MettleDiagnostic.fromJsonLine(trimmed);
            if (diagnostic != null) diagnostics.add(diagnostic);
        }
        deleteQuietly(output);
        CACHE.put(request.sourcePath, diagnostics);
        return diagnostics;
    }

    @Override
    public void apply(@NotNull PsiFile file, @Nullable List<MettleDiagnostic> diagnostics,
                      @NotNull AnnotationHolder holder) {
        if (diagnostics == null || diagnostics.isEmpty()) return;
        Document document = PsiDocumentManager.getInstance(file.getProject()).getDocument(file);
        if (document == null) return;
        VirtualFile virtualFile = file.getVirtualFile();
        String path = virtualFile == null ? null : virtualFile.getPath();

        for (MettleDiagnostic diagnostic : diagnostics) {
            if (!belongsTo(diagnostic, path, file.getName())) continue;
            TextRange range = rangeOf(document, diagnostic);
            if (range == null) continue;
            holder.newAnnotation(severityOf(diagnostic), diagnostic.displayMessage())
                    .range(range)
                    .tooltip(diagnostic.tooltip())
                    .create();
        }
    }

    // ------------------------------------------------------------- helpers

    private static boolean belongsTo(@NotNull MettleDiagnostic diagnostic, @Nullable String path,
                                     @NotNull String fileName) {
        if (diagnostic.file == null || diagnostic.file.isEmpty()) return true;
        String reported = diagnostic.file.replace('\\', '/');
        if (path != null && reported.equalsIgnoreCase(path)) return true;
        int slash = reported.lastIndexOf('/');
        String reportedName = slash < 0 ? reported : reported.substring(slash + 1);
        // A relative path from the compiler still identifies the file by name; imported modules
        // report their own name and are skipped, which matches how the compiler scopes warnings.
        return reportedName.equalsIgnoreCase(fileName)
                && (path == null || path.replace('\\', '/').endsWith(reported));
    }

    private static @Nullable TextRange rangeOf(@NotNull Document document, @NotNull MettleDiagnostic diagnostic) {
        int line = diagnostic.line - 1;
        if (line < 0 || line >= document.getLineCount()) return null;
        int lineStart = document.getLineStartOffset(line);
        int lineEnd = document.getLineEndOffset(line);
        int start = Math.min(lineStart + Math.max(0, diagnostic.column - 1), lineEnd);
        int end = diagnostic.length > 0 ? Math.min(start + diagnostic.length, lineEnd) : lineEnd;
        if (end <= start) {
            end = Math.min(start + 1, document.getTextLength());
            if (end <= start) return null;
        }
        return new TextRange(start, end);
    }

    private static @NotNull HighlightSeverity severityOf(@NotNull MettleDiagnostic diagnostic) {
        if (diagnostic.isError()) return HighlightSeverity.ERROR;
        if (diagnostic.isWarning()) return HighlightSeverity.WARNING;
        return HighlightSeverity.WEAK_WARNING;
    }

    /**
     * Diagnostics only need the front end, but the driver still wants an output path. It goes to
     * the temp directory: imports resolve against the input file, so the object's location is free.
     */
    private static @NotNull Path outputPath(@NotNull Path source) {
        String stem = source.getFileName().toString().replaceAll("[^A-Za-z0-9_.-]", "_");
        return Paths.get(System.getProperty("java.io.tmpdir"), "mettle-check")
                .resolve(Integer.toHexString(source.toString().hashCode()) + "-" + stem + ".obj");
    }

    private static void deleteQuietly(@NotNull Path path) {
        try {
            Files.deleteIfExists(path);
        } catch (Exception ignored) {
            // the object file is disposable; a locked one is cleaned up by the next run
        }
    }

    private static @NotNull String absolute(@NotNull Project project, @NotNull String candidate) {
        Path path = Paths.get(candidate);
        if (path.isAbsolute() || project.getBasePath() == null) return candidate;
        return Paths.get(project.getBasePath()).resolve(candidate).toString();
    }
}
