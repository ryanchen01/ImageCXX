function Component() {
    installer.installationFinished.connect(function() {
        // Registry writes alone do not invalidate the Shell's association cache.
        installer.execute(installer.environmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe", [
            "-NoProfile", "-NonInteractive", "-Command",
            "Add-Type -Namespace ImageCXX -Name Shell -MemberDefinition '[System.Runtime.InteropServices.DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);'; [ImageCXX.Shell]::SHChangeNotify(0x08000000, 0, [System.IntPtr]::Zero, [System.IntPtr]::Zero)"
        ]);
    });
}

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
    var application = classes + "Applications\\ImageCXX.exe";
    component.addOperation("GlobalConfig", application, "FriendlyAppName", "ImageCXX");
    component.addOperation("GlobalConfig", application + "\\shell\\open\\command", "Default", '"@TargetDir@/bin/ImageCXX.exe" -- "%1"');
    var capabilities = "HKEY_CURRENT_USER\\Software\\ImageCXX\\Capabilities";
    component.addOperation("GlobalConfig", capabilities, "ApplicationName", "ImageCXX");
    component.addOperation("GlobalConfig", capabilities, "ApplicationDescription", "Image viewer and analysis");
    // Windows matches the final suffix: compressed NIfTI needs .gz, not .nii.gz.
    var extensions = ["png", "jpg", "jpeg", "tif", "tiff", "mha", "mhd", "nii", "gz", "nrrd", "nhdr", "dcm", "dicom", "raw", "bin"];
    for (var i = 0; i < extensions.length; ++i) {
        component.addOperation("GlobalConfig", classes + "." + extensions[i] + "\\OpenWithProgids", progId, "");
        component.addOperation("GlobalConfig", application + "\\SupportedTypes", "." + extensions[i], "");
        component.addOperation("GlobalConfig", capabilities + "\\FileAssociations", "." + extensions[i], progId);
    }
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\RegisteredApplications", "ImageCXX", "Software\\ImageCXX\\Capabilities");
    component.addOperation("CreateShortcut", "@TargetDir@/bin/ImageCXX.exe", "@StartMenuDir@/ImageCXX.lnk");
};
