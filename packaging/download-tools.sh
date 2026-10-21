#!/usr/bin/env bash
set -euo pipefail
mkdir -p build/tools

download() {
    curl --fail --location --retry 3 "$1" --output "build/tools/$2"
    echo "$3  build/tools/$2" | shasum -a 256 -c -
}

case "$1" in
    macos)
        download https://github.com/sparkle-project/Sparkle/releases/download/2.10.0/Sparkle-2.10.0.tar.xz Sparkle.tar.xz c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c
        tar xf build/tools/Sparkle.tar.xz -C build/tools
        ;;
    linux)
        download https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage linuxdeploy.AppImage 8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1
        download https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage linuxdeploy-plugin-qt-x86_64.AppImage cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
        download https://github.com/AppImageCommunity/AppImageUpdate/releases/download/continuous/appimageupdatetool-x86_64.AppImage appimageupdatetool.AppImage 8d17a50e2f7502edacab48216d1b491de3669935858591ea0026cc2db375967c
        chmod +x build/tools/*.AppImage
        ;;
    *) echo "Expected macos or linux" >&2; exit 1 ;;
esac
