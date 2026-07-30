package org.mettle.clion.lang;

import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NonNls;
import org.jetbrains.annotations.NotNull;

public class MettleTokenType extends IElementType {
    public MettleTokenType(@NonNls @NotNull String debugName) {
        super(debugName, MettleLanguage.INSTANCE);
    }

    @Override
    public String toString() {
        return "Mettle:" + super.toString();
    }
}
