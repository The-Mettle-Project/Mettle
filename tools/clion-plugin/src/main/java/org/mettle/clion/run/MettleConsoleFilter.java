package org.mettle.clion.run;

import com.intellij.execution.filters.Filter;
import com.intellij.execution.filters.OpenFileHyperlinkInfo;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Turns {@code app.mettle:6:20} into a link.
 *
 * <p>Covers compiler diagnostics ({@code --> app.mettle:6:20}), runtime crash tracebacks
 * ({@code #0 main at app.mettle:9:10}), the profile report and the IR dumps' {@code ; @file:line:col}.
 */
public class MettleConsoleFilter implements Filter {

    private static final Pattern LOCATION =
            Pattern.compile("([A-Za-z]:[\\\\/][^\\s:*?\"<>|]+?|[^\\s:*?\"<>|]+?)\\.mettle:(\\d+)(?::(\\d+))?");

    private final Project project;

    public MettleConsoleFilter(@NotNull Project project) {
        this.project = project;
    }

    @Override
    public @Nullable Result applyFilter(@NotNull String line, int entireLength) {
        Matcher matcher = LOCATION.matcher(line);
        List<ResultItem> items = new ArrayList<>();
        int lineStart = entireLength - line.length();
        while (matcher.find()) {
            VirtualFile file = resolve(matcher.group(1) + ".mettle");
            if (file == null) continue;
            int row = parse(matcher.group(2), 1) - 1;
            int column = parse(matcher.group(3), 1) - 1;
            items.add(new ResultItem(lineStart + matcher.start(), lineStart + matcher.end(),
                    new OpenFileHyperlinkInfo(project, file, Math.max(0, row), Math.max(0, column))));
        }
        return items.isEmpty() ? null : new Result(items);
    }

    private @Nullable VirtualFile resolve(@NotNull String reported) {
        String normalized = reported.replace('\\', '/');
        LocalFileSystem fileSystem = LocalFileSystem.getInstance();
        Path path = Paths.get(normalized);
        if (path.isAbsolute()) return fileSystem.findFileByPath(normalized);
        String base = project.getBasePath();
        if (base != null) {
            VirtualFile file = fileSystem.findFileByPath(base + "/" + normalized);
            if (file != null) return file;
        }
        return null;
    }

    private static int parse(@Nullable String text, int fallback) {
        if (text == null) return fallback;
        try {
            return Integer.parseInt(text);
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }
}
