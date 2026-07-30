package org.mettle.clion.psi;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.psi.PsiFile;
import com.intellij.psi.PsiManager;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.settings.MettleSettings;
import org.mettle.clion.settings.MettleToolchain;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Resolves an {@code import "..."} path the way the compiler does (docs/imports.md): absolute,
 * then {@code std/} under the stdlib root, then {@code mettle.deps} package roots, then relative
 * to the importing file, then the {@code -I} directories, then the project root.
 */
public final class MettleImportResolver {

    private MettleImportResolver() {
    }

    public static @Nullable PsiFile resolve(@NotNull PsiFile importingFile, @NotNull String rawPath) {
        Path resolved = resolveToPath(importingFile, rawPath, true);
        if (resolved == null) return null;
        VirtualFile virtualFile = LocalFileSystem.getInstance().findFileByNioFile(resolved);
        if (virtualFile == null) return null;
        return PsiManager.getInstance(importingFile.getProject()).findFile(virtualFile);
    }

    /** @param addExtension {@code import} appends {@code .mettle}; {@code import_str} does not. */
    public static @Nullable Path resolveToPath(@NotNull PsiFile importingFile,
                                               @NotNull String rawPath,
                                               boolean addExtension) {
        String cleaned = rawPath.trim().replace('\\', '/');
        if (cleaned.isEmpty()) return null;
        Project project = importingFile.getProject();
        VirtualFile origin = importingFile.getVirtualFile();
        Path originDir = origin != null && origin.getParent() != null
                ? Paths.get(origin.getParent().getPath())
                : null;

        List<String> candidates = new ArrayList<>();
        candidates.add(cleaned);
        if (addExtension && !hasExtension(cleaned)) {
            candidates.add(0, cleaned + ".mettle");
        }

        for (String candidate : candidates) {
            Path absolute = tryPath(Paths.get(candidate));
            if (absolute != null) return absolute;

            if (candidate.startsWith("std/")) {
                Path stdlib = MettleToolchain.stdlibRoot(project, MettleToolchain.findCompiler(project, origin));
                if (stdlib != null) {
                    Path hit = tryPath(stdlib.resolve(candidate.substring("std/".length())));
                    if (hit != null) return hit;
                    hit = tryPath(stdlib.resolve(candidate));
                    if (hit != null) return hit;
                }
            }

            for (Map.Entry<String, Path> pkg : packageRoots(originDir).entrySet()) {
                String prefix = pkg.getKey() + "/";
                if (candidate.startsWith(prefix)) {
                    Path hit = tryPath(pkg.getValue().resolve(candidate.substring(prefix.length())));
                    if (hit != null) return hit;
                }
            }

            if (originDir != null) {
                Path hit = tryPath(originDir.resolve(candidate));
                if (hit != null) return hit;
            }

            for (String include : MettleSettings.getInstance(project).includePaths) {
                if (include == null || include.isBlank()) continue;
                Path base = Paths.get(include.trim());
                if (!base.isAbsolute() && project.getBasePath() != null) {
                    base = Paths.get(project.getBasePath()).resolve(include.trim());
                }
                Path hit = tryPath(base.resolve(candidate));
                if (hit != null) return hit;
            }

            if (project.getBasePath() != null) {
                Path hit = tryPath(Paths.get(project.getBasePath()).resolve(candidate));
                if (hit != null) return hit;
            }
        }
        return null;
    }

    /** {@code mettle.deps} entries found by walking up from the importing file. */
    private static @NotNull Map<String, Path> packageRoots(@Nullable Path from) {
        Map<String, Path> roots = new LinkedHashMap<>();
        for (Path dir = from; dir != null; dir = dir.getParent()) {
            Path deps = dir.resolve("mettle.deps");
            if (!Files.isRegularFile(deps)) continue;
            try {
                for (String line : Files.readAllLines(deps, StandardCharsets.UTF_8)) {
                    String trimmed = line.trim();
                    if (trimmed.isEmpty() || trimmed.startsWith("#")) continue;
                    int equals = trimmed.indexOf('=');
                    if (equals <= 0) continue;
                    String name = trimmed.substring(0, equals).trim();
                    String target = trimmed.substring(equals + 1).trim();
                    if (name.isEmpty() || target.isEmpty() || roots.containsKey(name)) continue;
                    Path root = Paths.get(target);
                    roots.put(name, root.isAbsolute() ? root : dir.resolve(target).normalize());
                }
            } catch (IOException | RuntimeException ignored) {
                // an unreadable deps file just contributes no roots
            }
        }
        return roots;
    }

    private static @Nullable Path tryPath(@NotNull Path path) {
        try {
            Path normalized = path.normalize();
            return Files.isRegularFile(normalized) ? normalized : null;
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    private static boolean hasExtension(@NotNull String path) {
        int slash = path.lastIndexOf('/');
        int dot = path.lastIndexOf('.');
        return dot > slash && dot < path.length() - 1;
    }
}
