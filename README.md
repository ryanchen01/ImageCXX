# ImageCXX

ImageCXX is a C++20 and Qt Widgets image viewer and analysis application with a compact launcher and separate image and tool windows. It keeps scientific pixel values separate from display settings and provides stack browsing, ROIs, measurements, plots and ITK processing.

Select an area ROI and choose **Analyze > Plot ROI measurements...** to plot mean intensity across Time or Z. The ROI's XY shape, current channel and other axis stay fixed. Plots use calibrated coordinates when available and frame or slice numbers otherwise. Each plot has an **Export CSV...** button for per-plane statistics and keeps its measured values when the source image changes.

## Build

Install these first:

- CMake 3.24+, Ninja and a C++20 compiler.
- The official Qt 6.12.0 desktop SDK with Shader Tools, Graphs, Image Formats, Quick 3D and Quick Timeline.

Run the commands below from the ImageCXX repository. Adjust the Qt path to match your installation.

### macOS with Homebrew

Install Xcode or its command line tools, [Homebrew](https://brew.sh), and the official Qt SDK listed above. Install the remaining dependencies with Homebrew:

```sh
brew install cmake ninja itk dcmtk libtiff googletest google-benchmark
```

Configure, build and test Release:

```sh
export QT_ROOT_DIR="$HOME/Qt/6.12.0/macos"
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The `release` preset uses the installed libraries and writes the build to `build/release`.

### macOS and Linux with vcpkg

Install a local [vcpkg checkout](https://github.com/microsoft/vcpkg). It supplies the dependencies in [vcpkg.json](vcpkg.json). Set `VCPKG_ROOT` to its location.

Point `QT_ROOT_DIR` to your platform's SDK directory, such as `macos` or `gcc_64`.

```sh
export QT_ROOT_DIR=/path/to/Qt/6.12.0/macos
export VCPKG_ROOT=/path/to/vcpkg
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
cmake --preset release-vcpkg
cmake --build --preset release-vcpkg
ctest --preset release-vcpkg
```

### Windows

Install Visual Studio 2022 with the **Desktop development with C++** workload and a Windows SDK. Install Git, CMake 3.24+ and Ninja, and make them available on `PATH`. Install the official Qt 6.12.0 **MSVC 2022 64-bit** SDK with the modules listed above.

Open **Developer PowerShell for VS 2022** with the x64 compiler environment. Set up [vcpkg](https://github.com/microsoft/vcpkg) once:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:/dev/vcpkg
$env:VCPKG_ROOT = "C:/dev/vcpkg"
& "$env:VCPKG_ROOT/bootstrap-vcpkg.bat" -disableMetrics
```

From the ImageCXX repository, configure, build and test Release. Adjust the Qt and vcpkg paths to match your installations:

```powershell
$env:QT_ROOT_DIR = "C:/Qt/6.12.0/msvc2022_64"
$env:VCPKG_ROOT = "C:/dev/vcpkg"
cmake --preset release-vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset release-vcpkg
$env:PATH = "$env:QT_ROOT_DIR/bin;$PWD/build/release-vcpkg/vcpkg_installed/x64-windows/bin;$env:PATH"
ctest --preset release-vcpkg
```

During configuration, vcpkg installs the libraries listed in `vcpkg.json`. The Release build is written to `build/release-vcpkg`.

## Development

Install the dependencies from the Build section, then configure, build and test Debug using the matching setup below.

### macOS with Homebrew

The `dev` preset uses the installed libraries and writes the build to `build/dev`.

```sh
export QT_ROOT_DIR="$HOME/Qt/6.12.0/macos"
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

### macOS and Linux with vcpkg

Use the environment variables from the vcpkg build instructions above. The `dev-vcpkg` preset writes the build to `build/dev-vcpkg`.

```sh
cmake --preset dev-vcpkg
cmake --build --preset dev-vcpkg
ctest --preset dev-vcpkg
```

### Windows

Complete the Windows setup in the Build section. In an x64 Visual Studio 2022 developer PowerShell, run these commands from the ImageCXX repository, adjusting the installation paths as needed:

```powershell
$env:QT_ROOT_DIR = "C:/Qt/6.12.0/msvc2022_64"
$env:VCPKG_ROOT = "C:/dev/vcpkg"
cmake --preset dev-vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset dev-vcpkg
$env:PATH = "$env:QT_ROOT_DIR/bin;$PWD/build/dev-vcpkg/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --preset dev-vcpkg
```

The Debug build is written to `build/dev-vcpkg`.

## License

ImageCXX is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or any later version at your option.

ImageCXX is distributed without any warranty. See [LICENSE](LICENSE) for the full terms.

SPDX-License-Identifier: GPL-3.0-or-later

Third-party dependencies remain under their respective licenses.
