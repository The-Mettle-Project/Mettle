package org.mettle.clion.psi;

import com.intellij.navigation.NavigationItem;
import com.intellij.psi.PsiNameIdentifierOwner;

/** Anything in Mettle that introduces a name: a function, type, field, parameter or binding. */
public interface MettleNamedElement extends PsiNameIdentifierOwner, NavigationItem {
}
