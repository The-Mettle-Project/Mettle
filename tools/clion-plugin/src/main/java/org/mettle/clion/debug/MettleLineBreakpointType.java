package org.mettle.clion.debug;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.xdebugger.breakpoints.XBreakpointProperties;
import com.intellij.xdebugger.breakpoints.XLineBreakpoint;
import com.intellij.xdebugger.breakpoints.XLineBreakpointType;
import com.intellij.xdebugger.evaluation.XDebuggerEditorsProvider;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleFileType;

public class MettleLineBreakpointType extends XLineBreakpointType<XBreakpointProperties> {

    public static final String ID = "mettle-line";

    public MettleLineBreakpointType() {
        super(ID, "Mettle line breakpoints");
    }

    @Override
    public boolean canPutAt(@NotNull VirtualFile file, int line, @NotNull Project project) {
        return file.getFileType() == MettleFileType.INSTANCE;
    }

    @Override
    public @Nullable XBreakpointProperties createBreakpointProperties(@NotNull VirtualFile file, int line) {
        return null;
    }

    @Override
    public @Nullable XDebuggerEditorsProvider getEditorsProvider(
            @NotNull XLineBreakpoint<XBreakpointProperties> breakpoint, @NotNull Project project) {
        return new MettleDebuggerEditorsProvider();
    }
}
