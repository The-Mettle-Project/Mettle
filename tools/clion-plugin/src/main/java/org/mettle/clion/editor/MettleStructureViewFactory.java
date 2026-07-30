package org.mettle.clion.editor;

import com.intellij.ide.structureView.StructureViewBuilder;
import com.intellij.ide.structureView.StructureViewModel;
import com.intellij.ide.structureView.StructureViewModelBase;
import com.intellij.ide.structureView.StructureViewTreeElement;
import com.intellij.ide.structureView.TreeBasedStructureViewBuilder;
import com.intellij.ide.util.treeView.smartTree.Sorter;
import com.intellij.lang.PsiStructureViewFactory;
import com.intellij.openapi.editor.Editor;
import com.intellij.psi.PsiFile;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;
import org.mettle.clion.psi.MettleDeclaration;
import org.mettle.clion.psi.MettleFile;

public class MettleStructureViewFactory implements PsiStructureViewFactory {

    @Override
    public @Nullable StructureViewBuilder getStructureViewBuilder(@NotNull PsiFile psiFile) {
        if (!(psiFile instanceof MettleFile)) return null;
        return new TreeBasedStructureViewBuilder() {
            @Override
            public @NotNull StructureViewModel createStructureViewModel(@Nullable Editor editor) {
                return new Model(psiFile);
            }
        };
    }

    private static class Model extends StructureViewModelBase implements StructureViewModel.ElementInfoProvider {
        Model(@NotNull PsiFile file) {
            super(file, new MettleStructureViewElement(file));
            withSorters(Sorter.ALPHA_SORTER);
        }

        @Override
        public boolean isAlwaysShowsPlus(StructureViewTreeElement element) {
            return false;
        }

        @Override
        public boolean isAlwaysLeaf(StructureViewTreeElement element) {
            Object value = element.getValue();
            if (!(value instanceof MettleDeclaration)) return false;
            return MettleStructureViewElement.isLeafKind((MettleDeclaration) value);
        }
    }
}
