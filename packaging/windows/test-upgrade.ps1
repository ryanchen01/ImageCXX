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
$registration = Get-Content "$freshTarget/components.xml" -Raw
if (!$registration.Contains('name="com.ryanchen01.ImageCXX"')) { throw 'The bootstrap did not register the expected component' }
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
