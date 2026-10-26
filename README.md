# ImageCXX

ImageCXX is a C++20 and Qt Widgets image viewer and analysis application with a compact launcher and separate image and tool windows. It keeps scientific pixel values separate from display settings and provides stack browsing, ROIs, measurements, plots and ITK processing.

## Build

Install these first:

- CMake 3.24+, Ninja and a C++20 compiler.
- The official Qt 6.12.0 desktop SDK with Shader Tools, Graphs, Image Formats, Quick 3D and Quick Timeline.
- A local [vcpkg checkout](https://github.com/microsoft/vcpkg). It supplies the dependencies in [vcpkg.json](vcpkg.json). Use the official SDK for Qt.

The commands below build Release. Run them from the ImageCXX repository. Replace the Qt and vcpkg paths with your own.

### macOS and Linux

Point `QT_ROOT_DIR` to your platform's SDK directory, such as `macos` or `gcc_64`.

```sh
export QT_ROOT_DIR=/path/to/Qt/6.12.0/macos
export VCPKG_ROOT=/path/to/vcpkg
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
cmake --preset release-vcpkg
cmake --build --preset release-vcpkg
ctest --preset release-vcpkg
```

Open `build/release-vcpkg/ImageCXX.app` on macOS, or run `build/release-vcpkg/ImageCXX` on Linux.

### Windows

Use an x64 Visual Studio 2022 developer PowerShell:

```powershell
$env:QT_ROOT_DIR = "C:/Qt/6.12.0/msvc2022_64"
$env:VCPKG_ROOT = "C:/path/to/vcpkg"
& "$env:VCPKG_ROOT/bootstrap-vcpkg.bat" -disableMetrics
cmake --preset release-vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset release-vcpkg
$env:PATH = "$env:QT_ROOT_DIR/bin;$PWD/build/release-vcpkg/vcpkg_installed/x64-windows/bin;$env:PATH"
ctest --preset release-vcpkg
./build/release-vcpkg/ImageCXX.exe
```

## Development

Use the dependencies and environment variables from the build instructions above. The `dev-vcpkg` preset builds Debug in `build/dev-vcpkg`.

On macOS and Linux:

```sh
cmake --preset dev-vcpkg
cmake --build --preset dev-vcpkg
ctest --preset dev-vcpkg
```

On Windows, use an x64 Visual Studio 2022 developer PowerShell:

```powershell
cmake --preset dev-vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset dev-vcpkg
$env:PATH = "$env:QT_ROOT_DIR/bin;$PWD/build/dev-vcpkg/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --preset dev-vcpkg
```

## License

ImageCXX is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or any later version at your option.

ImageCXX is distributed without any warranty. See [LICENSE](LICENSE) for the full terms.

SPDX-License-Identifier: GPL-3.0-or-later

Third-party dependencies remain under their respective licenses.
