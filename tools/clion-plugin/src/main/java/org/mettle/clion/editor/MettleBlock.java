package org.mettle.clion.editor;

import com.intellij.formatting.Block;
import com.intellij.formatting.Indent;
import com.intellij.formatting.Spacing;
import com.intellij.formatting.SpacingBuilder;
import com.intellij.lang.ASTNode;
import com.intellij.psi.TokenType;
import com.intellij.psi.formatter.common.AbstractBlock;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

import java.util.ArrayList;
import java.util.List;

/**
 * Indentation is structural: anything between a {@code {} and its {@code }} is one level in, and
 * the statements of a {@code case} arm are one level inside the arm.
 */
public class MettleBlock extends AbstractBlock {

    /** Nodes whose children between the braces are indented. */
    private static final TokenSet BRACED = TokenSet.create(
            MettleTypes.BLOCK, MettleTypes.STRUCT_DECL, MettleTypes.ENUM_DECL,
            MettleTypes.TRAIT_DECL, MettleTypes.IMPL_DECL, MettleTypes.SWITCH_STMT,
            MettleTypes.MATCH_STMT, MettleTypes.ASM_BLOCK, MettleTypes.AGGREGATE_EXPR);

    private final SpacingBuilder spacingBuilder;
    private final Indent indent;

    MettleBlock(@NotNull ASTNode node, @Nullable Indent indent, @NotNull SpacingBuilder spacingBuilder) {
        super(node, null, null);
        this.indent = indent;
        this.spacingBuilder = spacingBuilder;
    }

    @Override
    protected List<Block> buildChildren() {
        List<Block> blocks = new ArrayList<>();
        IElementType parentType = myNode.getElementType();
        boolean braced = BRACED.contains(parentType);
        boolean inCase = parentType == MettleTypes.CASE_CLAUSE;
        boolean insideBraces = false;
        boolean pastColon = false;

        for (ASTNode child = myNode.getFirstChildNode(); child != null; child = child.getTreeNext()) {
            IElementType type = child.getElementType();
            if (type == TokenType.WHITE_SPACE || child.getTextLength() == 0) continue;

            Indent childIndent = Indent.getNoneIndent();
            if (braced) {
                if (type == MettleTypes.LBRACE) {
                    insideBraces = true;
                } else if (type == MettleTypes.RBRACE) {
                    insideBraces = false;
                } else if (insideBraces) {
                    childIndent = Indent.getNormalIndent();
                }
            } else if (inCase) {
                if (pastColon) childIndent = Indent.getNormalIndent();
                if (type == MettleTypes.COLON) pastColon = true;
            } else if (isUnbracedBody(parentType, type)) {
                childIndent = Indent.getNormalIndent();
            }
            blocks.add(new MettleBlock(child, childIndent, spacingBuilder));
        }
        return blocks;
    }

    /** {@code if (c) return 0;} - a one-statement body without braces still indents. */
    private static boolean isUnbracedBody(IElementType parentType, IElementType childType) {
        boolean bodyOwner = parentType == MettleTypes.IF_STMT || parentType == MettleTypes.WHILE_STMT
                || parentType == MettleTypes.FOR_STMT;
        if (!bodyOwner) return false;
        return childType == MettleTypes.EXPR_STMT || childType == MettleTypes.ASSIGN_STMT
                || childType == MettleTypes.RETURN_STMT || childType == MettleTypes.BREAK_STMT
                || childType == MettleTypes.CONTINUE_STMT;
    }

    @Override
    public @Nullable Indent getIndent() {
        return indent;
    }

    @Override
    public @Nullable Spacing getSpacing(@Nullable Block child1, @NotNull Block child2) {
        return spacingBuilder.getSpacing(this, child1, child2);
    }

    @Override
    protected @Nullable Indent getChildIndent() {
        IElementType type = myNode.getElementType();
        if (BRACED.contains(type) || type == MettleTypes.CASE_CLAUSE) return Indent.getNormalIndent();
        return Indent.getNoneIndent();
    }

    @Override
    public boolean isLeaf() {
        return myNode.getFirstChildNode() == null;
    }
}
