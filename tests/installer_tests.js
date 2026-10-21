const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('packaging/windows/controller.qs', 'utf8');

function controller(options = {}) {
    const calls = [];
    const context = vm.createContext({
        installer: {
            isInstaller: () => !options.maintenance,
            value: (key, fallback) => ({ TempDir: 'C:/Temp', ProductVersion: '0.3.0',
                ImageCXXInstallDir: options.supplied }[key] ?? fallback),
            environmentVariable: key => ({ TEMP: 'C:/Temp', LOCALAPPDATA: 'C:/Users/Test User/AppData/Local', SystemRoot: 'C:/Windows' }[key]),
            setValue: (key, value) => calls.push(['value', key, value]),
            performOperation: (name, args) => { calls.push([name, ...args]); return !options.extractFailure; },
            setTemporaryRepositories: (urls, replace) => calls.push(['repository', ...urls, replace]),
            execute: (program, args) => {
                calls.push(['execute', program, ...args]);
                if (program.endsWith('reg.exe')) return options.existing
                    ? ['InstallDir    REG_SZ    C:\\Users\\Test User\\ImageCXX\r\n', 0] : ['', 1];
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
            installationFinished: { connect: () => {} },
            setCanceled: () => calls.push(['cancel']),
        },
        gui: { reject: () => calls.push(['reject']) },
        QMessageBox: { warning: () => calls.push(['warning']) },
    });
    vm.runInContext(source, context);
    return { instance: vm.runInContext('new Controller()', context), calls };
}

const fresh = controller();
fresh.instance.IntroductionPageCallback();
assert(fresh.calls.some(call => call[0] === 'repository' && call[1].startsWith('file:///C:/Temp/ImageCXX-0.3.0-')));
assert(!fresh.calls.some(call => call[0] === 'cancel'));

const existing = controller({ existing: true });
existing.instance.IntroductionPageCallback();
const handoff = existing.calls.find(call => call[0] === 'execute' && call[1].endsWith('MaintenanceTool.exe'));
assert.equal(handoff[1], 'C:/Users/Test User/ImageCXX/ImageCXXMaintenanceTool.exe');
assert.equal(handoff[2], '--start-updater');
assert.equal(handoff[3], '--set-temp-repository');
assert(existing.calls.some(call => call[0] === 'reject'));
assert(!existing.calls.some(call => JSON.stringify(call).includes('uninstall')));
assert(existing.calls.filter(call => ['Delete', 'Rmdir'].includes(call[0])).every(call => call[1].startsWith('C:/Temp/ImageCXX-')));

const failed = controller({ existing: true, updateFailure: true });
failed.instance.IntroductionPageCallback();
assert(failed.calls.some(call => call[0] === 'warning'));
assert.throws(() => controller({ existing: true, broken: true }), /cannot be updated/);
assert.throws(() => controller({ existing: true, wrongComponent: true }), /cannot be updated/);
assert.throws(() => controller({ extractFailure: true }), /Cannot extract/);
assert.throws(() => controller({ existing: true, supplied: 'C:/Other' }), /does not match/);
const maintenance = controller({ maintenance: true });
maintenance.instance.IntroductionPageCallback();
assert.equal(maintenance.calls.length, 0);
console.log('Installer routing, upgrade failure, and fresh-install checks passed.');
