package org.mettle.clion.debug;

import com.intellij.xdebugger.frame.XExecutionStack;
import com.intellij.xdebugger.frame.XStackFrame;
import com.intellij.xdebugger.frame.XSuspendContext;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.List;

/** The paused program: one thread, the frames already collected from the runtime. */
public class MettleSuspendContext extends XSuspendContext {

    private final Stack stack;

    MettleSuspendContext(@NotNull List<MettleStackFrame> frames) {
        this.stack = new Stack(frames);
    }

    @Override
    public @Nullable XExecutionStack getActiveExecutionStack() {
        return stack;
    }

    @Override
    public XExecutionStack @NotNull [] getExecutionStacks() {
        return new XExecutionStack[]{stack};
    }

    static class Stack extends XExecutionStack {
        private final List<MettleStackFrame> frames;

        Stack(@NotNull List<MettleStackFrame> frames) {
            super("main");
            this.frames = frames;
        }

        @Override
        public @Nullable XStackFrame getTopFrame() {
            return frames.isEmpty() ? null : frames.get(0);
        }

        @Override
        public void computeStackFrames(int firstFrameIndex, @NotNull XStackFrameContainer container) {
            if (firstFrameIndex >= frames.size()) {
                container.addStackFrames(List.of(), true);
                return;
            }
            container.addStackFrames(frames.subList(firstFrameIndex, frames.size()), true);
        }
    }
}
