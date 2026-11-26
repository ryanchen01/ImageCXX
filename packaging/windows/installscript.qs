function psString(value) {
    return "'" + value.replace(/'/g, "''") + "'";
}

function Component() {
    installer.installationFinished.connect(function() {
        if (installer.status !== QInstaller.Success) return;
        var command = "Add-Type -Namespace ImageCXX -Name Shell -MemberDefinition '[System.Runtime.InteropServices.DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int eventId, uint flags, System.IntPtr item1, System.IntPtr item2);'; [ImageCXX.Shell]::SHChangeNotify(0x08000000, 0, [System.IntPtr]::Zero, [System.IntPtr]::Zero)";
        var active = installer.value("ProductUUID", "");
        var targetDir = installer.value("TargetDir", "");
        if (active && targetDir) {
            var root = (installer.value("AllUsers", "") === "true" ? "HKLM:" : "HKCU:") + "\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
            var nativeTarget = installer.toNativeSeparators(targetDir).replace(/\\+$/, "");
            // QtIFW creates a new Uninstall key if an older installer ran standalone; prune duplicates for this directory.
            command += "; $root = " + psString(root)
                + "; $active = " + psString(active)
                + "; $target = " + psString(nativeTarget)
                + "; $tool = $target + '\\ImageCXXMaintenanceTool.exe'"
                + "; Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue | ForEach-Object {"
                + " if ($_.PSChildName -ieq $active) { return }"
                + " $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue;"
                + " if (-not $p -or $p.DisplayName -ne 'ImageCXX' -or $p.Publisher -ne 'ImageCXX') { return }"
                + " $loc = if ($p.InstallLocation) { ([string]$p.InstallLocation).Trim().Trim('\"').Replace('/', '\\').TrimEnd('\\') } else { '' };"
                + " $cmd = if ($p.UninstallString) { ([string]$p.UninstallString).Trim().Trim('\"').Replace('/', '\\') } else { '' };"
                + " if ($loc -ieq $target -or $cmd -ieq $tool) { Remove-Item -LiteralPath $_.PSPath -Recurse -Force -ErrorAction SilentlyContinue }"
                + " }";
        }
        // Registry writes alone do not invalidate the Shell's association cache.
        installer.execute(installer.environmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe", [
            "-NoProfile", "-NonInteractive", "-Command", command
        ]);
    });
}

Component.prototype.createOperations = function() {
    var version = component.value("Version");
    var productUuid = installer.value("ProductUUID", "");
    installer.setValue("ProductVersion", version);
    component.createOperations();
    component.addOperation("FakeStopProcessForUpdate", "ImageCXX.exe");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstallDir", "@TargetDir@");
    component.addOperation("GlobalConfig", "HKEY_CURRENT_USER\\Software\\ImageCXX", "InstalledVersion", version);
    if (!installer.isInstaller() && productUuid) {
        // QtIFW only calls registerMaintenanceTool() during fresh installs, so refresh Uninstall metadata on updates.
        var uninstallRoot = installer.value("AllUsers", "") === "true"
            ? "HKEY_LOCAL_MACHINE" : "HKEY_CURRENT_USER";
        var uninstallKey = uninstallRoot + "\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + productUuid;
        var now = new Date();
        var month = now.getMonth() + 1;
        var day = now.getDate();
        var installDate = String(now.getFullYear())
            + (month < 10 ? "0" + month : String(month))
            + (day < 10 ? "0" + day : String(day));
        component.addOperation("GlobalConfig", uninstallKey, "DisplayVersion", version);
        component.addOperation("GlobalConfig", uninstallKey, "InstallDate", installDate);
    }
    // Offer ImageCXX in Open with and Default apps without replacing the user's choice.
    var classes = "HKEY_CURRENT_USER\\Software\\Classes\\";
    var progId = "ImageCXX.Image";
    // Explorer consumes these paths directly; resolve IFW variables before conversion.
    var executable = installer.toNativeSeparators(installer.value("TargetDir") + "/bin/ImageCXX.exe");
    var openCommand = '"' + executable + '" -- "%1"';
    component.addOperation("GlobalConfig", classes + progId, "Default", "ImageCXX image");
    component.addOperation("GlobalConfig", classes + progId + "\\DefaultIcon", "Default", '"' + executable + '",0');
    component.addOperation("GlobalConfig", classes + progId + "\\shell\\open\\command", "Default", openCommand);
    var application = classes + "Applications\\ImageCXX.exe";
    component.addOperation("GlobalConfig", application, "FriendlyAppName", "ImageCXX");
    component.addOperation("GlobalConfig", application + "\\shell\\open\\command", "Default", openCommand);
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
