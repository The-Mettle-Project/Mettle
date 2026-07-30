package org.mettle.clion.editor;

import com.intellij.lang.BracePair;
import com.intellij.lang.PairedBraceMatcher;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

public class MettleBraceMatcher implements PairedBraceMatcher {

    private static final BracePair[] PAIRS = {
            new BracePair(MettleTypes.LBRACE, MettleTypes.RBRACE, true),
            new BracePair(MettleTypes.LPAREN, MettleTypes.RPAREN, false),
            new BracePair(MettleTypes.LBRACKET, MettleTypes.RBRACKET, false),
    };

    @Override
    public BracePair[] getPairs() {
        return PAIRS;
    }

    @Override
    public boolean isPairedBracesAllowedBeforeType(IElementType lbraceType, @Nullable IElementType next) {
        return next == null
                || next == MettleTypes.SEMICOLON
                || next == MettleTypes.COMMA
                || next == MettleTypes.RPAREN
                || next == MettleTypes.RBRACKET
                || next == MettleTypes.RBRACE
                || MettleTypes.COMMENTS.contains(next);
    }

    @Override
    public int getCodeConstructStart(PsiFile file, int openingBraceOffset) {
        return openingBraceOffset;
    }
}
