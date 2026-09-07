// SPDX-License-Identifier: MPL-2.0
//
// PlotJuggler 4 IFW install script.
//
// Behavior:
// - Per-user install by default (no gainAdminRights). Runs from
//   %LOCALAPPDATA%\PlotJuggler4 unless the user picks another dir. Corporate
//   locked-down machines that block UAC prompts still install cleanly.
// - Creates Start Menu + Desktop shortcuts on Windows.
// - Prompts before purging a previous installation in the same directory
//   (the earlier PJ3-inherited flow ran maintenancetool purge silently).

var targetDirectoryPage = null;

function Component()
{
    component.loaded.connect(this, this.installerLoaded);
}

Component.prototype.isDefault = function()
{
    return true;
}

Component.prototype.createOperations = function()
{
    try {
        component.createOperations();
        if (systemInfo.productType === "windows") {
            component.addOperation("CreateShortcut",
                "@TargetDir@/bin/PlotJuggler4.exe",
                "@StartMenuDir@/PlotJuggler 4.lnk",
                "workingDirectory=@TargetDir@/bin", "iconId=0",
                "description=Launch PlotJuggler 4");
            component.addOperation("CreateShortcut",
                "@TargetDir@/bin/PlotJuggler4.exe",
                "@DesktopDir@/PlotJuggler 4.lnk",
                "workingDirectory=@TargetDir@/bin", "iconId=0",
                "description=Launch PlotJuggler 4");
        }
    } catch (e) {
        console.log(e);
    }
}

// Custom target-directory page — https://stackoverflow.com/a/46614107
Component.prototype.installerLoaded = function()
{
    installer.setDefaultPageVisible(QInstaller.TargetDirectory, false);
    installer.addWizardPage(component, "TargetWidget", QInstaller.TargetDirectory);

    targetDirectoryPage = gui.pageWidgetByObjectName("DynamicTargetWidget");
    targetDirectoryPage.windowTitle = "Choose Installation Directory";
    targetDirectoryPage.description.setText("Please select where PlotJuggler 4 will be installed:");
    targetDirectoryPage.targetDirectory.textChanged.connect(this, this.targetDirectoryChanged);
    targetDirectoryPage.targetDirectory.setText(installer.value("TargetDir"));
    targetDirectoryPage.targetChooser.released.connect(this, this.targetChooserClicked);

    gui.pageById(QInstaller.ComponentSelection).entered.connect(this, this.componentSelectionPageEntered);
}

Component.prototype.targetChooserClicked = function()
{
    var dir = QFileDialog.getExistingDirectory("", targetDirectoryPage.targetDirectory.text);
    targetDirectoryPage.targetDirectory.setText(dir);
}

Component.prototype.targetDirectoryChanged = function()
{
    var dir = targetDirectoryPage.targetDirectory.text;
    if (installer.fileExists(dir) && installer.fileExists(dir + "/maintenancetool.exe")) {
        targetDirectoryPage.warning.setText("<p style=\"color: red\">Existing installation detected — it will be replaced on the next page.</p>");
    }
    else if (installer.fileExists(dir)) {
        targetDirectoryPage.warning.setText("<p style=\"color: red\">Installing into an existing directory. Its contents will be wiped on uninstall.</p>");
    }
    else {
        targetDirectoryPage.warning.setText("");
    }
    installer.setValue("TargetDir", dir);
}

Component.prototype.componentSelectionPageEntered = function()
{
    var dir = installer.value("TargetDir");
    if (installer.fileExists(dir) && installer.fileExists(dir + "/maintenancetool.exe")) {
        var answer = QMessageBox["question"]("existing.install",
            "Replace existing installation?",
            "PlotJuggler 4 is already installed at:\n\n" + dir + "\n\n" +
            "Continuing will uninstall it before the new version is installed. Continue?",
            QMessageBox.Yes | QMessageBox.No);
        if (answer !== QMessageBox.Yes) {
            gui.rejectPage(QInstaller.ComponentSelection);
            return;
        }
        installer.execute(dir + "/maintenancetool.exe", ["purge", "-c"]);
    }
}
