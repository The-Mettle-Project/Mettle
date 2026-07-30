package org.mettle.clion.lang;

import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import com.intellij.lang.ParserDefinition;
import com.intellij.lang.PsiParser;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.project.Project;
import com.intellij.psi.FileViewProvider;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettleExpressions;
import org.mettle.clion.psi.MettleFile;

public class MettleParserDefinition implements ParserDefinition {

    public static final IFileElementType FILE = new IFileElementType(MettleLanguage.INSTANCE);

    private static final TokenSet WHITE_SPACE = TokenSet.create(TokenType.WHITE_SPACE);

    private static final TokenSet NAMED_DECLARATIONS = TokenSet.create(
            MettleTypes.FUNCTION_DECL, MettleTypes.METHOD_DECL, MettleTypes.STRUCT_DECL,
            MettleTypes.ENUM_DECL, MettleTypes.TRAIT_DECL, MettleTypes.IMPL_DECL,
            MettleTypes.FIELD_DECL, MettleTypes.ENUM_MEMBER, MettleTypes.PARAM_DECL,
            MettleTypes.VAR_DECL, MettleTypes.CONST_DECL, MettleTypes.TYPE_PARAM);

    @Override
    public @NotNull Lexer createLexer(Project project) {
        return new MettleLexer();
    }

    @Override
    public @NotNull PsiParser createParser(Project project) {
        return new MettleParser();
    }

    @Override
    public @NotNull IFileElementType getFileNodeType() {
        return FILE;
    }

    @Override
    public @NotNull TokenSet getWhitespaceTokens() {
        return WHITE_SPACE;
    }

    @Override
    public @NotNull TokenSet getCommentTokens() {
        return MettleTypes.COMMENTS;
    }

    @Override
    public @NotNull TokenSet getStringLiteralElements() {
        return MettleTypes.STRINGS;
    }

    @Override
    public @NotNull PsiElement createElement(ASTNode node) {
        IElementType type = node.getElementType();
        if (NAMED_DECLARATIONS.contains(type)) return new MettleDeclaration(node);
        if (type == MettleTypes.REF_EXPR) return new MettleExpressions.RefExpr(node);
        if (type == MettleTypes.FIELD_EXPR) return new MettleExpressions.FieldExpr(node);
        if (type == MettleTypes.TYPE_REF) return new MettleExpressions.TypeRef(node);
        if (type == MettleTypes.IMPORT_DECL) return new MettleExpressions.ImportDecl(node);
        return new ASTWrapperPsiElement(node);
    }

    @Override
    public @NotNull PsiFile createFile(@NotNull FileViewProvider viewProvider) {
        return new MettleFile(viewProvider);
    }
}
