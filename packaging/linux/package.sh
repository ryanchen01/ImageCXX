#!/usr/bin/env bash
set -euo pipefail
version="$1"
root="$PWD"
appdir="$root/build/AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/libexec" "$appdir/usr/share/doc/ImageCXX" "$root/dist"
# linuxdeploy handles deployment on Linux, including vcpkg shared libraries and QML plugins.
cp build/release-vcpkg/ImageCXX "$appdir/usr/bin/"
cp build/tools/appimageupdatetool.AppImage "$appdir/usr/libexec/"
cp LICENSE "$appdir/usr/share/doc/ImageCXX/"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$QT_ROOT_DIR/bin/qmake"
export QML_SOURCES_PATHS="$root/plots"
export QML_MODULES_PATHS="$root/build/release-vcpkg/qml"
export LD_LIBRARY_PATH="$QT_ROOT_DIR/lib:$root/build/release-vcpkg/vcpkg_installed/x64-linux/lib:${LD_LIBRARY_PATH:-}"
export LDAI_UPDATE_INFORMATION="gh-releases-zsync|ryanchen01|ImageCXX|latest|ImageCXX-*-linux-x86_64.AppImage.zsync"
export LDAI_OUTPUT="$root/dist/ImageCXX-$version-linux-x86_64.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export LDAI_NO_APPSTREAM=1
build/tools/linuxdeploy.AppImage --appdir "$appdir" \
    --executable "$appdir/usr/bin/ImageCXX" \
    --desktop-file packaging/linux/com.ryanchen01.ImageCXX.desktop \
    --icon-file resources/icons/imagecxx.svg --icon-filename com.ryanchen01.ImageCXX \
    --plugin qt --output appimage
# appimagetool writes zsync metadata to the current directory.
mv "$root/${LDAI_OUTPUT##*/}.zsync" "$LDAI_OUTPUT.zsync"
test -s "$LDAI_OUTPUT.zsync"
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH -u QML2_IMPORT_PATH -u QML_IMPORT_PATH \
    "$LDAI_OUTPUT" --batch --version | grep -Fx "ImageCXX $version"
# Verify the bundled updater can run without FUSE before publishing it.
"$appdir/usr/libexec/appimageupdatetool.AppImage" --version
