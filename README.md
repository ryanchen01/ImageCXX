# ImageCXX

ImageCXX is a C++20 and Qt Widgets image viewer and analysis application with a compact launcher and separate image and tool windows. It keeps scientific pixel values separate from display settings and provides stack browsing, ROIs, measurements, plots and ITK processing.

Use the View menu to open a linked XY window or orthogonal XY/XZ/YZ windows. Each linked view opens in a separate window and shares slice/channel/time navigation, display settings and pan/zoom with its document. Move and resize the windows however you prefer; unrelated image documents remain independent.

Orthogonal views require a Z stack and follow the stored image axes, with calibrated pixel proportions when the axes use the same unit. Each window has its own slice slider: Z in XY, Y in XZ, and X in YZ. Channel and time selection is available in the Display panel. With the Pan tool, click or drag to move the shared intersection. The wheel browses the hovered plane; Ctrl or Command plus the wheel zooms, and middle-button or Space plus dragging pans. Selection editing, measurements and pixel commands use XY. Every window reads the document's shared image storage, and edits and undo refresh all linked windows.

The image model and undo history are in `core/`, format adapters in `io/`, and ITK filters and measurements in `processing/`. The `app/` module owns commands, batch processing and background jobs; `ui/`, `viewer/` and `plots/` provide the desktop interface. See [format support and save limits](io/README.md), [keyboard shortcuts](docs/shortcuts.md) and [unfinished work](TODO.md).

## Build

Install CMake 3.24 or later, Ninja, a C++20 compiler and the official Qt **6.12.0** desktop SDK. Add Qt Shader Tools, Qt Graphs and Qt Image Formats. Qt SVG is included in the base desktop SDK and must not be requested as an aqt add-on module. This Qt SDK also needs Qt Quick 3D as a transitive dependency of Qt Graphs, although ImageCXX uses only 2D plots. Include Qt Quick Timeline for the Qt Quick 3D QML plugins deployed in release packages. The desktop SDK also needs Qt Declarative, including Qt Quick, QML and QuickWidgets. Keep the matching Qt private development headers installed because the viewer uses Qt RHI behind its internal interface.

The supported dependency route is the [vcpkg manifest](vcpkg.json), pinned to baseline `bf19794383e8fb95eba5aa553235bc8ec2271688`. Qt comes from the official SDK, while ITK, DCMTK, libtiff, GoogleTest and Google Benchmark come from vcpkg. Do not install another Qt through vcpkg.

On macOS or Linux, with the SDK and vcpkg checked out locally:

```sh
export QT_ROOT_DIR=/path/to/Qt/6.12.0/macos
export VCPKG_ROOT=/path/to/vcpkg
git -C "$VCPKG_ROOT" checkout bf19794383e8fb95eba5aa553235bc8ec2271688
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
cmake --preset dev-vcpkg
cmake --build --preset dev-vcpkg
ctest --preset dev-vcpkg
```

Set `QT_ROOT_DIR` to the actual platform SDK directory. `QTDIR` is also accepted. The presets append `CMAKE_PREFIX_PATH` from the environment, using CMake's semicolon-separated list syntax. A command-line `-DCMAKE_PREFIX_PATH=...` overrides that setting.

On Windows, run these commands in an x64 Visual Studio 2022 developer PowerShell, after setting `$env:QT_ROOT_DIR` to the `msvc2022_64` SDK and `$env:VCPKG_ROOT` to vcpkg:

```powershell
git -C $env:VCPKG_ROOT checkout bf19794383e8fb95eba5aa553235bc8ec2271688
& "$env:VCPKG_ROOT/bootstrap-vcpkg.bat" -disableMetrics
cmake --preset dev-vcpkg -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build --preset dev-vcpkg
ctest --preset dev-vcpkg
```

Keep Qt's `bin` and vcpkg's `build/dev-vcpkg/vcpkg_installed/x64-windows/debug/bin` on `PATH` when running a Debug build on Windows. Use the triplet's `bin` directory for Release. The `dev` and `release` presets use dependencies already installed on the machine instead of enabling the vcpkg toolchain. Those builds are useful locally, but their dependency versions are not controlled by the manifest.

Start `build/dev-vcpkg/ImageCXX` on Linux, `build/dev-vcpkg/ImageCXX.exe` on Windows, or `build/dev-vcpkg/ImageCXX.app` on macOS. You can also pass image paths as command-line arguments.

## Checks and benchmarks

```sh
cmake --preset release-vcpkg -DIMAGECXX_BUILD_BENCHMARKS=ON
cmake --build --preset release-vcpkg
ctest --preset release-vcpkg --output-on-failure
```

The `sanitizers` and `sanitizers-vcpkg` presets enable AddressSanitizer and UndefinedBehaviorSanitizer with Clang or GCC. Headless widget tests use Qt's offscreen platform and skip GPU-only checks. Run `imagecxx_ui_tests` on a native desktop without `QT_QPA_PLATFORM=offscreen` to include GPU rendering checks.

Run `imagecxx_benchmarks` from the selected build directory with `--benchmark_out=benchmarks.json --benchmark_out_format=json`. It measures shared snapshots, slice views, buffer copies, histograms and ITK Gaussian filtering on deterministic generated inputs. These are CPU microbenchmarks. Record hardware, OS, compiler, build type and dependency versions alongside results before comparing runs. They do not measure launch time, GPU frame timing or real file decoding.

The [GitHub Actions workflow](.github/workflows/build.yml) defines Windows, macOS and Linux build/test jobs with cached dependencies. Linux also runs graphics and interactive plot tests under Xvfb with software rendering. The [release workflow](.github/workflows/release.yml) builds tagged installers and update packages. See [releasing and updates](docs/releases.md) for signing secrets, tag names, and platform behavior. CI requires the pinned Qt SDK and its add-on archives to be available from Qt's download service.

`clang-format` uses [.clang-format](.clang-format), and `clang-tidy` can use the generated `compile_commands.json` with [.clang-tidy](.clang-tidy).

## License

ImageCXX is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or any later version at your option.

ImageCXX is distributed without any warranty. See [LICENSE](LICENSE) for the full terms.

SPDX-License-Identifier: GPL-3.0-or-later

Third-party dependencies remain under their respective licenses.
