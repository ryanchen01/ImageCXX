param([Parameter(Mandatory=$true)][string]$Version)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Get-Location).Path
$stage = "$root/build/installer"
$meta = "$stage/packages/com.ryanchen01.ImageCXX/meta"
$data = "$stage/packages/com.ryanchen01.ImageCXX/data"
New-Item -ItemType Directory -Force $meta, $data, "$root/dist" | Out-Null
cmake --install build/release-vcpkg --prefix $data
# vcpkg's app-local step supplies scientific DLLs; windeployqt handles Qt and MSVC.
Get-ChildItem build/release-vcpkg -Filter *.dll | Copy-Item -Destination "$data/bin/"
& "$env:QT_ROOT_DIR/bin/windeployqt.exe" --release --qmldir "$root/plots" "$data/bin/ImageCXX.exe"
(Get-Content packaging/windows/config.xml.in -Raw).Replace('@VERSION@', $Version) | Set-Content "$stage/config.xml" -Encoding utf8
(Get-Content packaging/windows/package.xml.in -Raw).Replace('@VERSION@', $Version).Replace('@DATE@', (Get-Date -Format yyyy-MM-dd)) | Set-Content "$meta/package.xml" -Encoding utf8
Copy-Item packaging/windows/controller.qs "$stage/controller.qs"
Copy-Item resources/icons/imagecxx.ico "$stage/imagecxx.ico"
Copy-Item packaging/windows/installscript.qs "$meta/installscript.qs"
Copy-Item LICENSE "$meta/LICENSE.txt"
repogen --unite-metadata -p "$stage/packages" "$stage/repository"
$files = @(Get-ChildItem "$stage/repository" -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath("$stage/repository", $_.FullName).Replace('\', '/') })
ConvertTo-Json -InputObject $files | Set-Content "$stage/repository-files.json" -Encoding utf8
Push-Location "$stage/repository"
try { archivegen "$stage/repository.7z" @(Get-ChildItem -Name) } finally { Pop-Location }
@'
<RCC><qresource prefix="/imagecxx">
<file alias="repository.7z" compress="0">repository.7z</file>
<file alias="repository-files.json">repository-files.json</file>
</qresource></RCC>
'@ | Set-Content "$stage/repository.qrc" -Encoding utf8
# All payload is in the embedded repository, so the EXE works offline for installs and upgrades.
binarycreator --online-only -c "$stage/config.xml" -p "$stage/packages" -r "$stage/repository.qrc" "$root/dist/ImageCXX-$Version-windows-x86_64.exe"
