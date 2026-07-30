package org.mettle.clion.editor;

import com.intellij.codeInsight.completion.CompletionContributor;
import com.intellij.codeInsight.completion.CompletionParameters;
import com.intellij.codeInsight.completion.CompletionProvider;
import com.intellij.codeInsight.completion.CompletionResultSet;
import com.intellij.codeInsight.completion.CompletionType;
import com.intellij.codeInsight.lookup.LookupElementBuilder;
import com.intellij.icons.AllIcons;
import com.intellij.patterns.PlatformPatterns;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
import com.intellij.util.ProcessingContext;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.lang.MettleBuiltins;
import org.mettle.clion.lang.MettleTypes;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettleFile;
import org.mettle.clion.psi.MettlePsiUtil;
import org.mettle.clion.psi.MettleResolver;
import org.mettle.clion.settings.MettleToolchain;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.Set;
import java.util.stream.Stream;

/** Completion for members, keywords, visible declarations, decorators and import paths. */
public class MettleCompletionContributor extends CompletionContributor {

    public MettleCompletionContributor() {
        extend(CompletionType.BASIC, PlatformPatterns.psiElement(), new Provider());
    }

    private static class Provider extends CompletionProvider<CompletionParameters> {
        @Override
        protected void addCompletions(@NotNull CompletionParameters parameters,
                                      @NotNull ProcessingContext context,
                                      @NotNull CompletionResultSet result) {
            PsiElement position = parameters.getPosition();
            if (!(position.getContainingFile() instanceof MettleFile)) return;

            IElementType positionType = MettlePsiUtil.elementType(position);
            if (positionType == MettleTypes.STRING_LITERAL) {
                addImportPaths(position, result);
                return;
            }
            if (MettleTypes.COMMENTS.contains(positionType)) return;

            if (afterAt(position)) {
                for (String decorator : MettleBuiltins.DECORATORS) {
                    result.addElement(LookupElementBuilder.create(decorator)
                            .withIcon(AllIcons.Nodes.Annotationtype)
                            .withTypeText("decorator"));
                }
                return;
            }

            PsiElement parent = position.getParent();
            if (MettlePsiUtil.elementType(parent) == MettleTypes.FIELD_EXPR
                    && parent.getLastChild() == position) {
                addMembers(parent, result);
                return;
            }

            addVisibleSymbols(position, result);
            addKeywordsAndBuiltins(result);
        }
    }

    private static boolean afterAt(@NotNull PsiElement position) {
        PsiElement previous = position.getPrevSibling();
        if (previous != null && MettlePsiUtil.elementType(previous) == MettleTypes.AT) return true;
        PsiElement parent = position.getParent();
        return parent != null && MettlePsiUtil.elementType(parent) == MettleTypes.DECORATOR;
    }

    private static void addMembers(@NotNull PsiElement fieldExpression, @NotNull CompletionResultSet result) {
        PsiElement receiver = MettleResolver.firstExpression(fieldExpression);
        String typeName = MettleResolver.inferBaseTypeName(receiver);
        if (typeName == null) return;
        PsiElement type = MettleResolver.resolveTypeName(fieldExpression, typeName);
        for (PsiElement member : MettleResolver.membersOf(type)) {
            if (!(member instanceof MettleDeclaration)) continue;
            MettleDeclaration declaration = (MettleDeclaration) member;
            String name = declaration.getName();
            if (name == null) continue;
            result.addElement(LookupElementBuilder.create(name)
                    .withIcon(declaration.getIcon(0))
                    .withTypeText(typeText(declaration))
                    .withTailText(tailText(declaration), true));
        }
    }

    private static void addVisibleSymbols(@NotNull PsiElement position, @NotNull CompletionResultSet result) {
        Set<String> seen = new HashSet<>();
        MettleResolver.forEachVisible(position, declaration -> {
            String name = declaration.getName();
            if (name == null || !seen.add(name + "/" + declaration.getKind())) return;
            result.addElement(LookupElementBuilder.create(name)
                    .withIcon(declaration.getIcon(0))
                    .withTypeText(typeText(declaration))
                    .withTailText(tailText(declaration), true));
        });
    }

    private static void addKeywordsAndBuiltins(@NotNull CompletionResultSet result) {
        for (String keyword : MettleTypes.KEYWORDS.keySet()) {
            result.addElement(LookupElementBuilder.create(keyword).bold().withTypeText("keyword"));
        }
        for (String name : MettleBuiltins.TYPES) {
            result.addElement(LookupElementBuilder.create(name).withTypeText("built-in type"));
        }
        for (String name : MettleBuiltins.VALUES) {
            result.addElement(LookupElementBuilder.create(name).withTypeText("built-in"));
        }
        for (String name : MettleBuiltins.GPU) {
            result.addElement(LookupElementBuilder.create(name)
                    .withIcon(AllIcons.Nodes.Static)
                    .withTypeText("GPU built-in"));
        }
    }

    /** Modules that {@code import "..."} could name: the stdlib and this file's neighbours. */
    private static void addImportPaths(@NotNull PsiElement position, @NotNull CompletionResultSet result) {
        PsiFile file = position.getContainingFile();
        if (file == null) return;
        if (MettlePsiUtil.elementType(position.getParent()) != MettleTypes.IMPORT_DECL) return;

        Path stdlib = MettleToolchain.stdlibRoot(file.getProject(),
                MettleToolchain.findCompiler(file.getProject(), file.getVirtualFile()));
        if (stdlib != null) {
            listModules(stdlib, "std/", result);
        }
        if (file.getVirtualFile() != null && file.getVirtualFile().getParent() != null) {
            listModules(Paths.get(file.getVirtualFile().getParent().getPath()), "", result);
        }
    }

    private static void listModules(@NotNull Path directory, @NotNull String prefix,
                                    @NotNull CompletionResultSet result) {
        try (Stream<Path> entries = Files.list(directory)) {
            entries.forEach(entry -> {
                String name = entry.getFileName().toString();
                if (!name.endsWith(".mettle")) return;
                String module = prefix + name.substring(0, name.length() - ".mettle".length());
                result.addElement(LookupElementBuilder.create(module)
                        .withIcon(AllIcons.FileTypes.Any_type)
                        .withTypeText("module"));
            });
        } catch (IOException | RuntimeException ignored) {
            // an unreadable directory contributes no suggestions
        }
    }

    private static @Nullable String typeText(@NotNull MettleDeclaration declaration) {
        IElementType kind = declaration.getKind();
        if (kind == MettleTypes.FUNCTION_DECL || kind == MettleTypes.METHOD_DECL) {
            PsiElement returnType = MettlePsiUtil.childOfType(declaration, MettleTypes.TYPE_REF);
            return returnType == null ? "void" : returnType.getText();
        }
        return declaration.getDeclaredTypeText();
    }

    private static @Nullable String tailText(@NotNull MettleDeclaration declaration) {
        PsiElement params = MettlePsiUtil.childOfType(declaration, MettleTypes.PARAM_LIST);
        return params == null ? null : params.getText().replaceAll("\\s+", " ");
    }
}
