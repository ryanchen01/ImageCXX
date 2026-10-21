function Controller() {
    if (!installer.isInstaller()) return;
    var temporary = installer.environmentVariable("TEMP");
    var local = installer.environmentVariable("LOCALAPPDATA");
    if (!temporary || !local) throw new Error("Cannot determine the current user's application and temporary directories.");
    installer.setValue("TargetDir", local.replace(/\\/g, "/") + "/Programs/ImageCXX");
    this.repository = temporary.replace(/\\/g, "/") + "/ImageCXX-" + installer.value("ProductVersion") + "-" + Date.now();
    if (!installer.performOperation("Mkdir", [this.repository])
        || !installer.performOperation("Copy", [":/imagecxx/repository.7z", this.repository + "/repository.7z"])
        || !installer.performOperation("Extract", [this.repository + "/repository.7z", this.repository])) {
        throw new Error("Cannot extract the ImageCXX installation repository.");
    }
    this.repositoryUrl = encodeURI("file:///" + this.repository.replace(/\\/g, "/")).replace(/#/g, "%23").replace(/\?/g, "%3F");
    installer.setTemporaryRepositories([this.repositoryUrl], true);
    var result = installer.execute(installer.environmentVariable("SystemRoot") + "/System32/reg.exe",
        ["query", "HKCU\\Software\\ImageCXX", "/v", "InstallDir"]);
    var match = result && result[1] === 0 && /^\s*InstallDir\s+REG_SZ\s+(.+?)\s*$/m.exec(result[0]);
    this.existing = match ? match[1].replace(/\\/g, "/") : "";
    var supplied = installer.value("ImageCXXInstallDir", "");
    if (supplied) {
        supplied = supplied.replace(/\\/g, "/");
        if (this.existing && supplied.toLowerCase() !== this.existing.toLowerCase())
            throw new Error("The running app does not match the registered ImageCXX installation.");
        this.existing = supplied;
    }
    if (this.existing && (!installer.fileExists(this.existing + "/ImageCXXMaintenanceTool.exe")
        || !installer.fileExists(this.existing + "/components.xml")
        || installer.readFile(this.existing + "/components.xml").indexOf('name="com.ryanchen01.ImageCXX"') < 0)) {
        throw new Error("The existing ImageCXX installation cannot be updated. Repair it before continuing.");
    }
    installer.installationFinished.connect(this, this.cleanup);
}

Controller.prototype.cleanup = function() {
    if (!this.repository) return;
    var files = JSON.parse(installer.readFile(":/imagecxx/repository-files.json"));
    for (var i = 0; i < files.length; ++i)
        installer.performOperation("Delete", [this.repository + "/" + files[i]]);
    installer.performOperation("Delete", [this.repository + "/repository.7z"]);
    installer.performOperation("Rmdir", [this.repository + "/com.ryanchen01.ImageCXX"]);
    installer.performOperation("Rmdir", [this.repository]);
};

Controller.prototype.IntroductionPageCallback = function() {
    if (!installer.isInstaller() || !this.existing) return;
    // The installed maintenance tool owns the operation log and IFW's rollback logic.
    var result = installer.execute(this.existing + "/ImageCXXMaintenanceTool.exe",
        ["--start-updater", "--set-temp-repository", this.repositoryUrl]);
    this.cleanup();
    if (!result || result[1] !== 0)
        QMessageBox.warning("ImageCXXUpdateFailed", "ImageCXX", "The update did not complete. Your existing installation has been retained.");
    installer.setCanceled();
    gui.reject();
};
