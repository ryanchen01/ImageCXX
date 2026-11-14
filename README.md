# ImageCXX

ImageCXX is a desktop viewer and analysis tool for scientific and medical images (such as microscopy stacks, CT/MRI volumes, and gel/blot scans) on macOS, Windows, and Linux.

It keeps your original pixel data separate from display settings, so adjusting contrast, zoom, color maps, or annotations never alters your underlying scientific measurements.

## What You Can Do

- **Open scientific and medical formats:** Load TIFF/BigTIFF, DICOM (single files and 3D series), NIfTI, NRRD, MetaImage (MHA/MHD), and raw binary files (as well as PNG and JPEG). Drag and drop files or folders directly onto the window.
- **Browse multi-dimensional stacks:** Step through depth slices (Z), color channels (C), and time frames (T), or compare images side by side and in 3D using **Single**, **Linked**, **Orthogonal**, and **Overlay** views.
- **Adjust display non-destructively:** Tune intensity range, gamma, inversion, and channel/color lookup tables (LUTs), add display-only annotations (`B`), and print with calibrated aspect ratios.
- **Measure, plot, and export:** Draw rectangle, ellipse, polygon, or line selections (ROIs) to inspect intensity statistics, histograms (`H` / `Shift+H`), line profiles (`K`), ROI time/Z plots, and gel/blot lanes, then export results to CSV.
- **Process images and batches:** Apply Gaussian blur, median filtering, thresholding, binary dilation/erosion, resampling, reslicing, and 3D projections on a full image, current slice, or selection, with undo/redo and batch processing.

## Quick Tips

- **Find any command fast:** Press `L` (or `Ctrl+L` / `Cmd+L`) to search commands in the bottom bar, or `Shift+U` to open the Control Panel.
- **Navigate and pan:** Hold `Space` and drag (or middle-drag) to pan; scroll or press `Left`/`Right` to step through stack slices.
- **More shortcuts and format details:** See [Keyboard Shortcuts](docs/shortcuts.md) for canvas shortcuts, selection modifiers, and copy/paste behavior, and [Image I/O](docs/io.md) for format-specific details.

## Build

Install these first:

- CMake 3.24+, Ninja and a C++20 compiler.
- The official Qt 6.12.0 desktop SDK with Qt Linguist tools, Shader Tools, Graphs, Image Formats, Quick 3D and Quick Timeline.

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

### Translations

User-facing source text is English. Keep UI text in `tr()` (or
`QCoreApplication::translate()` outside QObject classes), and use `qsTr()` in QML.
Command IDs, parameter values, file-format keys and scientific unit symbols stay stable.
Choice and category values use `QT_TRANSLATE_NOOP` so they are extracted without
changing their stored values; translate them when displaying them.

Update every configured catalog after changing source text:

```sh
cmake --build --preset dev --target update_translations
```

`ImageCXX_en_US.ts` is the English reference catalog. To add languages, configure
with a semicolon-separated locale list, then update the catalogs:

```sh
cmake --preset dev -DIMAGECXX_TRANSLATION_LANGUAGES="en_US;fr_FR;zh_CN"
cmake --build --preset dev --target update_translations
```

Edit the generated `ImageCXX_<locale>.ts` files with Qt Linguist and commit them.
`lupdate` preserves translations, adds new strings as unfinished, and marks removed
strings obsolete. Keep the locale list in your build configuration when building
or packaging those languages.

```sh
cmake --build --preset dev --target release_translations
cmake --build --preset dev
```

The normal build also compiles and embeds the catalogs. At startup, the app selects
an embedded translation using the system UI locale; unfinished or missing entries
fall back to the English source text. Restart the app after changing the locale.
Qt's standard dialog translations are loaded from Qt's installed translation directory
when available. Diagnostics supplied by external libraries retain their own wording.

## License

ImageCXX is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or any later version at your option.

ImageCXX is distributed without any warranty. See [LICENSE](LICENSE) for the full terms.

SPDX-License-Identifier: GPL-3.0-or-later

Third-party dependencies remain under their respective licenses.
