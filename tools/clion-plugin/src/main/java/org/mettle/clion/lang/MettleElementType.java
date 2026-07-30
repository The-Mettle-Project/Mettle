package org.mettle.clion.lang;

import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NonNls;
import org.jetbrains.annotations.NotNull;

public class MettleElementType extends IElementType {
    public MettleElementType(@NonNls @NotNull String debugName) {
        super(debugName, MettleLanguage.INSTANCE);
    }
}
