"""Exercise the macOS packaging audit without Apple tools or signing credentials."""

import base64
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "packaging/macos/package.sh").read_text()
AUDIT = SOURCE.split('python3 - "$app" "$identity" "$keychain" <<\'PY\'\n', 1)[1].split("\nPY\n", 1)[0]
DEPENDENCY_COMMANDS = (
    "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
    "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB",
)
KEY_PREPARATION = SOURCE.split('python3 - "$temporary" <<\'PY\'\n', 1)[1].split("\nPY\n", 1)[0]
SIGN_UPDATE = SOURCE[SOURCE.index("if ! signature=$("):SOURCE.index("\npublic_key=")]


class MacosPackageTests(unittest.TestCase):
    def run_audit(self, commands, *, universal=False, tool_error=False):
        with tempfile.TemporaryDirectory() as folder:
            app = pathlib.Path(folder) / "ImageCXX.app"
            binary = app / "Contents/PlugIns/platforms/libqcocoa.dylib"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(bytes.fromhex("cafebabe" if universal else "cffaedfe"))
            entries = [(command, str(binary) if name is None else name) for command, name in commands]
            output = ""
            for arch in (["arm64", "x86_64"] if universal else [None]):
                output += f"{binary}" + (f" (architecture {arch})" if arch else "") + ":\n"
                for number, (command, name) in enumerate(entries):
                    output += (
                        f"Load command {number}\n          cmd {command}\n      cmdsize 80\n"
                        f"         name {name} (offset 24)\n"
                        "   time stamp 1 Thu Jan  1 00:00:01 1970\n"
                        "      current version 1.0.0\ncompatibility version 1.0.0\n"
                    )

            def otool(args, **kwargs):
                self.assertEqual(args, ["otool", "-l", str(binary)])
                self.assertTrue(kwargs["text"])
                if tool_error:
                    raise subprocess.CalledProcessError(1, args)
                return output

            with patch.object(sys, "argv", ["audit", str(app), "test-identity", "test-keychain"]), \
                    patch.object(subprocess, "check_output", side_effect=otool), \
                    patch.object(subprocess, "run") as sign:
                try:
                    exec(compile(AUDIT, "<macos-package-audit>", "exec"), {})
                except Exception:
                    sign.assert_not_called()
                    raise
                sign.assert_called_once()
                self.assertEqual(sign.call_args.args[0][-1], str(binary))
                self.assertTrue(sign.call_args.kwargs["check"])

    def test_absolute_self_id_with_bundled_and_system_dependencies(self):
        self.run_audit([
            ("LC_ID_DYLIB", None),
            ("LC_LOAD_DYLIB", "@rpath/QtCore.framework/Versions/A/QtCore"),
            ("LC_LOAD_DYLIB", "@loader_path/../Frameworks/libgood.dylib"),
            ("LC_LOAD_DYLIB", "/usr/lib/libSystem.B.dylib"),
            ("LC_LOAD_DYLIB", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"),
            ("LC_LOAD_DYLIB", "/Library/Apple/System/Library/Frameworks/libgood.dylib"),
        ])

    def test_external_loads_are_rejected_with_or_without_self_id(self):
        for command in DEPENDENCY_COMMANDS:
            for prefix in ([], [("LC_ID_DYLIB", None)]):
                with self.subTest(command=command, has_id=bool(prefix)):
                    with self.assertRaisesRegex(RuntimeError, "/opt/vendor SDK/libbad.dylib"):
                        self.run_audit(prefix + [(command, "/opt/vendor SDK/libbad.dylib")])

    def test_load_matching_self_id_is_still_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Unbundled dependency"):
            self.run_audit([("LC_ID_DYLIB", None), ("LC_LOAD_DYLIB", None)])

    def test_executable_with_system_dependency(self):
        self.run_audit([("LC_LOAD_DYLIB", "/usr/lib/libSystem.B.dylib")])

    def test_universal_binary_ignores_ids_and_architecture_headers(self):
        self.run_audit([
            ("LC_ID_DYLIB", None), ("LC_LOAD_DYLIB", "@rpath/libgood.dylib"),
        ], universal=True)

    def test_universal_binary_external_dependency_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "/usr/local/lib/libbad.dylib"):
            self.run_audit([
                ("LC_ID_DYLIB", None), ("LC_LOAD_DYLIB", "/usr/local/lib/libbad.dylib"),
            ], universal=True)

    def test_otool_failure_stops_before_signing(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_audit([], tool_error=True)


class SparkleSigningTests(unittest.TestCase):
    def prepare_key(self, value):
        with tempfile.TemporaryDirectory() as folder:
            env = dict(os.environ, SPARKLE_PRIVATE_KEY=value,
                       MACOS_CERTIFICATE_P12=base64.b64encode(b"test-certificate").decode(),
                       APPSTORE_API_PRIVATE_KEY="-----BEGIN TEST KEY-----")
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", 'umask 077\npython3 - "$1" <<\'PY\'\n' + KEY_PREPARATION + "\nPY\n", "test", folder],
                env=env, text=True, capture_output=True,
            )
            key_file = pathlib.Path(folder) / "sparkle-private-key"
            if result.returncode == 0:
                self.assertEqual(key_file.stat().st_mode & 0o777, 0o600)
                self.assertEqual(result.stdout + result.stderr, "")
                return result, key_file.read_text()
            self.assertFalse(key_file.exists())
            return result, None

    def test_modern_and_legacy_exported_keys(self):
        for size in (32, 96):
            with self.subTest(size=size):
                encoded = base64.b64encode(bytes(range(size))).decode()
                result, content = self.prepare_key(encoded)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(content, encoded + "\n")

    def test_exported_key_whitespace_is_normalized(self):
        encoded = base64.b64encode(bytes(range(32))).decode()
        result, content = self.prepare_key("\r\n  " + encoded[:20] + "\n" + encoded[20:] + " \r\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(content, encoded + "\n")

    def test_malformed_keys_fail_without_disclosing_input(self):
        for value in ("synthetic-private-key-must-not-be-logged!", base64.b64encode(bytes(range(64))).decode(), ""):
            with self.subTest(size=len(value)):
                result, content = self.prepare_key(value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("SPARKLE_PRIVATE_KEY must", result.stderr)
                if value:
                    self.assertNotIn(value, result.stdout + result.stderr)
                self.assertIsNone(content)

    def test_signer_failure_does_not_disclose_its_stdout(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            signer = root / "build/tools/bin/sign_update"
            signer.parent.mkdir(parents=True)
            signer.write_text('#!/bin/sh\nprintf "%s\\n" "ERROR! synthetic-secret-must-not-be-logged"\nexit 1\n')
            signer.chmod(0o755)
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", 'temporary="$PWD"\ndmg=test.dmg\n' + SIGN_UPDATE],
                cwd=root, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("Sparkle update signing failed", result.stderr)
            self.assertNotIn("synthetic-secret", result.stdout + result.stderr)

    def test_signer_uses_key_file_and_preserves_signature(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            (root / "sparkle-private-key").write_text("synthetic-exported-key\n")
            signer = root / "build/tools/bin/sign_update"
            signer.parent.mkdir(parents=True)
            signer.write_text('#!/bin/sh\ntest "$1" = --ed-key-file && test -f "$2" && test "$3" = -p && test "$4" = test.dmg || exit 1\nprintf "%s\\n" "synthetic-signature"\n')
            signer.chmod(0o755)
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", 'temporary="$PWD"\ndmg=test.dmg\n' + SIGN_UPDATE + '\nprintf "%s" "$signature"'],
                cwd=root, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "synthetic-signature")


if __name__ == "__main__":
    unittest.main()
