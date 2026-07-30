package org.mettle.clion.editor;

import com.intellij.navigation.ChooseByNameContributor;
import com.intellij.navigation.NavigationItem;
import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.PsiManager;
import com.intellij.psi.search.FileTypeIndex;
import com.intellij.psi.search.GlobalSearchScope;
import com.intellij.openapi.vfs.VirtualFile;
import org.jetbrains.annotations.NotNull;
import org.mettle.clion.lang.MettleFileType;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettleFile;

import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/** Feeds Navigate | Symbol with every declaration in the project's Mettle files. */
public class MettleGotoSymbolContributor implements ChooseByNameContributor {

    @Override
    public String @NotNull [] getNames(Project project, boolean includeNonProjectItems) {
        Set<String> names = new LinkedHashSet<>();
        for (MettleDeclaration declaration : allDeclarations(project, includeNonProjectItems)) {
            String name = declaration.getName();
            if (name != null) names.add(name);
        }
        return names.toArray(new String[0]);
    }

    @Override
    public NavigationItem @NotNull [] getItemsByName(String name, String pattern, Project project,
                                                     boolean includeNonProjectItems) {
        List<NavigationItem> items = new ArrayList<>();
        for (MettleDeclaration declaration : allDeclarations(project, includeNonProjectItems)) {
            if (name.equals(declaration.getName())) items.add(declaration);
        }
        return items.toArray(new NavigationItem[0]);
    }

    private static List<MettleDeclaration> allDeclarations(Project project, boolean includeNonProjectItems) {
        List<MettleDeclaration> declarations = new ArrayList<>();
        GlobalSearchScope scope = includeNonProjectItems
                ? GlobalSearchScope.allScope(project)
                : GlobalSearchScope.projectScope(project);
        Collection<VirtualFile> files = FileTypeIndex.getFiles(MettleFileType.INSTANCE, scope);
        PsiManager manager = PsiManager.getInstance(project);
        for (VirtualFile file : files) {
            PsiFile psiFile = manager.findFile(file);
            if (!(psiFile instanceof MettleFile)) continue;
            collect(psiFile, declarations, 0);
        }
        return declarations;
    }

    private static void collect(PsiElement element, List<MettleDeclaration> sink, int depth) {
        if (depth > 3) return;
        for (PsiElement child = element.getFirstChild(); child != null; child = child.getNextSibling()) {
            if (child instanceof MettleDeclaration) {
                MettleDeclaration declaration = (MettleDeclaration) child;
                if (declaration.getName() != null) sink.add(declaration);
                collect(child, sink, depth + 1);
            }
        }
    }
}
