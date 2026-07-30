package org.mettle.clion.debug;

import com.intellij.openapi.application.ApplicationManager;
import com.intellij.xdebugger.XSourcePosition;
import com.intellij.xdebugger.evaluation.XDebuggerEvaluator;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.List;
import java.util.regex.Pattern;

/**
 * Evaluates variable paths in the paused frame.
 *
 * <p>The runtime resolves {@code name(.field | ->field | [index])*} against its live pointers; it
 * is not an expression interpreter, so anything else is rejected with a clear message rather than
 * silently returning something wrong.
 */
public class MettleEvaluator extends XDebuggerEvaluator {

    private static final Pattern PATH = Pattern.compile(
            "[A-Za-z_][A-Za-z0-9_]*(\\s*(\\.|->)\\s*[A-Za-z_][A-Za-z0-9_]*|\\[\\d+])*");

    private final MettleDebugProtocol protocol;
    private final int frameIndex;

    MettleEvaluator(@NotNull MettleDebugProtocol protocol, int frameIndex) {
        this.protocol = protocol;
        this.frameIndex = frameIndex;
    }

    @Override
    public void evaluate(@NotNull String expression, @NotNull XEvaluationCallback callback,
                         @Nullable XSourcePosition expressionPosition) {
        String trimmed = expression.trim();
        if (!PATH.matcher(trimmed).matches()) {
            callback.errorOccurred("Evaluate accepts variable paths such as `box.min.x` or `grid[2]`.");
            return;
        }
        String path = trimmed.replaceAll("\\s+", "");
        ApplicationManager.getApplication().executeOnPooledThread(() -> {
            String reply = protocol.queryOne("eval\t" + frameIndex + "\t" + path, 5000);
            String[] fields = reply == null ? new String[0] : reply.split("\t", -1);
            if (fields.length >= 5 && "evalr".equals(fields[0]) && "1".equals(fields[1])) {
                String value = String.join("\t", List.of(fields).subList(4, fields.length));
                callback.evaluated(new MettleValue(protocol, frameIndex, path, path,
                        fields[2], value, "1".equals(fields[3]), false));
            } else {
                callback.errorOccurred("`" + trimmed + "` is not a variable in this frame.");
            }
        });
    }
}
