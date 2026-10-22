"""Exercise the macOS packaging audit without Apple tools or signing credentials."""

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


if __name__ == "__main__":
    unittest.main()
