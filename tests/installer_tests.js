const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('packaging/windows/controller.qs', 'utf8');

function controller(options = {}) {
    const calls = [];
    let finishedListener = null;
    const context = vm.createContext({
        installer: {
            isInstaller: () => !options.maintenance,
            value: (key, fallback) => ({
                TempDir: 'C:/Temp',
                ProductVersion: '0.3.0',
                ImageCXXInstallDir: options.supplied,
                'HKEY_CURRENT_USER\\Software\\ImageCXX\\InstallDir': options.existing
                    ? (options.existingValue ?? 'C:\\Users\\Test User\\图像\\ImageCXX') : '',
            }[key] ?? fallback),
            environmentVariable: key => ({ TEMP: options.temp ?? 'C:/Temp', LOCALAPPDATA: 'C:/Users/Test User/AppData/Local', SystemRoot: 'C:/Windows' }[key]),
            setValue: (key, value) => calls.push(['value', key, value]),
            performOperation: (name, args) => { calls.push([name, ...args]); return !options.extractFailure; },
            setTemporaryRepositories: (urls, replace) => calls.push(['repository', ...urls, replace]),
            execute: (program, args) => {
                calls.push(['execute', program, ...args]);
                return ['', options.updateFailure ? 1 : 0];
            },
            fileExists: () => !options.broken,
            readFile: (path, encoding) => {
                assert.equal(encoding, 'UTF-8');
                if (path.endsWith('components.xml')) return '<Packages><Package><Name>'
                    + (options.wrongComponent ? 'AnotherApp' : 'com.ryanchen01.ImageCXX')
                    + '</Name></Package></Packages>';
                return '["Updates.xml","com.ryanchen01.ImageCXX/0.3.0content.7z"]';
            },
            installationFinished: { connect: callback => { finishedListener = callback; } },
            setCanceled: () => calls.push(['cancel']),
        },
        buttons: { CancelButton: 3 },
        gui: {
            reject: () => calls.push(['reject']),
            clickButton: (button, delay) => calls.push(['clickButton', button, delay]),
        },
        QMessageBox: { warning: () => calls.push(['warning']) },
    });
    vm.runInContext(source, context);
    return {
        instance: vm.runInContext('new Controller()', context),
        calls,
        finish: () => finishedListener && finishedListener(),
    };
}

const fresh = controller({ temp: 'C:/Temp,1' });
const freshCallback = fresh.instance.IntroductionPageCallback;
freshCallback();
assert(fresh.calls.some(call => call[0] === 'repository' && call[1].startsWith('file:///C:/Temp%2C1/ImageCXX-0.3.0-')));
assert(!fresh.calls.some(call => call[0] === 'cancel'));
fresh.finish();
assert(fresh.calls.some(call => call[0] === 'Delete' && call[1].startsWith('C:/Temp,1/ImageCXX-0.3.0-')));
assert(fresh.calls.some(call => call[0] === 'Rmdir' && call[1].startsWith('C:/Temp,1/ImageCXX-0.3.0-')));
const freshCallsAfterCleanup = fresh.calls.length;
fresh.finish();
assert.equal(fresh.calls.length, freshCallsAfterCleanup);

const existing = controller({ existing: true });
const existingCallback = existing.instance.IntroductionPageCallback;
existingCallback();
const handoff = existing.calls.find(call => call[0] === 'execute' && call[1].endsWith('MaintenanceTool.exe'));
assert.equal(handoff[1], 'C:/Users/Test User/图像/ImageCXX/ImageCXXMaintenanceTool.exe');
assert.equal(handoff[2], '--start-updater');
assert.equal(handoff[3], '--set-temp-repository');
assert(!existing.calls.some(call => call[0] === 'execute' && call[1].endsWith('reg.exe')));
assert(existing.calls.some(call => call[0] === 'cancel'));
assert(existing.calls.some(call => call[0] === 'clickButton' && call[1] === 3 && call[2] === 0));
assert(!existing.calls.some(call => call[0] === 'reject'));
assert(!existing.calls.some(call => JSON.stringify(call).includes('uninstall')));
assert(existing.calls.filter(call => ['Delete', 'Rmdir'].includes(call[0])).every(call => call[1].startsWith('C:/Temp/ImageCXX-')));
const existingCallsAfterHandoff = existing.calls.length;
existingCallback();
assert.equal(existing.calls.length, existingCallsAfterHandoff);

const slashNormalized = controller({
    existing: true,
    existingValue: 'C:\\Users\\Test User\\图像\\ImageCXX\\',
    supplied: 'C:/Users/Test User/图像/ImageCXX/',
});
const normalizedCallback = slashNormalized.instance.IntroductionPageCallback;
normalizedCallback();
assert.equal(slashNormalized.calls.find(call => call[0] === 'execute' && call[1].endsWith('MaintenanceTool.exe'))[1],
    'C:/Users/Test User/图像/ImageCXX/ImageCXXMaintenanceTool.exe');

const failed = controller({ existing: true, updateFailure: true });
const failedCallback = failed.instance.IntroductionPageCallback;
failedCallback();
assert(failed.calls.some(call => call[0] === 'warning'));
assert(failed.calls.some(call => call[0] === 'cancel'));
assert(failed.calls.some(call => call[0] === 'clickButton' && call[1] === 3 && call[2] === 0));
assert.throws(() => controller({ existing: true, broken: true }), /cannot be updated/);
assert.throws(() => controller({ existing: true, wrongComponent: true }), /cannot be updated/);
assert.throws(() => controller({ extractFailure: true }), /Cannot extract/);
assert.throws(() => controller({ existing: true, supplied: 'C:/Other' }), /does not match/);
const maintenance = controller({ maintenance: true });
maintenance.instance.IntroductionPageCallback();
assert.equal(maintenance.calls.length, 0);
console.log('Installer routing, upgrade failure, and fresh-install checks passed.');

const installScriptSource = fs.readFileSync('packaging/windows/installscript.qs', 'utf8');
function runInstallScript(options = {}) {
    const operations = [];
    const context = vm.createContext({
        QInstaller: { Success: 0, Failure: 1, Canceled: 3 },
        installer: {
            isInstaller: () => options.isInstaller ?? true,
            status: options.status ?? 0,
            setValue: () => {},
            value: (key, fallback) => ({
                TargetDir: options.targetDir ?? 'C:/Users/Test User/图像/ImageCXX',
                ProductUUID: options.productUuid ?? '',
                AllUsers: options.allUsers ?? '',
            }[key] ?? fallback),
            toNativeSeparators: path => path.replaceAll('/', '\\'),
            installationFinished: { connect: callback => { context.notifyShell = callback; } },
            environmentVariable: () => 'C:/Windows',
            execute: (...args) => operations.push(['execute', ...args]),
        },
        component: {
            value: () => options.version ?? '0.3.0',
            createOperations: () => {},
            addOperation: (...args) => operations.push(args),
        },
    });
    vm.runInContext(installScriptSource, context);
    vm.runInContext('new Component().createOperations()', context);
    return { operations, notifyShell: () => context.notifyShell() };
}

// Resolve filenames by their final extension, as Windows does for .nii.gz files.
const freshInstall = runInstallScript({
    isInstaller: true,
    productUuid: '{11111111-2222-3333-4444-555555555555}',
});
const operations = freshInstall.operations;
for (const handler of ['ImageCXX.Image', 'Applications\\ImageCXX.exe']) {
    assert(operations.some(call => call[0] === 'GlobalConfig'
        && call[1] === 'HKEY_CURRENT_USER\\Software\\Classes\\' + handler + '\\shell\\open\\command'
        && call[2] === 'Default'
        && call[3] === '"C:\\Users\\Test User\\图像\\ImageCXX\\bin\\ImageCXX.exe" -- "%1"'),
        'Missing quoted native file-open command for ' + handler);
}
assert(operations.some(call => call[0] === 'GlobalConfig'
    && call[1] === 'HKEY_CURRENT_USER\\Software\\Classes\\ImageCXX.Image\\DefaultIcon'
    && call[3] === '"C:\\Users\\Test User\\图像\\ImageCXX\\bin\\ImageCXX.exe",0'));
assert(!operations.some(call => call[0] === 'GlobalConfig' && call[1].includes('\\Uninstall\\')));
console.log('Native Windows file association path checks passed.');
for (const filename of ['scan.nii.gz', 'scan.nii', 'image.png']) {
    const extension = require('node:path').win32.extname(filename);
    assert(operations.some(call => call[0] === 'GlobalConfig'
        && call[1] === 'HKEY_CURRENT_USER\\Software\\Classes\\' + extension + '\\OpenWithProgids'
        && call[2] === 'ImageCXX.Image'), 'Missing Open with handler for ' + filename);
}
assert(!operations.some(call => call[0] === 'GlobalConfig'
    && call[1] === 'HKEY_CURRENT_USER\\Software\\Classes\\.gz' && call[2] === 'Default'));
console.log('Compressed NIfTI Open with registration checks passed.');

freshInstall.notifyShell();
const freshExec = operations.find(call => call[0] === 'execute');
assert(freshExec
    && freshExec[1].endsWith('/powershell.exe') && freshExec[2].includes('-NonInteractive')
    && freshExec[2].at(-1).includes('SHChangeNotify(0x08000000')
    && freshExec[2].at(-1).includes("'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall'")
    && freshExec[2].at(-1).includes("'{11111111-2222-3333-4444-555555555555}'")
    && !freshExec[2].at(-1).includes('@'));
for (const extension of ['.gz', '.nii', '.png']) {
    assert(operations.some(call => call[0] === 'GlobalConfig'
        && call[1] === 'HKEY_CURRENT_USER\\Software\\Classes\\Applications\\ImageCXX.exe\\SupportedTypes'
        && call[2] === extension));
}

const updateInstall = runInstallScript({
    isInstaller: false,
    productUuid: '{11111111-2222-3333-4444-555555555555}',
    targetDir: "C:/Users/O'Brien/图像/ImageCXX",
    version: '0.3.6',
});
const uninstallKey = 'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{11111111-2222-3333-4444-555555555555}';
assert(updateInstall.operations.some(call => call[0] === 'GlobalConfig'
    && call[1] === uninstallKey && call[2] === 'DisplayVersion' && call[3] === '0.3.6'));
assert(updateInstall.operations.some(call => call[0] === 'GlobalConfig'
    && call[1] === uninstallKey && call[2] === 'InstallDate' && /^\d{8}$/.test(call[3])));
updateInstall.notifyShell();
const updateExec = updateInstall.operations.find(call => call[0] === 'execute');
assert(updateExec
    && updateExec[2].at(-1).includes("'C:\\Users\\O''Brien\\图像\\ImageCXX'")
    && !updateExec[2].at(-1).includes('@'));

const failedInstall = runInstallScript({
    isInstaller: false,
    status: 1,
    productUuid: '{11111111-2222-3333-4444-555555555555}',
});
failedInstall.notifyShell();
assert(!failedInstall.operations.some(call => call[0] === 'execute'));
console.log('Windows Uninstall registry update and duplicate cleanup checks passed.');
