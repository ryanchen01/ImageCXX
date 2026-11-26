var repository = "";
var repositoryUrl = "";
var existing = "";

function cleanup() {
    if (!repository) return;
    var temp = repository;
    repository = "";
    var files = JSON.parse(installer.readFile(":/imagecxx/repository-files.json", "UTF-8"));
    for (var i = 0; i < files.length; ++i)
        installer.performOperation("Delete", [temp + "/" + files[i]]);
    installer.performOperation("Delete", [temp + "/repository.7z"]);
    installer.performOperation("Rmdir", [temp + "/com.ryanchen01.ImageCXX"]);
    installer.performOperation("Rmdir", [temp]);
}

function Controller() {
    if (!installer.isInstaller()) return;
    var temporary = installer.environmentVariable("TEMP");
    var local = installer.environmentVariable("LOCALAPPDATA");
    if (!temporary || !local) throw new Error("Cannot determine the current user's application and temporary directories.");
    installer.setValue("TargetDir", local.replace(/\\/g, "/") + "/Programs/ImageCXX");
    repository = temporary.replace(/\\/g, "/") + "/ImageCXX-" + installer.value("ProductVersion") + "-" + Date.now();
    if (!installer.performOperation("Mkdir", [repository])
        || !installer.performOperation("Copy", [":/imagecxx/repository.7z", repository + "/repository.7z"])
        || !installer.performOperation("Extract", [repository + "/repository.7z", repository])) {
        throw new Error("Cannot extract the ImageCXX installation repository.");
    }
    repositoryUrl = encodeURI("file:///" + repository.replace(/\\/g, "/")).replace(/#/g, "%23").replace(/\?/g, "%3F").replace(/,/g, "%2C");
    installer.setTemporaryRepositories([repositoryUrl], true);
    existing = installer.value("HKEY_CURRENT_USER\\Software\\ImageCXX\\InstallDir", "").replace(/\\/g, "/").replace(/\/+$/, "");
    var supplied = installer.value("ImageCXXInstallDir", "");
    if (supplied) {
        supplied = supplied.replace(/\\/g, "/").replace(/\/+$/, "");
        if (existing && supplied.toLowerCase() !== existing.toLowerCase())
            throw new Error("The running app does not match the registered ImageCXX installation.");
        existing = supplied;
    }
    if (existing && (!installer.fileExists(existing + "/ImageCXXMaintenanceTool.exe")
        || !installer.fileExists(existing + "/components.xml")
        || installer.readFile(existing + "/components.xml", "UTF-8").indexOf('<Name>com.ryanchen01.ImageCXX</Name>') < 0)) {
        throw new Error("The existing ImageCXX installation cannot be updated. Repair it before continuing.");
    }
    installer.installationFinished.connect(cleanup);
}

Controller.prototype.IntroductionPageCallback = function() {
    if (!installer.isInstaller() || !existing) return;
    var target = existing;
    existing = "";
    // The installed maintenance tool owns the operation log and IFW's rollback logic.
    var result = installer.execute(target + "/ImageCXXMaintenanceTool.exe",
        ["--start-updater", "--set-temp-repository", repositoryUrl]);
    cleanup();
    if (!result || result[1] !== 0)
        QMessageBox.warning("ImageCXXUpdateFailed", "ImageCXX", "The update did not complete. Your existing installation has been retained.");
    installer.setCanceled();
    gui.clickButton(buttons.CancelButton, 0);
};
