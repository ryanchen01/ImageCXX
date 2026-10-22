#!/usr/bin/env bash
set -euo pipefail
version="$1"
: "${MACOS_CERTIFICATE_P12:?Missing release environment certificate}"
: "${MACOS_CERTIFICATE_PASSWORD:?Missing certificate password}"
: "${APPSTORE_API_KEY_ID:?Missing App Store Connect key ID}"
: "${APPSTORE_API_PRIVATE_KEY:?Missing App Store Connect key}"
: "${APPSTORE_ISSUER_ID:?Missing App Store Connect issuer ID}"
: "${SPARKLE_PRIVATE_KEY:?Missing Sparkle signing key}"
root="$PWD"
stage="$root/build/macos-package"
app="$stage/ImageCXX.app"
temporary=$(mktemp -d)
keychain="$temporary/release.keychain-db"
keychain_password=$(openssl rand -hex 32)
old_keychains=()
while IFS= read -r line; do old_keychains+=("$line"); done < <(security list-keychains -d user | sed 's/^ *"//;s/"$//')
cleanup() {
    security list-keychains -d user -s "${old_keychains[@]}"
    security delete-keychain "$keychain" || true
    rm -rf "$temporary"
}
trap cleanup EXIT
umask 077
python3 - "$temporary" <<'PY'
import base64, binascii, os, pathlib, sys
folder = pathlib.Path(sys.argv[1])
sparkle_key = "".join(os.environ["SPARKLE_PRIVATE_KEY"].split())
try:
    sparkle_secret = base64.b64decode(sparkle_key, validate=True)
except (ValueError, binascii.Error):
    raise SystemExit("SPARKLE_PRIVATE_KEY must be base64 key data exported with Sparkle generate_keys -x.")
# Sparkle 2.10 accepts a 32-byte seed or a 96-byte legacy key pair.
if len(sparkle_secret) not in (32, 96):
    raise SystemExit("SPARKLE_PRIVATE_KEY must decode to a 32-byte seed or 96-byte legacy Sparkle key pair.")
(folder / "sparkle-private-key").write_text(sparkle_key + "\n")
(folder / "certificate.p12").write_bytes(base64.b64decode("".join(os.environ["MACOS_CERTIFICATE_P12"].split()), validate=True))
key = os.environ["APPSTORE_API_PRIVATE_KEY"].strip()
(folder / "AuthKey.p8").write_bytes(key.encode() if key.startswith("-----BEGIN") else base64.b64decode("".join(key.split()), validate=True))
PY
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security import "$temporary/certificate.p12" -k "$keychain" -P "$MACOS_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain" >/dev/null
security list-keychains -d user -s "$keychain" "${old_keychains[@]}"
identity=$(security find-identity -v -p codesigning "$keychain" | sed -n 's/.* \([A-F0-9]\{40\}\) "Developer ID Application:.*$/\1/p' | head -1)
test -n "$identity"
mkdir -p "$stage" "$root/dist"
ditto "$root/build/release-vcpkg/ImageCXX.app" "$app"
"$QT_ROOT_DIR/bin/macdeployqt" "$app" -qmldir="$root/plots" -always-overwrite -no-codesign
mkdir -p "$app/Contents/Resources/licenses"
cp LICENSE "$app/Contents/Resources/licenses/ImageCXX.txt"
python3 - "$app" "$identity" "$keychain" <<'PY'
import os, pathlib, subprocess, sys
app, identity, keychain = sys.argv[1:]
# QML can pull in QtSql; external database drivers require vendor SDKs we do not use.
for driver in (pathlib.Path(app) / "Contents/PlugIns/sqldrivers").glob("*.dylib"):
    if driver.name != "libqsqlite.dylib":
        driver.unlink()
magic = {bytes.fromhex(value) for value in ("feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca", "cafebabf", "bfbafeca")}
dependency_commands = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}
for folder, dirs, files in os.walk(app, topdown=False):
    for name in files:
        path = pathlib.Path(folder) / name
        if path.is_symlink():
            continue
        with path.open("rb") as file:
            is_macho = file.read(4) in magic
        if is_macho:
            # Unlike -L, inspect dependency commands without treating LC_ID_DYLIB as a load.
            load_commands = subprocess.check_output(["otool", "-l", str(path)], text=True).splitlines()
            command = None
            for line in load_commands:
                fields = line.strip().split(maxsplit=1)
                if len(fields) != 2:
                    continue
                field, value = fields
                if field == "cmd":
                    command = value
                elif field == "name" and command in dependency_commands:
                    library = value.rsplit(" (offset ", 1)[0]
                    if library.startswith("/") and not library.startswith(("/System/Library/", "/usr/lib/", "/Library/Apple/System/Library/")):
                        raise RuntimeError(f"Unbundled dependency in {path}: {library}")
            subprocess.run(["codesign", "--force", "--sign", identity, "--keychain", keychain,
                "--options", "runtime", "--timestamp", "--preserve-metadata=entitlements", str(path)], check=True)
    if folder != app and folder.endswith((".app", ".xpc", ".framework", ".bundle")):
        subprocess.run(["codesign", "--force", "--sign", identity, "--keychain", keychain,
            "--options", "runtime", "--timestamp", "--preserve-metadata=entitlements", folder], check=True)
PY
codesign --force --sign "$identity" --keychain "$keychain" --options runtime --timestamp \
    --entitlements packaging/macos/entitlements.plist "$app"
codesign --verify --deep --strict --verbose=2 "$app"
env -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH -u QT_PLUGIN_PATH -u QML2_IMPORT_PATH -u QML_IMPORT_PATH \
    "$app/Contents/MacOS/ImageCXX" --batch --version | grep -Fx "ImageCXX $version"
ditto -c -k --sequesterRsrc --keepParent "$app" "$temporary/ImageCXX.zip"
xcrun notarytool submit "$temporary/ImageCXX.zip" --key "$temporary/AuthKey.p8" \
    --key-id "$APPSTORE_API_KEY_ID" --issuer "$APPSTORE_ISSUER_ID" --wait --output-format json > "$temporary/app-notary.json"
python3 -c 'import json,sys; assert json.load(open(sys.argv[1]))["status"] == "Accepted", "App notarization failed"' "$temporary/app-notary.json"
xcrun stapler staple "$app"
spctl --assess --type execute --verbose=2 "$app"
ln -s /Applications "$stage/Applications"
dmg="$root/dist/ImageCXX-$version-macos-arm64.dmg"
hdiutil create -volname ImageCXX -srcfolder "$stage" -format UDZO -ov "$dmg"
codesign --sign "$identity" --keychain "$keychain" --timestamp "$dmg"
xcrun notarytool submit "$dmg" --key "$temporary/AuthKey.p8" \
    --key-id "$APPSTORE_API_KEY_ID" --issuer "$APPSTORE_ISSUER_ID" --wait --output-format json > "$temporary/dmg-notary.json"
python3 -c 'import json,sys; assert json.load(open(sys.argv[1]))["status"] == "Accepted", "DMG notarization failed"' "$temporary/dmg-notary.json"
xcrun stapler staple "$dmg"
xcrun stapler validate "$dmg"
if ! signature=$(build/tools/bin/sign_update --ed-key-file "$temporary/sparkle-private-key" -p "$dmg"); then
    # Sparkle can print private key input in its stdout diagnostics. Keep that output private.
    echo 'Sparkle update signing failed after key-format validation; check the exported Sparkle key and signer compatibility.' >&2
    exit 1
fi
public_key=$(/usr/libexec/PlistBuddy -c 'Print :SUPublicEDKey' "$app/Contents/Info.plist")
# Fail the release if the CI private key does not match the public key shipped in the app.
swift - "$dmg" "$signature" "$public_key" <<'SWIFT'
import Foundation
import CryptoKit
let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]), options: .mappedIfSafe)
let key = try Curve25519.Signing.PublicKey(rawRepresentation: Data(base64Encoded: CommandLine.arguments[3])!)
guard let signature = Data(base64Encoded: CommandLine.arguments[2]), key.isValidSignature(signature, for: data) else {
    fatalError("Sparkle signature does not match the embedded public key")
}
SWIFT
python3 - "$version" "$dmg" "$signature" "$GITHUB_REF_NAME" <<'PY'
import pathlib, sys, xml.etree.ElementTree as ET
version, dmg, signature, tag = sys.argv[1:]
namespace = "http://www.andymatuschak.org/xml-namespaces/sparkle"
ET.register_namespace("sparkle", namespace)
rss = ET.Element("rss", version="2.0")
channel = ET.SubElement(rss, "channel")
ET.SubElement(channel, "title").text = "ImageCXX updates"
item = ET.SubElement(channel, "item")
ET.SubElement(item, "title").text = "ImageCXX " + version
ET.SubElement(item, "{" + namespace + "}version").text = version
ET.SubElement(item, "{" + namespace + "}minimumSystemVersion").text = "14.4.0"
ET.SubElement(item, "{" + namespace + "}hardwareRequirements").text = "arm64"
ET.SubElement(item, "enclosure", {
    "url": "https://github.com/ryanchen01/ImageCXX/releases/download/" + tag + "/" + pathlib.Path(dmg).name,
    "length": str(pathlib.Path(dmg).stat().st_size), "type": "application/octet-stream",
    "{" + namespace + "}edSignature": signature})
ET.indent(rss)
ET.ElementTree(rss).write("dist/appcast.xml", encoding="utf-8", xml_declaration=True)
PY
