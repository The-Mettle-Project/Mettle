package org.mettle.clion.editor;

import com.intellij.codeInsight.editorActions.SimpleTokenSetQuoteHandler;
import org.mettle.clion.lang.MettleTypes;

public class MettleQuoteHandler extends SimpleTokenSetQuoteHandler {
    public MettleQuoteHandler() {
        super(MettleTypes.STRING_LITERAL, MettleTypes.CHAR_LITERAL);
    }
}
