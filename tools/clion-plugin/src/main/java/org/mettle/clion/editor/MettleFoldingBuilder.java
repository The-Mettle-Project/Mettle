package org.mettle.clion.editor;

import com.intellij.lang.ASTNode;
import com.intellij.lang.folding.FoldingBuilderEx;
import com.intellij.lang.folding.FoldingDescriptor;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.util.TextRange;
import com.intellij.psi.PsiElement;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettlePsiUtil;

import java.util.ArrayList;
import java.util.List;

/** Folds bodies, brace-delimited declarations, block comments and runs of imports. */
public class MettleFoldingBuilder extends FoldingBuilderEx {

    @Override
    public FoldingDescriptor @NotNull [] buildFoldRegions(@NotNull PsiElement root,
                                                          @NotNull Document document,
                                                          boolean quick) {
        List<FoldingDescriptor> descriptors = new ArrayList<>();
        collect(root, document, descriptors);
        collectImportRun(root, document, descriptors);
        return descriptors.toArray(FoldingDescriptor.EMPTY_ARRAY);
    }

    private void collect(@NotNull PsiElement element, @NotNull Document document,
                         @NotNull List<FoldingDescriptor> descriptors) {
        for (PsiElement child = element.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = MettlePsiUtil.elementType(child);
            if (type == MettleTypes.BLOCK_COMMENT) {
                addIfMultiline(descriptors, child.getNode(), child.getTextRange(), document);
                continue;
            }
            if (type == MettleTypes.BLOCK || type == MettleTypes.ASM_BLOCK) {
                addIfMultiline(descriptors, child.getNode(), child.getTextRange(), document);
            } else if (type == MettleTypes.STRUCT_DECL || type == MettleTypes.ENUM_DECL
                    || type == MettleTypes.TRAIT_DECL || type == MettleTypes.IMPL_DECL
                    || type == MettleTypes.SWITCH_STMT || type == MettleTypes.MATCH_STMT) {
                TextRange braces = braceRange(child);
                if (braces != null) addIfMultiline(descriptors, child.getNode(), braces, document);
            }
            collect(child, document, descriptors);
        }
    }

    /** All leading imports fold into one region, the way a language with a header block does. */
    private void collectImportRun(@NotNull PsiElement root, @NotNull Document document,
                                  @NotNull List<FoldingDescriptor> descriptors) {
        PsiElement first = null;
        PsiElement last = null;
        for (PsiElement child = root.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (MettlePsiUtil.elementType(child) == MettleTypes.IMPORT_DECL) {
                if (first == null) first = child;
                last = child;
            }
        }
        if (first == null || first == last) return;
        TextRange range = new TextRange(first.getTextRange().getStartOffset(),
                last.getTextRange().getEndOffset());
        addIfMultiline(descriptors, first.getNode(), range, document);
    }

    private static void addIfMultiline(@NotNull List<FoldingDescriptor> descriptors,
                                       @Nullable ASTNode node, @NotNull TextRange range,
                                       @NotNull Document document) {
        if (node == null || range.getLength() < 2) return;
        if (range.getEndOffset() > document.getTextLength()) return;
        if (document.getLineNumber(range.getStartOffset()) == document.getLineNumber(range.getEndOffset())) {
            return;
        }
        descriptors.add(new FoldingDescriptor(node, range));
    }

    private static @Nullable TextRange braceRange(@NotNull PsiElement element) {
        ASTNode node = element.getNode();
        if (node == null) return null;
        ASTNode open = node.findChildByType(MettleTypes.LBRACE);
        ASTNode close = node.findChildByType(MettleTypes.RBRACE);
        if (open == null || close == null) return null;
        return new TextRange(open.getStartOffset(), close.getStartOffset() + close.getTextLength());
    }

    @Override
    public @Nullable String getPlaceholderText(@NotNull ASTNode node) {
        IElementType type = node.getElementType();
        if (type == MettleTypes.BLOCK_COMMENT) return "/*...*/";
        if (type == MettleTypes.IMPORT_DECL) return "import ...";
        return "{...}";
    }

    @Override
    public boolean isCollapsedByDefault(@NotNull ASTNode node) {
        return false;
    }
}
