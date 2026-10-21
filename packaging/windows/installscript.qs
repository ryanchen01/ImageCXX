function Component() {}

Component.prototype.createOperations = function() {
    installer.setValue("ProductVersion", component.value("Version"));
    component.createOperations();
    component.addOperation("FakeStopProcessForUpdate", "ImageCXX.exe");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstallDir", "@TargetDir@");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstalledVersion", component.value("Version"));
    component.addOperation("CreateShortcut", "@TargetDir@/bin/ImageCXX.exe", "@StartMenuDir@/ImageCXX.lnk");
};
