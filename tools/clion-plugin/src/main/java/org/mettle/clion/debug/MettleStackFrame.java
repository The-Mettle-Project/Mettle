package org.mettle.clion.debug;

import com.intellij.icons.AllIcons;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.util.NlsSafe;
import com.intellij.ui.ColoredTextContainer;
import com.intellij.ui.SimpleTextAttributes;
import com.intellij.xdebugger.XSourcePosition;
import com.intellij.xdebugger.evaluation.XDebuggerEvaluator;
import com.intellij.xdebugger.frame.XCompositeNode;
import com.intellij.xdebugger.frame.XStackFrame;
import com.intellij.xdebugger.frame.XValueChildrenList;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

public class MettleStackFrame extends XStackFrame {

    private final MettleDebugProtocol protocol;
    private final int index;
    private final String functionName;
    private final @Nullable XSourcePosition position;

    MettleStackFrame(@NotNull MettleDebugProtocol protocol, int index, @NotNull String functionName,
                     @Nullable XSourcePosition position) {
        this.protocol = protocol;
        this.index = index;
        this.functionName = functionName;
        this.position = position;
    }

    @Override
    public @Nullable XSourcePosition getSourcePosition() {
        return position;
    }

    @Override
    public @Nullable Object getEqualityObject() {
        return functionName + "#" + index;
    }

    @Override
    public void customizePresentation(@NotNull ColoredTextContainer component) {
        component.append(functionName, SimpleTextAttributes.REGULAR_ATTRIBUTES);
        if (position != null) {
            @NlsSafe String location = " (" + position.getFile().getName() + ":" + (position.getLine() + 1) + ")";
            component.append(location, SimpleTextAttributes.GRAYED_ATTRIBUTES);
        }
        component.setIcon(AllIcons.Debugger.Frame);
    }

    @Override
    public void computeChildren(@NotNull XCompositeNode node) {
        ApplicationManager.getApplication().executeOnPooledThread(() -> {
            XValueChildrenList children = new XValueChildrenList();
            for (String line : protocol.query("vars\t" + index, "varsdone", 5000)) {
                MettleValue value = MettleValue.fromLine(protocol, index, "", line);
                if (value != null) children.add(value);
            }
            node.addChildren(children, true);
        });
    }

    @Override
    public @Nullable XDebuggerEvaluator getEvaluator() {
        return new MettleEvaluator(protocol, index);
    }
}
