package org.mettle.clion.explain;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.process.ProcessOutput;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.progress.ProgressIndicator;
import com.intellij.openapi.progress.Task;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.util.messages.Topic;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.run.MettleBuild;
import org.mettle.clion.util.MettleProcess;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Owns the optimization reports: runs {@code --release --explain-json} for a file, keeps the last
 * result per file, and tells everyone who is showing it.
 *
 * <p>Each source gets its own output directory, so the compiler's {@code .explain.base} survives
 * between runs and every report after the first can say what changed since the last one.
 */
@Service(Service.Level.PROJECT)
public final class MettleExplainService {

    public interface Listener {
        void reportChanged(@NotNull VirtualFile file, @NotNull Snapshot snapshot);
    }

    public static final Topic<Listener> TOPIC =
            Topic.create("Mettle optimization report", Listener.class);

    /** What one explain build produced. */
    public static class Snapshot {
        public final @Nullable MettleExplainReport report;
        public final @NotNull String output;
        public final @Nullable String error;
        public final long finishedAt;

        Snapshot(@Nullable MettleExplainReport report, @NotNull String output, @Nullable String error) {
            this.report = report;
            this.output = output;
            this.error = error;
            this.finishedAt = System.currentTimeMillis();
        }

        public boolean isFailure() {
            return report == null;
        }
    }

    private final Project project;
    private final Map<String, Snapshot> snapshots = new ConcurrentHashMap<>();
    private final Map<String, AtomicBoolean> running = new ConcurrentHashMap<>();
    private volatile boolean autoRefresh = true;
    private volatile @Nullable VirtualFile shown;

    public MettleExplainService(@NotNull Project project) {
        this.project = project;
    }

    public static @NotNull MettleExplainService getInstance(@NotNull Project project) {
        return project.getService(MettleExplainService.class);
    }

    public @Nullable Snapshot snapshot(@Nullable VirtualFile file) {
        return file == null ? null : snapshots.get(file.getPath());
    }

    /** The file the tool window is currently showing, so save-triggered refreshes know the target. */
    public @Nullable VirtualFile shownFile() {
        return shown;
    }

    public void setShownFile(@Nullable VirtualFile file) {
        this.shown = file;
    }

    public boolean isAutoRefresh() {
        return autoRefresh;
    }

    public void setAutoRefresh(boolean autoRefresh) {
        this.autoRefresh = autoRefresh;
    }

    public boolean hasReportFor(@Nullable VirtualFile file) {
        return snapshot(file) != null;
    }

    /** Recompiles in the background and publishes the result. Repeat calls for a file coalesce. */
    public void refresh(@NotNull VirtualFile file) {
        AtomicBoolean lock = running.computeIfAbsent(file.getPath(), path -> new AtomicBoolean());
        if (!lock.compareAndSet(false, true)) return;
        shown = file;

        new Task.Backgroundable(project, "Mettle optimization report: " + file.getName(), true) {
            @Override
            public void run(@NotNull ProgressIndicator indicator) {
                indicator.setIndeterminate(true);
                indicator.setText("Compiling with --release --explain");
                Snapshot snapshot = compile(file);
                snapshots.put(file.getPath(), snapshot);
                ApplicationManager.getApplication().invokeLater(() -> {
                    if (project.isDisposed()) return;
                    project.getMessageBus().syncPublisher(TOPIC).reportChanged(file, snapshot);
                });
            }

            @Override
            public void onFinished() {
                lock.set(false);
            }
        }.queue();
    }

    private @NotNull Snapshot compile(@NotNull VirtualFile file) {
        Path source = Paths.get(file.getPath());
        MettleBuild.Plan plan;
        try {
            plan = MettleBuild.explain(project, source, List.of(), true);
        } catch (ExecutionException failure) {
            return new Snapshot(null, "", failure.getMessage());
        }

        Path sidecar = MettleBuild.explainSidecar(source);
        deleteQuietly(sidecar);

        // Keep the prose report on stderr instead of letting long ones divert to a file.
        ProcessOutput output = MettleProcess.run(
                plan.commandLine().withEnvironment("METTLE_EXPLAIN_REPORT_LINES", "0"), 120000);
        if (output == null) {
            return new Snapshot(null, "", "Could not start " + plan.compiler);
        }
        String text = (output.getStdout() + output.getStderr()).trim();

        String json = readQuietly(sidecar);
        MettleExplainReport report = json == null ? null : MettleExplainReport.parse(json);
        if (report == null) {
            String error = output.getExitCode() != 0
                    ? "The compiler could not build this file (exit code " + output.getExitCode() + ")."
                    : "The compiler produced no --explain-json sidecar.";
            return new Snapshot(null, text, error);
        }
        return new Snapshot(report, text, null);
    }

    private static @Nullable String readQuietly(@NotNull Path path) {
        try {
            return Files.isRegularFile(path)
                    ? new String(Files.readAllBytes(path), StandardCharsets.UTF_8) : null;
        } catch (Exception ignored) {
            return null;
        }
    }

    private static void deleteQuietly(@NotNull Path path) {
        try {
            Files.deleteIfExists(path);
        } catch (Exception ignored) {
            // a stale sidecar is caught by the "no sidecar" branch instead
        }
    }
}
