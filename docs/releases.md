# Releases and updates

Push a stable tag such as `v0.3.0` or `0.3.0` to run `.github/workflows/release.yml`. The tag supplies the CMake application version, Windows component and installer versions, macOS bundle and appcast versions, and artifact filenames. Tags must contain exactly three numeric components without leading zeroes. Prerelease tags are not supported.

The workflow builds and tests Windows x64, macOS arm64, and Linux x64 on the existing CI runners. Linux packages use Ubuntu 24.04 as their build baseline. It uploads these assets to a draft GitHub Release, then publishes it after every platform succeeds:

- `ImageCXX-VERSION-windows-x86_64.exe`
- `ImageCXX-VERSION-macos-arm64.dmg`
- `ImageCXX-VERSION-linux-x86_64.AppImage`
- The AppImage's `.zsync` file
- `appcast.xml`
- `SHA256SUMS`

The release repository is `ryanchen01/ImageCXX`. Its API, asset paths, Sparkle feed, and AppImage update information are public URLs. A private repository would need a different distribution arrangement. Keep tags and published assets immutable. The automatic updater ignores drafts, prereleases, equal versions, and older versions.

## Signing secrets

The packaging jobs use the GitHub environment named `release`. Its deployment rules must allow release tags. macOS uses these secrets:

| Secret | Value |
| --- | --- |
| `MACOS_CERTIFICATE_P12` | Base64 of a Developer ID Application certificate and its private key exported as P12 |
| `MACOS_CERTIFICATE_PASSWORD` | The P12 password |
| `APPSTORE_API_KEY_ID` | App Store Connect API key ID |
| `APPSTORE_API_PRIVATE_KEY` | The `.p8` key contents, either PEM text or base64 |
| `APPSTORE_ISSUER_ID` | The API key's issuer UUID |
| `SPARKLE_PRIVATE_KEY` | The base64 text exported by Sparkle's `generate_keys -x`, without encoding it a second time |

The matching Sparkle public key is already embedded in `packaging/macos/Info.plist.in`. CI verifies each DMG's EdDSA signature against that public key before publishing. Back up the private key; replacing the embedded public key without a migration would prevent existing installations from accepting updates.

macOS packaging preserves framework symlinks, signs nested Mach-O binaries and bundles with the hardened runtime, notarizes and staples the app, then signs, notarizes, and staples the DMG. The temporary certificate keychain and authentication files are removed when the packaging script exits. Appcast and installer URLs refer to the exact tag. The appcast declares macOS 14.4 or newer and Apple Silicon.

Windows installers do not use Authenticode signing. Windows may show an unknown-publisher prompt.

## Update behavior

Release builds enable `IMAGECXX_ENABLE_UPDATES`. GUI startup sends one asynchronous request to the GitHub Releases API using Qt Network. Unavailable networks, API errors, and rate limits are silent. Batch mode never checks for updates. Downloads run only after the user chooses to install. The existing unsaved-image, measurement, and running-job prompts still control shutdown.

On Windows, the app downloads the matching EXE and checks its size and SHA-256 against the release asset metadata. It then closes through the normal window close handler and launches the installer. Canceling shutdown leaves the current application running.

The EXE contains an IFW repository, so fresh installations and upgrades work offline after downloading the EXE. New installations default to `%LOCALAPPDATA%/Programs/ImageCXX`. A stable registry entry under `HKCU\Software\ImageCXX` records the chosen directory. An upgrade validates the existing component registration and maintenance tool, then runs that maintenance tool against the EXE's extracted repository. IFW removes the previous component's tracked files, installs the new component, and retains its rollback and uninstall operation log. It never starts by uninstalling the entire application or deleting its directory. The maintenance tool displays its update wizard. A mismatched or damaged registration stops the upgrade. Source builds and installations made by other installers need an initial IFW installation.

On macOS, the GitHub check hands a newer release to Sparkle. Sparkle uses `releases/latest/download/appcast.xml`, verifies the EdDSA signature and app signing identity, installs the app, and relaunches it. Canceling an unsaved-work prompt postpones a prepared update until a later accepted quit. The DMG is also available for manual installation.

On Linux, updates are offered only when running as an AppImage. The bundled AppImageUpdate CLI uses the exact release's `.zsync` URL and reuses blocks from the existing AppImage. It operates on a staging copy beside the original. The app verifies the completed file against GitHub's SHA-256 before replacing anything. After accepted shutdown, renames replace the original file and retain the old image as `NAME.AppImage.zs-old`. Failed replacement attempts restore the original. The updated AppImage then relaunches. The containing directory must be writable and have room for staging and a backup. Native source installations do not update themselves.

Windows and Linux download verification trusts GitHub's HTTPS API and release asset digests. macOS additionally requires the separate Sparkle update signature. No GitHub access token is shipped in the app.

## Local checks

Ordinary developer builds leave startup checking disabled. To test the macOS integration locally, download the verified Sparkle distribution and configure the build explicitly:

```sh
bash packaging/download-tools.sh macos
cmake --preset release -DIMAGECXX_ENABLE_UPDATES=ON \
  -DIMAGECXX_SPARKLE_ROOT="$PWD/build/tools" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.4
cmake --build --preset release
ctest --preset release --output-on-failure
node tests/installer_tests.js
```

Windows release CI checks a fresh installation of the shipped EXE, then installs a small older IFW package, updates it through the actual maintenance tool, and checks obsolete-file removal, preservation of untracked data and settings, and execution using bundled DLLs. The JavaScript test checks bootstrap detection, routing, and refusal paths without modifying a real installation.

`packaging/download-tools.sh` checks download hashes before running third-party tools. The Linux upstreams publish mutable `continuous` assets. If one changes, review the new tool and update its pinned hash; a mismatch deliberately fails packaging.

Before distributing the first release, test fresh installation and version-to-version updating on each supported OS, including cancellation and unwritable installation directories. GitHub-hosted signing and notarization require a tagged run and cannot be validated with local unit tests.
