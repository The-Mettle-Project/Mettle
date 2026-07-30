package org.mettle.clion.settings;

import com.intellij.openapi.components.PersistentStateComponent;
import com.intellij.openapi.components.State;
import com.intellij.openapi.components.Storage;
import com.intellij.openapi.project.Project;
import com.intellij.util.xmlb.XmlSerializerUtil;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.util.ArrayList;
import java.util.List;

/** Per-project Mettle toolchain configuration. */
@State(name = "MettleSettings", storages = @Storage("mettle.xml"))
public class MettleSettings implements PersistentStateComponent<MettleSettings> {

    /** Path to the mettle executable. Empty means "find it automatically". */
    public String compilerPath = "";

    /** Passed as {@code --stdlib} when set. */
    public String stdlibPath = "";

    /** Extra {@code -I} directories. */
    public List<String> includePaths = new ArrayList<>();

    /** Run the compiler for diagnostics as files are edited. */
    public boolean diagnosticsEnabled = true;

    /** Give up on one diagnostics run after this long. */
    public int diagnosticsTimeoutMs = 10000;

    /** Extra arguments appended to every diagnostics run. */
    public String extraDiagnosticArgs = "";

    public static MettleSettings getInstance(@NotNull Project project) {
        return project.getService(MettleSettings.class);
    }

    @Override
    public @Nullable MettleSettings getState() {
        return this;
    }

    @Override
    public void loadState(@NotNull MettleSettings state) {
        XmlSerializerUtil.copyBean(state, this);
    }
}
