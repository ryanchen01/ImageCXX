#!/usr/bin/env bash
# Register an AppImage for this user's Open With / default-app settings.
set -euo pipefail
if [[ $# != 1 ]]; then
    echo "Usage: $0 /path/to/ImageCXX.AppImage" >&2
    exit 2
fi
appimage=$(realpath -- "$1")
test -x "$appimage"
command -v update-mime-database >/dev/null
command -v update-desktop-database >/dev/null
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$data_home"
data_home=$(realpath -- "$data_home")
workdir=$(mktemp -d)
trap 'rm -rf -- "$workdir"' EXIT
(
    cd "$workdir"
    "$appimage" --appimage-extract 'usr/share/mime/packages/com.ryanchen01.ImageCXX.xml' >/dev/null
    "$appimage" --appimage-extract 'usr/share/applications/com.ryanchen01.ImageCXX.desktop' >/dev/null
    "$appimage" --appimage-extract 'usr/share/icons/hicolor/scalable/apps/com.ryanchen01.ImageCXX.svg' >/dev/null
)
share="$workdir/squashfs-root/usr/share"
mkdir -p "$data_home/mime/packages" "$data_home/applications" "$data_home/icons/hicolor/scalable/apps"
cp "$share/mime/packages/com.ryanchen01.ImageCXX.xml" "$data_home/mime/packages/"
cp "$share/icons/hicolor/scalable/apps/com.ryanchen01.ImageCXX.svg" "$data_home/icons/hicolor/scalable/apps/"
# Desktop-entry escaping is applied before the quoted Exec argument is parsed.
executable=${appimage//\\/\\\\\\\\}
executable=${executable//\"/\\\\\"}
executable=${executable//\$/\\\\\$}
executable=${executable//\`/\\\\\`}
executable=${executable//%/%%}
while IFS= read -r line; do
    if [[ "$line" == Exec=* ]]; then
        printf 'Exec="%s" -- %%F\n' "$executable"
    else
        printf '%s\n' "$line"
    fi
done < "$share/applications/com.ryanchen01.ImageCXX.desktop" > "$data_home/applications/com.ryanchen01.ImageCXX.desktop"
update-mime-database "$data_home/mime"
update-desktop-database "$data_home/applications"
echo "ImageCXX is registered. Choose it in your file manager's Open With settings."
