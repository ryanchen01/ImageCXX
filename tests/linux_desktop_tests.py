"""Run with python3 tests/linux_desktop_tests.py on Linux."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DesktopIntegration(unittest.TestCase):
    def test_appimage_registers_host_files(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            appimage = work / 'ImageCXX with spaces.AppImage'
            appimage.write_text('''#!/usr/bin/env python3
import os, pathlib, shutil, sys
assert sys.argv[1] == '--appimage-extract'
source = pathlib.Path(os.environ['DESKTOP_TEST_SOURCE'])
files = {
    'usr/share/applications/com.ryanchen01.ImageCXX.desktop': source / 'packaging/linux/com.ryanchen01.ImageCXX.desktop',
    'usr/share/mime/packages/com.ryanchen01.ImageCXX.xml': source / 'packaging/linux/com.ryanchen01.ImageCXX.xml',
    'usr/share/icons/hicolor/scalable/apps/com.ryanchen01.ImageCXX.svg': source / 'resources/icons/imagecxx.svg',
}
target = pathlib.Path('squashfs-root') / sys.argv[2]
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(files[sys.argv[2]], target)
''')
            appimage.chmod(0o755)
            tools = work / 'bin'
            tools.mkdir()
            # MIME compilation is real; record the desktop refresh if its tool is absent.
            refresh = shutil.which('update-desktop-database')
            if not refresh:
                refresh_script = tools / 'update-desktop-database'
                refresh_script.write_text('#!/bin/sh\ntest -f "$1/com.ryanchen01.ImageCXX.desktop"\n')
                refresh_script.chmod(0o755)
            data = work / 'user data'
            env = dict(os.environ, XDG_DATA_HOME=str(data), DESKTOP_TEST_SOURCE=str(root),
                       PATH=str(tools) + os.pathsep + os.environ['PATH'])
            subprocess.run(['bash', str(root / 'packaging/linux/install-desktop.sh'), str(appimage)],
                           env=env, check=True, capture_output=True, text=True)
            desktop = (data / 'applications/com.ryanchen01.ImageCXX.desktop').read_text()
            self.assertIn(f'Exec="{appimage}" -- %F\n', desktop)
            self.assertIn('image/x-panasonic-rw;', desktop)
            self.assertIn('application/x-imagecxx-raw;', desktop)
            self.assertTrue((data / 'icons/hicolor/scalable/apps/com.ryanchen01.ImageCXX.svg').is_file())
            globs = (data / 'mime/globs').read_text()
            for pattern in ('*.nii', '*.nii.gz', '*.nrrd', '*.nhdr', '*.mha', '*.mhd', '*.raw', '*.bin'):
                self.assertIn(':' + pattern + '\n', globs)
            self.assertEqual((data / 'mime/packages/com.ryanchen01.ImageCXX.xml').read_bytes(),
                             (root / 'packaging/linux/com.ryanchen01.ImageCXX.xml').read_bytes())
            # Re-running registration should keep the same desktop entry.
            subprocess.run(['bash', str(root / 'packaging/linux/install-desktop.sh'), str(appimage)],
                           env=env, check=True, capture_output=True, text=True)
            self.assertEqual((data / 'applications/com.ryanchen01.ImageCXX.desktop').read_text(), desktop)


if __name__ == '__main__':
    unittest.main()
