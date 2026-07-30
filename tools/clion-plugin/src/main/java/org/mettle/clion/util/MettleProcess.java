package org.mettle.clion.util;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.configurations.GeneralCommandLine;
import com.intellij.execution.process.CapturingProcessHandler;
import com.intellij.execution.process.ProcessOutput;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Runs a command and waits for it, with the platform's process plumbing. */
public final class MettleProcess {

    private MettleProcess() {
    }

    public static @NotNull GeneralCommandLine commandLine(@NotNull String exePath,
                                                          @NotNull List<String> arguments,
                                                          @Nullable File workingDirectory) {
        GeneralCommandLine commandLine = new GeneralCommandLine();
        commandLine.setExePath(exePath);
        commandLine.addParameters(arguments);
        commandLine.setCharset(StandardCharsets.UTF_8);
        if (workingDirectory != null) commandLine.setWorkDirectory(workingDirectory);
        // The compiler colours its output when stderr looks like a terminal; plain text parses better.
        commandLine.getEnvironment().put("NO_COLOR", "1");
        return commandLine;
    }

    /** Splits a command-line string the way a shell would, honouring double quotes. */
    public static @NotNull List<String> splitArguments(@Nullable String text) {
        List<String> arguments = new java.util.ArrayList<>();
        if (text == null || text.isBlank()) return arguments;
        StringBuilder current = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '"') {
                quoted = !quoted;
            } else if (Character.isWhitespace(c) && !quoted) {
                if (current.length() > 0) {
                    arguments.add(current.toString());
                    current.setLength(0);
                }
            } else {
                current.append(c);
            }
        }
        if (current.length() > 0) arguments.add(current.toString());
        return arguments;
    }

    /** Runs to completion, or returns null if the process could not be started. */
    public static @Nullable ProcessOutput run(@NotNull GeneralCommandLine commandLine, int timeoutMs) {
        try {
            CapturingProcessHandler handler = new CapturingProcessHandler(commandLine);
            return handler.runProcess(Math.max(1000, timeoutMs), true);
        } catch (ExecutionException failure) {
            return null;
        }
    }
}
