param([Parameter(Mandatory=$true)][string]$Version)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$fixture = "$root/build/upgrade-test"
$target = "$fixture/installed"
$meta = "$fixture/packages/com.ryanchen01.ImageCXX/meta"
New-Item -ItemType Directory -Force $meta, "$fixture/packages/com.ryanchen01.ImageCXX/data/bin" | Out-Null
# Exercise the shipped bootstrap's embedded repository, not only IFW's maintenance API.
$freshTarget = "$fixture/fresh-installed"
& "$root/dist/ImageCXX-$Version-windows-x86_64.exe" --root $freshTarget --accept-licenses --default-answer --confirm-command install | Out-Host
[xml]$registration = Get-Content "$freshTarget/components.xml" -Raw
if (@($registration.Packages.Package.Name) -notcontains 'com.ryanchen01.ImageCXX') { throw 'The bootstrap did not register the expected component' }
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
& "$freshTarget/ImageCXXMaintenanceTool.exe" --accept-licenses --default-answer --confirm-command purge | Out-Host
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
Set-Content "$target/user-file.txt" 'Preserved user file'
New-ItemProperty HKCU:/Software/ImageCXX -Name UpgradeTest -Value 'preserved' -Force | Out-Null
try {
    $repository = ([Uri]"$root/build/installer/repository/").AbsoluteUri
    & $maintenance --set-temp-repository $repository --accept-licenses --default-answer --confirm-command update | Out-Host
    if (Test-Path "$target/obsolete.txt") { throw 'The upgrade left obsolete package files behind' }
    if ((Get-Content "$target/user-file.txt") -ne 'Preserved user file') { throw 'The upgrade removed user data' }
    if ((Get-ItemProperty HKCU:/Software/ImageCXX).UpgradeTest -ne 'preserved') { throw 'The upgrade removed settings' }
    if ((Get-ItemProperty HKCU:/Software/ImageCXX).InstalledVersion -ne $Version) { throw 'The upgrade did not register the new version' }
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
    Remove-ItemProperty HKCU:/Software/ImageCXX -Name UpgradeTest -ErrorAction SilentlyContinue
}
