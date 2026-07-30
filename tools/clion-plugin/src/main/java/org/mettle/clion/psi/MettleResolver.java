package org.mettle.clion.psi;

import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleTypes;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Consumer;

/**
 * Name resolution: locals and parameters first, then this file's top-level declarations, then
 * everything reachable through {@code import}.
 *
 * <p>Mettle requires declare-before-use for bindings, so locals only match when they are declared
 * ahead of the reference; top-level declarations are visible anywhere in the file.
 */
public final class MettleResolver {

    private static final int MAX_FILES = 64;
    private static final int MAX_TYPE_DEPTH = 12;

    private static final TokenSet EXPRESSIONS = TokenSet.create(
            MettleTypes.BINARY_EXPR, MettleTypes.UNARY_EXPR, MettleTypes.CAST_EXPR,
            MettleTypes.CALL_EXPR, MettleTypes.INDEX_EXPR, MettleTypes.FIELD_EXPR,
            MettleTypes.PAREN_EXPR, MettleTypes.REF_EXPR, MettleTypes.LITERAL_EXPR,
            MettleTypes.NEW_EXPR, MettleTypes.AGGREGATE_EXPR, MettleTypes.RANGE_EXPR);

    private static final TokenSet FILE_LEVEL = TokenSet.create(
            MettleTypes.FUNCTION_DECL, MettleTypes.STRUCT_DECL, MettleTypes.ENUM_DECL,
            MettleTypes.TRAIT_DECL, MettleTypes.VAR_DECL, MettleTypes.CONST_DECL);

    private static final TokenSet LOCAL_SCOPES = TokenSet.create(
            MettleTypes.BLOCK, MettleTypes.FOR_STMT, MettleTypes.CASE_CLAUSE,
            MettleTypes.FUNCTION_DECL, MettleTypes.METHOD_DECL);

    private MettleResolver() {
    }

    // ------------------------------------------------------------- symbols

    public static @Nullable PsiElement resolveSymbol(@NotNull PsiElement context, @NotNull String name) {
        PsiElement local = resolveLocal(context, name);
        if (local != null) return local;

        MettleFile file = MettlePsiUtil.mettleFile(context);
        if (file == null) return null;
        for (PsiFile reachable : reachableFiles(file)) {
            PsiElement hit = findFileLevel(reachable, name);
            if (hit != null) return hit;
        }
        return null;
    }

    /** Every declaration a reference at {@code context} could name, for completion. */
    public static void forEachVisible(@NotNull PsiElement context, @NotNull Consumer<MettleDeclaration> sink) {
        int offset = context.getTextRange().getStartOffset();
        for (PsiElement scope = context.getParent(); scope != null; scope = scope.getParent()) {
            if (scope instanceof PsiFile) break;
            if (!LOCAL_SCOPES.contains(MettlePsiUtil.elementType(scope))) continue;
            for (MettleDeclaration declaration : scopeDeclarations(scope)) {
                if (declaration.getTextRange().getStartOffset() < offset || isParameter(declaration)) {
                    sink.accept(declaration);
                }
            }
        }
        MettleFile file = MettlePsiUtil.mettleFile(context);
        if (file == null) return;
        for (PsiFile reachable : reachableFiles(file)) {
            forEachFileLevel(reachable, sink);
        }
    }

    private static boolean isParameter(@NotNull MettleDeclaration declaration) {
        IElementType kind = declaration.getKind();
        return kind == MettleTypes.PARAM_DECL || kind == MettleTypes.TYPE_PARAM;
    }

    private static @Nullable PsiElement resolveLocal(@NotNull PsiElement context, @NotNull String name) {
        int offset = context.getTextRange().getStartOffset();
        for (PsiElement scope = context.getParent(); scope != null; scope = scope.getParent()) {
            if (scope instanceof PsiFile) break;
            if (!LOCAL_SCOPES.contains(MettlePsiUtil.elementType(scope))) continue;
            MettleDeclaration best = null;
            for (MettleDeclaration declaration : scopeDeclarations(scope)) {
                if (!name.equals(declaration.getName())) continue;
                if (declaration.getTextRange().getStartOffset() < offset || isParameter(declaration)) {
                    best = declaration;
                }
            }
            if (best != null) return best;
        }
        return null;
    }

    /** Declarations introduced directly by one scope. */
    private static @NotNull List<MettleDeclaration> scopeDeclarations(@NotNull PsiElement scope) {
        List<MettleDeclaration> result = new ArrayList<>();
        for (PsiElement child = scope.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = MettlePsiUtil.elementType(child);
            if (child instanceof MettleDeclaration
                    && (type == MettleTypes.VAR_DECL || type == MettleTypes.CONST_DECL)) {
                result.add((MettleDeclaration) child);
            } else if (type == MettleTypes.PARAM_LIST || type == MettleTypes.TYPE_PARAM_LIST) {
                for (PsiElement param : child.getChildren()) {
                    if (param instanceof MettleDeclaration) result.add((MettleDeclaration) param);
                }
            }
        }
        return result;
    }

    private static @Nullable PsiElement findFileLevel(@NotNull PsiFile file, @NotNull String name) {
        for (PsiElement child = file.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = MettlePsiUtil.elementType(child);
            if (FILE_LEVEL.contains(type) && child instanceof MettleDeclaration
                    && name.equals(((MettleDeclaration) child).getName())) {
                return child;
            }
            if (type == MettleTypes.ENUM_DECL) {
                for (PsiElement member : MettlePsiUtil.childrenOfType(child, MettleTypes.ENUM_MEMBER)) {
                    if (member instanceof MettleDeclaration
                            && name.equals(((MettleDeclaration) member).getName())) {
                        return member;
                    }
                }
            }
        }
        return null;
    }

    private static void forEachFileLevel(@NotNull PsiFile file, @NotNull Consumer<MettleDeclaration> sink) {
        for (PsiElement child = file.getFirstChild(); child != null; child = child.getNextSibling()) {
            IElementType type = MettlePsiUtil.elementType(child);
            if (FILE_LEVEL.contains(type) && child instanceof MettleDeclaration) {
                sink.accept((MettleDeclaration) child);
            }
            if (type == MettleTypes.ENUM_DECL) {
                for (PsiElement member : MettlePsiUtil.childrenOfType(child, MettleTypes.ENUM_MEMBER)) {
                    if (member instanceof MettleDeclaration) sink.accept((MettleDeclaration) member);
                }
            }
        }
    }

    // --------------------------------------------------------------- types

    public static @Nullable PsiElement resolveTypeName(@NotNull PsiElement context, @NotNull String typeName) {
        // a generic parameter shadows a top-level type of the same name
        for (PsiElement scope = context.getParent(); scope != null; scope = scope.getParent()) {
            if (scope instanceof PsiFile) break;
            PsiElement list = MettlePsiUtil.childOfType(scope, MettleTypes.TYPE_PARAM_LIST);
            for (PsiElement param : MettlePsiUtil.childrenOfType(list, MettleTypes.TYPE_PARAM)) {
                if (param instanceof MettleDeclaration && typeName.equals(((MettleDeclaration) param).getName())) {
                    return param;
                }
            }
        }
        MettleFile file = MettlePsiUtil.mettleFile(context);
        if (file == null) return null;
        for (PsiFile reachable : reachableFiles(file)) {
            for (PsiElement child = reachable.getFirstChild(); child != null; child = child.getNextSibling()) {
                IElementType type = MettlePsiUtil.elementType(child);
                if ((type == MettleTypes.STRUCT_DECL || type == MettleTypes.ENUM_DECL
                        || type == MettleTypes.TRAIT_DECL)
                        && child instanceof MettleDeclaration
                        && typeName.equals(((MettleDeclaration) child).getName())) {
                    return child;
                }
            }
        }
        return null;
    }

    /** A field, method or enum variant of a named type - including methods added by {@code impl}. */
    public static @Nullable PsiElement resolveMember(@Nullable PsiElement typeDeclaration,
                                                     @NotNull String memberName) {
        if (typeDeclaration == null) return null;
        for (PsiElement member : membersOf(typeDeclaration)) {
            if (member instanceof MettleDeclaration
                    && memberName.equals(((MettleDeclaration) member).getName())) {
                return member;
            }
        }
        return null;
    }

    public static @NotNull List<PsiElement> membersOf(@Nullable PsiElement typeDeclaration) {
        List<PsiElement> members = new ArrayList<>();
        if (typeDeclaration == null) return members;
        members.addAll(MettlePsiUtil.childrenOfTypes(typeDeclaration, TokenSet.create(
                MettleTypes.FIELD_DECL, MettleTypes.METHOD_DECL, MettleTypes.ENUM_MEMBER)));

        String typeName = typeDeclaration instanceof MettleDeclaration
                ? ((MettleDeclaration) typeDeclaration).getName() : null;
        MettleFile file = MettlePsiUtil.mettleFile(typeDeclaration);
        if (typeName == null || file == null) return members;

        for (PsiFile reachable : reachableFiles(file)) {
            for (PsiElement impl : MettlePsiUtil.childrenOfType(reachable, MettleTypes.IMPL_DECL)) {
                List<PsiElement> types = MettlePsiUtil.childrenOfType(impl, MettleTypes.TYPE_REF);
                PsiElement target = types.isEmpty() ? null : types.get(types.size() - 1);
                if (target != null && typeName.equals(MettlePsiUtil.typeBaseName(target))) {
                    members.addAll(MettlePsiUtil.childrenOfType(impl, MettleTypes.METHOD_DECL));
                    members.addAll(MettlePsiUtil.childrenOfType(impl, MettleTypes.FUNCTION_DECL));
                }
            }
        }
        return members;
    }

    /**
     * The base type name an expression produces: {@code Point} for {@code p}, {@code p->next},
     * {@code grid[3]} or {@code (Point*)raw}. Pointer and array decoration is dropped, which is
     * exactly what member lookup needs.
     */
    public static @Nullable String inferBaseTypeName(@Nullable PsiElement expression) {
        return inferBaseTypeName(expression, 0);
    }

    private static @Nullable String inferBaseTypeName(@Nullable PsiElement expression, int depth) {
        if (expression == null || depth > MAX_TYPE_DEPTH) return null;
        IElementType type = MettlePsiUtil.elementType(expression);

        if (type == MettleTypes.PAREN_EXPR || type == MettleTypes.UNARY_EXPR) {
            return inferBaseTypeName(firstExpression(expression), depth + 1);
        }
        if (type == MettleTypes.INDEX_EXPR) {
            return inferBaseTypeName(firstExpression(expression), depth + 1);
        }
        if (type == MettleTypes.CAST_EXPR || type == MettleTypes.NEW_EXPR) {
            return MettlePsiUtil.typeBaseName(MettlePsiUtil.childOfType(expression, MettleTypes.TYPE_REF));
        }
        if (type == MettleTypes.REF_EXPR) {
            if (expression.getFirstChild() != null
                    && MettlePsiUtil.elementType(expression.getFirstChild()) == MettleTypes.KW_THIS) {
                PsiElement owner = MettlePsiUtil.enclosingTypeDeclaration(expression);
                return owner instanceof MettleDeclaration ? ((MettleDeclaration) owner).getName() : null;
            }
            PsiElement declaration = resolveSymbol(expression, expression.getText());
            return declaredTypeName(declaration);
        }
        if (type == MettleTypes.FIELD_EXPR) {
            PsiElement member = resolveFieldExpression(expression);
            return declaredTypeName(member);
        }
        if (type == MettleTypes.CALL_EXPR) {
            PsiElement callee = firstExpression(expression);
            PsiElement function = resolveCallee(callee);
            return function == null ? null
                    : MettlePsiUtil.typeBaseName(MettlePsiUtil.childOfType(function, MettleTypes.TYPE_REF));
        }
        return null;
    }

    /** The member a {@code a.b} / {@code a->b} expression names. */
    public static @Nullable PsiElement resolveFieldExpression(@NotNull PsiElement fieldExpression) {
        PsiElement receiver = firstExpression(fieldExpression);
        PsiElement last = fieldExpression.getLastChild();
        if (last == null) return null;
        String memberName = last.getText();
        String typeName = inferBaseTypeName(receiver, 0);
        if (typeName == null || memberName.isEmpty()) return null;
        return resolveMember(resolveTypeName(fieldExpression, typeName), memberName);
    }

    private static @Nullable PsiElement resolveCallee(@Nullable PsiElement callee) {
        if (callee == null) return null;
        IElementType type = MettlePsiUtil.elementType(callee);
        if (type == MettleTypes.REF_EXPR) return resolveSymbol(callee, callee.getText());
        if (type == MettleTypes.FIELD_EXPR) return resolveFieldExpression(callee);
        return null;
    }

    private static @Nullable String declaredTypeName(@Nullable PsiElement declaration) {
        if (declaration == null) return null;
        return MettlePsiUtil.typeBaseName(MettlePsiUtil.childOfType(declaration, MettleTypes.TYPE_REF));
    }

    public static @Nullable PsiElement firstExpression(@Nullable PsiElement parent) {
        if (parent == null) return null;
        for (PsiElement child = parent.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (EXPRESSIONS.contains(MettlePsiUtil.elementType(child))) return child;
        }
        return null;
    }

    // --------------------------------------------------------------- files

    /**
     * This file plus everything it imports, transitively.
     *
     * <p>Cached per file: resolution runs on every keystroke and each import costs a few file
     * system probes.
     */
    public static @NotNull List<PsiFile> reachableFiles(@NotNull PsiFile start) {
        return com.intellij.psi.util.CachedValuesManager.getCachedValue(start,
                () -> com.intellij.psi.util.CachedValueProvider.Result.create(
                        computeReachableFiles(start),
                        com.intellij.psi.util.PsiModificationTracker.MODIFICATION_COUNT));
    }

    private static @NotNull List<PsiFile> computeReachableFiles(@NotNull PsiFile start) {
        Set<PsiFile> seen = new LinkedHashSet<>();
        Deque<PsiFile> queue = new ArrayDeque<>();
        queue.add(start);
        seen.add(start);
        while (!queue.isEmpty() && seen.size() < MAX_FILES) {
            PsiFile file = queue.removeFirst();
            for (PsiFile imported : directImports(file)) {
                if (seen.add(imported)) queue.addLast(imported);
            }
        }
        return new ArrayList<>(seen);
    }

    public static @NotNull List<PsiFile> directImports(@NotNull PsiFile file) {
        List<PsiFile> result = new ArrayList<>();
        for (PsiElement statement : MettlePsiUtil.childrenOfType(file, MettleTypes.IMPORT_DECL)) {
            String path = importPath(statement);
            if (path == null) continue;
            PsiFile target = MettleImportResolver.resolve(file, path);
            if (target instanceof MettleFile) result.add(target);
        }
        return result;
    }

    /** The quoted module path of an import statement, unquoted. */
    public static @Nullable String importPath(@Nullable PsiElement importDeclaration) {
        PsiElement literal = MettlePsiUtil.childOfType(importDeclaration, MettleTypes.STRING_LITERAL);
        if (literal == null) return null;
        return unquote(literal.getText());
    }

    public static @NotNull String unquote(@NotNull String text) {
        String result = text;
        if (result.startsWith("\"")) result = result.substring(1);
        if (result.endsWith("\"") && result.length() > 0) result = result.substring(0, result.length() - 1);
        return result;
    }
}
