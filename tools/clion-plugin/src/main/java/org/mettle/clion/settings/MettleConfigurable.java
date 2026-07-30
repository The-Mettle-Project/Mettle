package org.mettle.clion.settings;

import com.intellij.openapi.fileChooser.FileChooser;
import com.intellij.openapi.fileChooser.FileChooserDescriptor;
import com.intellij.openapi.options.Configurable;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.ui.TextFieldWithBrowseButton;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.ui.components.JBCheckBox;
import com.intellij.ui.components.JBLabel;
import com.intellij.ui.components.JBScrollPane;
import com.intellij.ui.components.JBTextArea;
import com.intellij.ui.components.JBTextField;
import com.intellij.util.ui.FormBuilder;
import org.jetbrains.annotations.Nls;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import javax.swing.JComponent;
import javax.swing.JPanel;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Settings | Languages &amp; Frameworks | Mettle. */
public class MettleConfigurable implements Configurable {

    private final Project project;

    private final TextFieldWithBrowseButton compilerPath = new TextFieldWithBrowseButton();
    private final TextFieldWithBrowseButton stdlibPath = new TextFieldWithBrowseButton();
    private final JBTextArea includePaths = new JBTextArea(4, 40);
    private final JBCheckBox diagnosticsEnabled =
            new JBCheckBox("Run the compiler for diagnostics (on open and save)");
    private final JBTextField diagnosticsTimeout = new JBTextField();
    private final JBTextField extraDiagnosticArgs = new JBTextField();
    private final JBLabel detected = new JBLabel();

    public MettleConfigurable(@NotNull Project project) {
        this.project = project;
        browse(compilerPath, new FileChooserDescriptor(true, false, false, false, false, false),
                "Select the mettle executable");
        browse(stdlibPath, new FileChooserDescriptor(false, true, false, false, false, false),
                "Select the stdlib root");
    }

    private void browse(@NotNull TextFieldWithBrowseButton field, @NotNull FileChooserDescriptor descriptor,
                        @NotNull String title) {
        descriptor.setTitle(title);
        field.addActionListener(event -> {
            VirtualFile chosen = FileChooser.chooseFile(descriptor, project, null);
            if (chosen != null) field.setText(chosen.getPath());
        });
    }

    @Override
    public @Nls(capitalization = Nls.Capitalization.Title) String getDisplayName() {
        return "Mettle";
    }

    @Override
    public @Nullable JComponent createComponent() {
        includePaths.setLineWrap(false);
        JPanel panel = FormBuilder.createFormBuilder()
                .addLabeledComponent("Compiler:", compilerPath)
                .addComponentToRightColumn(detected)
                .addLabeledComponent("Stdlib root:", stdlibPath)
                .addLabeledComponent("Include directories (-I), one per line:",
                        new JBScrollPane(includePaths), true)
                .addComponent(diagnosticsEnabled)
                .addLabeledComponent("Diagnostics timeout (ms):", diagnosticsTimeout)
                .addLabeledComponent("Extra diagnostics arguments:", extraDiagnosticArgs)
                .addComponentFillVertically(new JPanel(), 0)
                .getPanel();
        updateDetectedLabel();
        return panel;
    }

    private void updateDetectedLabel() {
        Path found = MettleToolchain.findCompiler(project, null);
        detected.setText(found == null
                ? "Leave empty to search bin/ above the source, then PATH. Nothing found right now."
                : "Leave empty to search bin/ above the source, then PATH. Currently: " + found);
    }

    @Override
    public boolean isModified() {
        MettleSettings settings = MettleSettings.getInstance(project);
        return !compilerPath.getText().trim().equals(settings.compilerPath)
                || !stdlibPath.getText().trim().equals(settings.stdlibPath)
                || !readIncludePaths().equals(settings.includePaths)
                || diagnosticsEnabled.isSelected() != settings.diagnosticsEnabled
                || parseTimeout() != settings.diagnosticsTimeoutMs
                || !extraDiagnosticArgs.getText().trim().equals(settings.extraDiagnosticArgs);
    }

    @Override
    public void apply() {
        MettleSettings settings = MettleSettings.getInstance(project);
        settings.compilerPath = compilerPath.getText().trim();
        settings.stdlibPath = stdlibPath.getText().trim();
        settings.includePaths = readIncludePaths();
        settings.diagnosticsEnabled = diagnosticsEnabled.isSelected();
        settings.diagnosticsTimeoutMs = parseTimeout();
        settings.extraDiagnosticArgs = extraDiagnosticArgs.getText().trim();
        updateDetectedLabel();
    }

    @Override
    public void reset() {
        MettleSettings settings = MettleSettings.getInstance(project);
        compilerPath.setText(settings.compilerPath);
        stdlibPath.setText(settings.stdlibPath);
        includePaths.setText(String.join("\n", settings.includePaths));
        diagnosticsEnabled.setSelected(settings.diagnosticsEnabled);
        diagnosticsTimeout.setText(Integer.toString(settings.diagnosticsTimeoutMs));
        extraDiagnosticArgs.setText(settings.extraDiagnosticArgs);
        updateDetectedLabel();
    }

    private List<String> readIncludePaths() {
        List<String> result = new ArrayList<>();
        for (String line : Arrays.asList(includePaths.getText().split("\\R"))) {
            String trimmed = line.trim();
            if (!trimmed.isEmpty()) result.add(trimmed);
        }
        return result;
    }

    private int parseTimeout() {
        try {
            return Math.max(1000, Integer.parseInt(diagnosticsTimeout.getText().trim()));
        } catch (NumberFormatException ignored) {
            return 10000;
        }
    }
}
