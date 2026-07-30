package org.mettle.clion.psi;

import com.intellij.lang.ASTNode;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import com.intellij.psi.util.PsiTreeUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

import java.util.ArrayList;
import java.util.List;

public final class MettlePsiUtil {
    private MettlePsiUtil() {
    }

    public static @Nullable PsiElement childOfType(@Nullable PsiElement parent, IElementType type) {
        if (parent == null) return null;
        ASTNode node = parent.getNode();
        if (node == null) return null;
        ASTNode child = node.findChildByType(type);
        return child == null ? null : child.getPsi();
    }

    public static @Nullable PsiElement childOfTypes(@Nullable PsiElement parent, TokenSet types) {
        if (parent == null) return null;
        ASTNode node = parent.getNode();
        if (node == null) return null;
        ASTNode child = node.findChildByType(types);
        return child == null ? null : child.getPsi();
    }

    public static @NotNull List<PsiElement> childrenOfType(@Nullable PsiElement parent, IElementType type) {
        List<PsiElement> result = new ArrayList<>();
        if (parent == null) return result;
        for (PsiElement child = parent.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (child.getNode() != null && child.getNode().getElementType() == type) {
                result.add(child);
            }
        }
        return result;
    }

    public static @NotNull List<PsiElement> childrenOfTypes(@Nullable PsiElement parent, TokenSet types) {
        List<PsiElement> result = new ArrayList<>();
        if (parent == null) return result;
        for (PsiElement child = parent.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (child.getNode() != null && types.contains(child.getNode().getElementType())) {
                result.add(child);
            }
        }
        return result;
    }

    public static @Nullable IElementType elementType(@Nullable PsiElement element) {
        return element == null || element.getNode() == null ? null : element.getNode().getElementType();
    }

    public static boolean isType(@Nullable PsiElement element, IElementType type) {
        return elementType(element) == type;
    }

    /** The identifier token that names a declaration, or null for anonymous ones. */
    public static @Nullable PsiElement nameIdentifier(@Nullable PsiElement declaration) {
        return childOfType(declaration, MettleTypes.IDENTIFIER);
    }

    /** The declared type of a {@code var} / field / parameter, as written. */
    public static @Nullable PsiElement declaredType(@Nullable PsiElement declaration) {
        return childOfType(declaration, MettleTypes.TYPE_REF);
    }

    /** The base name of a type reference: {@code Point} for {@code Point*[4]}. */
    public static @Nullable String typeBaseName(@Nullable PsiElement typeRef) {
        if (typeRef == null) return null;
        ASTNode node = typeRef.getNode();
        if (node == null) return null;
        ASTNode name = node.findChildByType(
                TokenSet.orSet(MettleTypes.BUILTIN_TYPES, TokenSet.create(MettleTypes.IDENTIFIER)));
        return name == null ? null : name.getText();
    }

    public static @Nullable MettleFile mettleFile(@Nullable PsiElement element) {
        PsiFile file = element == null ? null : element.getContainingFile();
        return file instanceof MettleFile ? (MettleFile) file : null;
    }

    /** The struct or impl a method belongs to, or null at top level. */
    public static @Nullable PsiElement enclosingTypeDeclaration(@Nullable PsiElement element) {
        PsiElement parent = element;
        while (parent != null && !(parent instanceof PsiFile)) {
            IElementType type = elementType(parent);
            if (type == MettleTypes.STRUCT_DECL || type == MettleTypes.IMPL_DECL) return parent;
            parent = parent.getParent();
        }
        return null;
    }

    public static @Nullable PsiElement enclosingFunction(@Nullable PsiElement element) {
        return PsiTreeUtil.findFirstParent(element, false, e -> {
            IElementType type = elementType(e);
            return type == MettleTypes.FUNCTION_DECL || type == MettleTypes.METHOD_DECL;
        });
    }

    /** Text of the first token of a declaration line, used for presentations. */
    public static @NotNull String signatureText(@Nullable PsiElement declaration) {
        if (declaration == null) return "";
        StringBuilder sb = new StringBuilder();
        for (PsiElement child = declaration.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = elementType(child);
            if (type == MettleTypes.BLOCK) break;
            String text = child.getText();
            if (text.isEmpty()) continue;
            if (sb.length() > 0 && needsSpace(sb.charAt(sb.length() - 1), text.charAt(0))) sb.append(' ');
            sb.append(text.replaceAll("\\s+", " ").trim());
        }
        return sb.toString().trim();
    }

    private static boolean needsSpace(char previous, char next) {
        if (Character.isWhitespace(previous) || Character.isWhitespace(next)) return false;
        boolean previousWord = Character.isLetterOrDigit(previous) || previous == '_';
        boolean nextWord = Character.isLetterOrDigit(next) || next == '_';
        return previousWord && nextWord;
    }
}
