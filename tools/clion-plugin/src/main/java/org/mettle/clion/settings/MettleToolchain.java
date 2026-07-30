package org.mettle.clion.settings;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.SystemInfo;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Finds the {@code mettle} executable and its stdlib.
 *
 * <p>The search mirrors the VS Code extension so both agree on which compiler a project uses:
 * the configured path first, then {@code bin/mettle} walking up from the source file, then PATH.
 */
public final class MettleToolchain {

    private MettleToolchain() {
    }

    public static @NotNull String executableName() {
        return SystemInfo.isWindows ? "mettle.exe" : "mettle";
    }

    /** The compiler to use for {@code contextFile}, or null when none can be found. */
    public static @Nullable Path findCompiler(@NotNull Project project, @Nullable VirtualFile contextFile) {
        MettleSettings settings = MettleSettings.getInstance(project);
        String configured = settings.compilerPath == null ? "" : settings.compilerPath.trim();
        if (!configured.isEmpty()) {
            Path path = Paths.get(configured);
            if (!path.isAbsolute()) {
                String base = project.getBasePath();
                if (base != null) path = Paths.get(base).resolve(configured);
            }
            return Files.isRegularFile(path) ? path.normalize() : null;
        }

        Path start = null;
        if (contextFile != null && contextFile.getParent() != null) {
            start = Paths.get(contextFile.getParent().getPath());
        } else if (project.getBasePath() != null) {
            start = Paths.get(project.getBasePath());
        }
        for (Path dir = start; dir != null; dir = dir.getParent()) {
            Path candidate = dir.resolve("bin").resolve(executableName());
            if (Files.isRegularFile(candidate)) return candidate.normalize();
        }
        return findOnPath(executableName());
    }

    /** The stdlib root to pass with {@code --stdlib}, or null to let the compiler decide. */
    public static @Nullable Path stdlibRoot(@NotNull Project project, @Nullable Path compiler) {
        MettleSettings settings = MettleSettings.getInstance(project);
        String configured = settings.stdlibPath == null ? "" : settings.stdlibPath.trim();
        if (!configured.isEmpty()) {
            Path path = Paths.get(configured);
            if (!path.isAbsolute() && project.getBasePath() != null) {
                path = Paths.get(project.getBasePath()).resolve(configured);
            }
            return Files.isDirectory(path) ? path.normalize() : null;
        }
        if (compiler != null && compiler.getParent() != null) {
            Path binSibling = compiler.getParent().resolve("stdlib");
            if (Files.isDirectory(binSibling)) return binSibling.normalize();
            Path parent = compiler.getParent().getParent();
            if (parent != null && Files.isDirectory(parent.resolve("stdlib"))) {
                return parent.resolve("stdlib").normalize();
            }
        }
        String base = project.getBasePath();
        if (base != null && Files.isDirectory(Paths.get(base, "stdlib"))) {
            return Paths.get(base, "stdlib").normalize();
        }
        return null;
    }

    private static @Nullable Path findOnPath(@NotNull String name) {
        String pathVariable = System.getenv("PATH");
        if (pathVariable == null) return null;
        for (String entry : pathVariable.split(File.pathSeparator)) {
            if (entry.isEmpty()) continue;
            try {
                Path candidate = Paths.get(entry).resolve(name);
                if (Files.isRegularFile(candidate)) return candidate.normalize();
            } catch (RuntimeException ignored) {
                // an unusable PATH entry is not worth failing over
            }
        }
        return null;
    }
}
