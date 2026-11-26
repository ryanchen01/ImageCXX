param([Parameter(Mandatory=$true)][string]$Version)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$fixture = "$root/build/upgrade-test"
$target = "$fixture/installed"
$meta = "$fixture/packages/com.ryanchen01.ImageCXX/meta"
New-Item -ItemType Directory -Force $meta, "$fixture/packages/com.ryanchen01.ImageCXX/data/bin" | Out-Null

function Get-ImageCXXUninstallEntries {
    param([Parameter(Mandatory=$true)][string]$InstallDir)
    $normalized = [IO.Path]::GetFullPath($InstallDir).Replace('/', '\').TrimEnd('\')
    $tool = "$normalized\ImageCXXMaintenanceTool.exe"
    $entries = @()
    foreach ($item in Get-ChildItem -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue) {
        $props = Get-ItemProperty -LiteralPath $item.PSPath -ErrorAction SilentlyContinue
        if (-not $props -or $props.DisplayName -ne 'ImageCXX' -or $props.Publisher -ne 'ImageCXX') { continue }
        $loc = if ($props.InstallLocation) { ([string]$props.InstallLocation).Trim().Trim('"').Replace('/', '\').TrimEnd('\') } else { '' }
        $cmd = if ($props.UninstallString) { ([string]$props.UninstallString).Trim().Trim('"').Replace('/', '\') } else { '' }
        if ($loc -ieq $normalized -or $cmd -ieq $tool) {
            $entries += $props
        }
    }
    return $entries
}

# Exercise the shipped bootstrap's embedded repository, not only IFW's maintenance API.
$freshTarget = "$fixture/fresh-installed"
& "$root/dist/ImageCXX-$Version-windows-x86_64.exe" --root $freshTarget --accept-licenses --default-answer --confirm-command install | Out-Host
try {
    [xml]$registration = Get-Content "$freshTarget/components.xml" -Raw
    if (@($registration.Packages.Package.Name) -notcontains 'com.ryanchen01.ImageCXX') { throw 'The bootstrap did not register the expected component' }
    $freshEntries = @(Get-ImageCXXUninstallEntries $freshTarget)
    if ($freshEntries.Count -ne 1) { throw "Expected 1 Uninstall entry after fresh install, found $($freshEntries.Count)" }
    if ($freshEntries[0].DisplayVersion -ne $Version) { throw "Expected DisplayVersion '$Version' after fresh install, found '$($freshEntries[0].DisplayVersion)'" }
    $expectedExecutable = [IO.Path]::GetFullPath("$freshTarget/bin/ImageCXX.exe").Replace('/', '\')
    foreach ($handler in @('ImageCXX.Image', 'Applications/ImageCXX.exe')) {
        $openCommand = (Get-Item "HKCU:/Software/Classes/$handler/shell/open/command").GetValue('')
        if ($openCommand -ne ('"' + $expectedExecutable + '" -- "%1"')) { throw "The installer did not register a quoted native file-open command for $handler" }
    }
    if ((Get-Item 'HKCU:/Software/Classes/ImageCXX.Image/DefaultIcon').GetValue('') -ne ('"' + $expectedExecutable + '",0')) { throw 'The installer did not register a native icon path' }
    foreach ($extension in @('png', 'jpg', 'jpeg', 'tif', 'tiff', 'mha', 'mhd', 'nii', 'gz', 'nrrd', 'nhdr', 'dcm', 'dicom', 'raw', 'bin')) {
        if ((Get-Item 'HKCU:/Software/ImageCXX/Capabilities/FileAssociations').GetValue(".$extension") -ne 'ImageCXX.Image') { throw "Missing default-app association for .$extension" }
        if ((Get-Item "HKCU:/Software/Classes/.$extension/OpenWithProgids").GetValueNames() -notcontains 'ImageCXX.Image') { throw "Missing Open with registration for .$extension" }
    }
    $capabilitiesPath = (Get-Item 'HKCU:/Software/RegisteredApplications').GetValue('ImageCXX')
    if ($capabilitiesPath -ne 'Software\ImageCXX\Capabilities') { throw 'The installer did not register its application capabilities' }
    # Query the same Shell handlers used by Open with, using actual filenames.
    Add-Type -Path "$root/tests/windows_shell_handlers.cs"
    foreach ($filename in @('scan.nii.gz', 'scan.nii', 'image.png')) {
        $handlers = @([ImageCXXShellHandlers]::GetNames($filename) | ForEach-Object { $_.Replace('\', '/') })
        # The Shell can identify a Capabilities handler by its RegisteredApplications name.
        # The executable command and each extension's ProgID are checked above.
        if ($handlers -notcontains 'ImageCXX' -and $handlers -notcontains ($freshTarget.Replace('\', '/') + '/bin/ImageCXX.exe')) { throw "ImageCXX is missing from Shell handlers for ${filename}. Found: $($handlers -join ', ')" }
    }
} finally {
    & "$freshTarget/ImageCXXMaintenanceTool.exe" --accept-licenses --default-answer --confirm-command purge | Out-Host
}
if (@(Get-ImageCXXUninstallEntries $freshTarget).Count -ne 0) { throw 'Fresh install purge left Uninstall entries behind' }
$config = (Get-Content packaging/windows/config.xml.in -Raw).Replace('@VERSION@', '0.0.0').Replace('<ControlScript>controller.qs</ControlScript>', '')
$config | Set-Content "$fixture/config.xml" -Encoding utf8
Copy-Item resources/icons/imagecxx.ico "$fixture/imagecxx.ico"
(Get-Content packaging/windows/package.xml.in -Raw).Replace('@VERSION@', '0.0.0').Replace('@DATE@', '2020-01-01') | Set-Content "$meta/package.xml" -Encoding utf8
Copy-Item packaging/windows/installscript.qs "$meta/installscript.qs"
Copy-Item LICENSE "$meta/LICENSE.txt"
Set-Content "$fixture/packages/com.ryanchen01.ImageCXX/data/obsolete.txt" 'Removed by upgrade'
Set-Content "$fixture/packages/com.ryanchen01.ImageCXX/data/bin/ImageCXX.exe" 'Old tracked executable'
binarycreator --offline-only -c "$fixture/config.xml" -p "$fixture/packages" "$fixture/old-setup.exe"
& "$fixture/old-setup.exe" --root $target --accept-licenses --default-answer --confirm-command install | Out-Host
$maintenance = "$target/ImageCXXMaintenanceTool.exe"
if (!(Test-Path $maintenance)) { throw 'The old installer did not create a maintenance tool' }
$initialEntries = @(Get-ImageCXXUninstallEntries $target)
if ($initialEntries.Count -ne 1) { throw "Expected 1 Uninstall entry after old install, found $($initialEntries.Count)" }
if ($initialEntries[0].DisplayVersion -ne '0.0.0') { throw "Expected DisplayVersion '0.0.0' after old install, found '$($initialEntries[0].DisplayVersion)'" }
$staleName = '{00000000-0000-0000-0000-000000000001}'
$staleKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$staleName"
$normalizedTarget = [IO.Path]::GetFullPath($target).Replace('/', '\').TrimEnd('\')
New-Item -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' -Name $staleName -Force | Out-Null
New-ItemProperty -LiteralPath $staleKey -Name DisplayName -Value 'ImageCXX' -PropertyType String -Force | Out-Null
New-ItemProperty -LiteralPath $staleKey -Name Publisher -Value 'ImageCXX' -PropertyType String -Force | Out-Null
New-ItemProperty -LiteralPath $staleKey -Name DisplayVersion -Value '0.0.0-stale' -PropertyType String -Force | Out-Null
New-ItemProperty -LiteralPath $staleKey -Name InstallLocation -Value $normalizedTarget -PropertyType String -Force | Out-Null
New-ItemProperty -LiteralPath $staleKey -Name UninstallString -Value ('"' + $normalizedTarget + '\ImageCXXMaintenanceTool.exe"') -PropertyType String -Force | Out-Null
Set-Content "$target/user-file.txt" 'Preserved user file'
New-ItemProperty HKCU:/Software/ImageCXX -Name UpgradeTest -Value 'preserved' -Force | Out-Null
try {
    $repository = ([Uri]"$root/build/installer/repository/").AbsoluteUri
    & $maintenance --set-temp-repository $repository --accept-licenses --default-answer --confirm-command update | Out-Host
    if (Test-Path "$target/obsolete.txt") { throw 'The upgrade left obsolete package files behind' }
    if ((Get-Content "$target/user-file.txt") -ne 'Preserved user file') { throw 'The upgrade removed user data' }
    if ((Get-ItemProperty HKCU:/Software/ImageCXX).UpgradeTest -ne 'preserved') { throw 'The upgrade removed settings' }
    if ((Get-ItemProperty HKCU:/Software/ImageCXX).InstalledVersion -ne $Version) { throw 'The upgrade did not register the new version' }
    $updatedEntries = @(Get-ImageCXXUninstallEntries $target)
    if ($updatedEntries.Count -ne 1) { throw "Expected 1 Uninstall entry after update, found $($updatedEntries.Count)" }
    if ($updatedEntries[0].DisplayVersion -ne $Version) { throw "Expected DisplayVersion '$Version' after update, found '$($updatedEntries[0].DisplayVersion)'" }
    if ($updatedEntries[0].InstallDate -notmatch '^\d{8}$') { throw "Expected YYYYMMDD InstallDate after update, found '$($updatedEntries[0].InstallDate)'" }
    if (Test-Path -LiteralPath $staleKey) { throw 'The update did not remove stale duplicate Uninstall entries' }
    $previousPath = $env:PATH
    $previousPlugins = $env:QT_PLUGIN_PATH
    try {
        $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
        Remove-Item Env:QT_PLUGIN_PATH -ErrorAction SilentlyContinue
        $output = & "$target/bin/ImageCXX.exe" --batch --version | Out-String
        if ($output.Trim() -ne "ImageCXX $Version") { throw 'The installed app cannot run with only its bundled dependencies' }
    } finally {
        $env:PATH = $previousPath
        $env:QT_PLUGIN_PATH = $previousPlugins
    }
} finally {
    & $maintenance --accept-licenses --default-answer --confirm-command purge | Out-Host
    if (Test-Path -LiteralPath $staleKey) { Remove-Item -LiteralPath $staleKey -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-ItemProperty HKCU:/Software/ImageCXX -Name UpgradeTest -ErrorAction SilentlyContinue
}
if (@(Get-ImageCXXUninstallEntries $target).Count -ne 0) { throw 'Purging updated install left Uninstall entries behind' }

$handoffTarget = "$fixture/handoff-图像-installed"
$handoffTemp = [IO.Path]::GetFullPath("$fixture/handoff-temp")
$stubSource = [IO.Path]::GetFullPath("$fixture/handoff-stub.cs")
$stubExe = [IO.Path]::GetFullPath("$fixture/handoff-stub.exe")
New-Item -ItemType Directory -Force $handoffTarget, $handoffTemp | Out-Null
@'
<Packages><Package><Name>com.ryanchen01.ImageCXX</Name></Package></Packages>
'@ | Set-Content "$handoffTarget/components.xml" -Encoding utf8
@'
using System;
using System.IO;

class Program {
    static int Main(string[] args) {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        File.WriteAllLines(Path.Combine(dir, "handoff-args.txt"), args);
        string repo = null;
        for (int i = 0; i + 1 < args.Length; i++) {
            if (args[i] == "--set-temp-repository") repo = args[i + 1];
        }
        bool ok = false;
        if (!string.IsNullOrEmpty(repo)) {
            ok = File.Exists(Path.Combine(new Uri(repo).LocalPath, "Updates.xml"));
        }
        File.WriteAllText(Path.Combine(dir, "handoff-repo-ok.txt"), ok ? "ok" : "missing");
        return 0;
    }
}
'@ | Set-Content -LiteralPath $stubSource -Encoding utf8
$csc = "$env:SystemRoot\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
& $csc /nologo "/out:$stubExe" "$stubSource" | Out-Host
Copy-Item -LiteralPath $stubExe "$handoffTarget/ImageCXXMaintenanceTool.exe"

$normalizedHandoff = [IO.Path]::GetFullPath($handoffTarget).Replace('/', '\').TrimEnd('\')
New-Item -Path 'HKCU:/Software/ImageCXX' -Force | Out-Null
New-ItemProperty -LiteralPath 'HKCU:/Software/ImageCXX' -Name InstallDir -Value $normalizedHandoff -PropertyType String -Force | Out-Null
$previousQpa = $env:QT_QPA_PLATFORM
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    $env:TEMP = $handoffTemp
    $env:TMP = $handoffTemp
    $psi = [Diagnostics.ProcessStartInfo]::new([IO.Path]::GetFullPath("$root/dist/ImageCXX-$Version-windows-x86_64.exe"))
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $process = [Diagnostics.Process]::Start($psi)
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        throw 'The Windows installer did not exit after handing off to the maintenance tool'
    }
    if ($process.ExitCode -notin @(0, 3)) {
        throw "Unexpected bootstrapper exit code during handoff: $($process.ExitCode)"
    }
    if (!(Test-Path "$handoffTarget/handoff-args.txt")) {
        throw 'The bootstrapper did not invoke ImageCXXMaintenanceTool.exe for an existing installation'
    }
    $handoffArgs = @(Get-Content "$handoffTarget/handoff-args.txt")
    $repoIndex = [Array]::IndexOf($handoffArgs, '--set-temp-repository')
    if ($handoffArgs -notcontains '--start-updater' -or $repoIndex -lt 0 -or ($repoIndex + 1) -ge $handoffArgs.Count -or $handoffArgs[$repoIndex + 1] -notlike 'file:///*') {
        throw "Unexpected maintenance tool handoff arguments: $($handoffArgs -join ' ')"
    }
    if ((Get-Content "$handoffTarget/handoff-repo-ok.txt" -Raw).Trim() -ne 'ok') {
        throw 'The bootstrapper did not extract Updates.xml into the temporary repository before handoff'
    }
    $repoLocalPath = ([Uri]$handoffArgs[$repoIndex + 1]).LocalPath
    if (Test-Path -LiteralPath $repoLocalPath) {
        throw "The bootstrapper left temporary repository folder behind after handoff: $repoLocalPath"
    }
    if (@(Get-ImageCXXUninstallEntries $handoffTarget).Count -ne 0) {
        throw 'The bootstrapper created a duplicate Uninstall entry during handoff'
    }
} finally {
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
    if ($null -eq $previousQpa) {
        Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    } else {
        $env:QT_QPA_PLATFORM = $previousQpa
    }
    Remove-ItemProperty -LiteralPath 'HKCU:/Software/ImageCXX' -Name InstallDir -ErrorAction SilentlyContinue
}
