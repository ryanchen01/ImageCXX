function Component() {}

Component.prototype.createOperations = function() {
    installer.setValue("ProductVersion", component.value("Version"));
    component.createOperations();
    component.addOperation("FakeStopProcessForUpdate", "ImageCXX.exe");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstallDir", "@TargetDir@");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstalledVersion", component.value("Version"));
    // Offer ImageCXX in Open with and Default apps without replacing the user's choice.
    var classes = "HKEY_CURRENT_USER\\Software\\Classes\\";
    var progId = "ImageCXX.Image";
    component.addOperation("GlobalConfig", classes + progId, "Default", "ImageCXX image");
    component.addOperation("GlobalConfig", classes + progId + "\\DefaultIcon", "Default", '"@TargetDir@/bin/ImageCXX.exe",0');
    component.addOperation("GlobalConfig", classes + progId + "\\shell\\open\\command", "Default", '"@TargetDir@/bin/ImageCXX.exe" -- "%1"');
    var capabilities = "HKEY_CURRENT_USER\\Software\\ImageCXX\\Capabilities";
    component.addOperation("GlobalConfig", capabilities, "ApplicationName", "ImageCXX");
    component.addOperation("GlobalConfig", capabilities, "ApplicationDescription", "Image viewer and analysis");
    // Windows matches the final suffix: compressed NIfTI needs .gz, not .nii.gz.
    var extensions = ["png", "jpg", "jpeg", "tif", "tiff", "mha", "mhd", "nii", "gz", "nrrd", "nhdr", "dcm", "dicom", "raw", "bin"];
    for (var i = 0; i < extensions.length; ++i) {
        component.addOperation("GlobalConfig", classes + "." + extensions[i] + "\\OpenWithProgids", progId, "");
        component.addOperation("GlobalConfig", capabilities + "\\FileAssociations", "." + extensions[i], progId);
    }
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\RegisteredApplications", "ImageCXX", "Software\\ImageCXX\\Capabilities");
    component.addOperation("CreateShortcut", "@TargetDir@/bin/ImageCXX.exe", "@StartMenuDir@/ImageCXX.lnk");
};
