package org.mettle.clion.debug;

import com.intellij.icons.AllIcons;
import com.intellij.xdebugger.XExpression;
import com.intellij.xdebugger.frame.XCompositeNode;
import com.intellij.xdebugger.frame.XNamedValue;
import com.intellij.xdebugger.frame.XValueChildrenList;
import com.intellij.xdebugger.frame.XValueModifier;
import com.intellij.xdebugger.frame.XValueNode;
import com.intellij.xdebugger.frame.XValuePlace;
import com.intellij.xdebugger.frame.presentation.XRegularValuePresentation;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.Icon;
import java.util.List;

/**
 * One variable in the Variables pane.
 *
 * <p>Values are read - and written - through the live pointers the instrumentation registered, so
 * editing one here really does change the running program's state.
 */
public class MettleValue extends XNamedValue {

    private final MettleDebugProtocol protocol;
    private final int frameIndex;
    private final String path;
    private final String typeName;
    private final String value;
    private final boolean expandable;
    private final boolean parameter;

    MettleValue(@NotNull MettleDebugProtocol protocol, int frameIndex, @NotNull String name,
                @NotNull String path, @NotNull String typeName, @NotNull String value,
                boolean expandable, boolean parameter) {
        super(name);
        this.protocol = protocol;
        this.frameIndex = frameIndex;
        this.path = path;
        this.typeName = typeName;
        this.value = value;
        this.expandable = expandable;
        this.parameter = parameter;
    }

    /** Parses a {@code var\tname\ttype\tis_param\tkids\tvalue} line. */
    static @Nullable MettleValue fromLine(@NotNull MettleDebugProtocol protocol, int frameIndex,
                                          @NotNull String parentPath, @NotNull String line) {
        String[] fields = line.split("\t", -1);
        if (fields.length < 6 || !"var".equals(fields[0])) return null;
        String name = fields[1];
        String value = String.join("\t", List.of(fields).subList(5, fields.length));
        return new MettleValue(protocol, frameIndex, name, childPath(parentPath, name),
                fields[2], value, "1".equals(fields[4]), "1".equals(fields[3]));
    }

    /** Field paths are dotted; array elements append {@code [i]} directly. */
    static @NotNull String childPath(@NotNull String parentPath, @NotNull String name) {
        if (parentPath.isEmpty()) return name;
        return name.startsWith("[") ? parentPath + name : parentPath + "." + name;
    }

    @Override
    public void computePresentation(@NotNull XValueNode node, @NotNull XValuePlace place) {
        node.setPresentation(icon(), new XRegularValuePresentation(value, typeName), expandable);
    }

    private Icon icon() {
        if (expandable) return AllIcons.Debugger.Value;
        return parameter ? AllIcons.Nodes.Parameter : AllIcons.Debugger.Value;
    }

    @Override
    public void computeChildren(@NotNull XCompositeNode node) {
        if (!expandable) {
            node.addChildren(XValueChildrenList.EMPTY, true);
            return;
        }
        com.intellij.openapi.application.ApplicationManager.getApplication().executeOnPooledThread(() -> {
            XValueChildrenList children = new XValueChildrenList();
            for (String line : protocol.query("expand\t" + frameIndex + "\t" + path, "varsdone", 5000)) {
                MettleValue child = fromLine(protocol, frameIndex, path, line);
                if (child != null) children.add(child);
            }
            node.addChildren(children, true);
        });
    }

    @Override
    public @Nullable XValueModifier getModifier() {
        return new XValueModifier() {
            @Override
            public @Nullable String getInitialValueEditorText() {
                return value;
            }

            @Override
            public void setValue(@NotNull XExpression expression, @NotNull XModificationCallback callback) {
                com.intellij.openapi.application.ApplicationManager.getApplication()
                        .executeOnPooledThread(() -> {
                            String reply = protocol.queryOne(
                                    "set\t" + frameIndex + "\t" + path + "\t"
                                            + expression.getExpression().trim(), 5000);
                            String[] fields = reply == null ? new String[0] : reply.split("\t", -1);
                            if (fields.length >= 2 && "setr".equals(fields[0]) && "1".equals(fields[1])) {
                                callback.valueModified();
                            } else {
                                callback.errorOccurred("Cannot write '" + path
                                        + "': unsupported type, or not a variable in this frame.");
                            }
                        });
            }
        };
    }

    @Override
    public @Nullable String getEvaluationExpression() {
        return path;
    }
}
