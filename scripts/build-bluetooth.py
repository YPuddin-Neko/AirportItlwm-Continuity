#!/usr/bin/env python3
"""Build the pinned Sequoia Continuity Bluetooth kext companions."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import pathlib
import plistlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
RECIPES = {
    'bluetool': ('BrcmPatchRAM', '3ad0963b5008dae84d1960b32c4b391dd617fb3e',
                 'BrcmPatchRAM-3ad0963.tar.gz', 'BlueToolFixup', 'bluetool-intel-standard-hci.patch',
                 'https://codeload.github.com/acidanthera/BrcmPatchRAM/tar.gz/3ad0963b5008dae84d1960b32c4b391dd617fb3e'),
    'firmware': ('IntelBluetoothFirmware', 'a414375505fa3b014be32f8fca31b5bbfdf9f10f',
                 'IntelBluetoothFirmware-a414375.tar.gz', 'IntelBluetoothFirmware', 'intel-usb-completion.patch',
                 'https://codeload.github.com/OpenIntelWireless/IntelBluetoothFirmware/tar.gz/a414375505fa3b014be32f8fca31b5bbfdf9f10f'),
}
LILU_URL = 'https://github.com/acidanthera/Lilu/releases/download/1.7.1/Lilu-1.7.1-DEBUG.zip'


def ensure_file(dest: pathlib.Path, url: str):
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + '.part')
    urllib.request.urlretrieve(url, tmp)
    tmp.replace(dest)


def apply_recipe_patch(source, archive_path, patch_path):
    """Rebuild patched inputs from the pinned archive, including on a rebuild."""
    paths = []
    for line in patch_path.read_text().splitlines():
        if line.startswith('--- a/'):
            path = pathlib.PurePosixPath(line[6:].split('\t', 1)[0])
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('Invalid patch input: ' + str(path))
            paths.append(path)
    if not paths:
        raise ValueError('Recipe patch has no source files')
    with tempfile.TemporaryDirectory(prefix='recipe-', dir=source.parent) as directory:
        staged = pathlib.Path(directory)
        with tarfile.open(archive_path) as archive_file:
            for path in paths:
                member = archive_file.getmember(source.name + '/' + str(path))
                if not member.isfile():
                    raise ValueError('Recipe input is not a regular file: ' + str(path))
                target = staged / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive_file.extractfile(member).read())
        subprocess.run(['patch', '--fuzz=0', '-f', '-p1', '-i', str(patch_path)],
                       cwd=staged, check=True)
        for path in paths:
            shutil.copyfile(staged / path, source / path)


def project_inputs(source, project, target):
    value = json.loads(subprocess.check_output([
        'plutil', '-convert', 'json', '-o', '-',
        str(source / (project + '.xcodeproj/project.pbxproj'))]))
    objects = value['objects']
    paths = {}

    def visit(key, parent):
        entry = objects[key]
        path = (source if entry.get('sourceTree') == 'SOURCE_ROOT' else parent) / entry.get('path', '')
        paths[key] = path
        for child in entry.get('children', []):
            visit(child, path)
    visit(objects[value['rootObject']]['mainGroup'], source)
    entry = next(x for x in objects.values() if x.get('isa') == 'PBXNativeTarget' and x.get('name') == target)
    sources = []
    for phase_id in entry['buildPhases']:
        phase = objects[phase_id]
        if phase['isa'] == 'PBXSourcesBuildPhase':
            for key in phase['files']:
                reference = objects[key]
                if reference.get('settings', {}).get('COMPILER_FLAGS'):
                    raise ValueError('Unhandled per-file compiler flags: ' + key)
                sources.append(paths[reference['fileRef']])
    return sources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=RECIPES)
    parser.add_argument('--configuration', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--jobs', default=3, type=int)
    parser.add_argument('--preview', type=int)
    args = parser.parse_args()
    project, revision, archive, target, patch, url = RECIPES[args.component]
    base = ROOT / 'build/bluetooth'
    deps = base / 'deps'
    ensure_file(deps / archive, url)
    ensure_file(deps / 'Lilu-1.7.1-DEBUG.zip', LILU_URL)
    source = base / (project + '-' + revision)
    if not source.exists():
        with tarfile.open(deps / archive) as archive_file:
            archive_file.extractall(base, filter='data')
    sdk = source / 'MacKernelSDK'
    if not sdk.exists():
        sdk.symlink_to(ROOT / 'MacKernelSDK', target_is_directory=True)
    lilu = source / 'Lilu.kext'
    if not lilu.exists():
        with zipfile.ZipFile(deps / 'Lilu-1.7.1-DEBUG.zip') as archive_file:
            for member in archive_file.infolist():
                if member.filename.startswith('Lilu.kext/'):
                    archive_file.extract(member, source)
    patch_path = ROOT / 'bluetooth/patches' / patch
    apply_recipe_patch(source, deps / archive, patch_path)
    output = base / 'products' / args.component / args.configuration
    if args.component == 'bluetool':
        shutil.copyfile(ROOT / 'bluetooth/BlueToolFixup/IntelStandardHCI.hpp',
                        source / 'BrcmPatchRAM/IntelStandardHCI.hpp')
        shutil.copyfile(ROOT / 'bluetooth/BlueToolFixup/IntelAudioClock.hpp',
                        source / 'BrcmPatchRAM/IntelAudioClock.hpp')
        privacy = output / 'privacy'
        subprocess.run([sys.executable, ROOT / 'scripts/build-intel-privacy.py',
                        '--output-dir', privacy], cwd=ROOT, check=True)
        shutil.copyfile(privacy / 'IntelPrivacyImage.hpp',
                        source / 'BrcmPatchRAM/IntelPrivacyImage.hpp')
        shutil.copyfile(ROOT / 'bluetooth/BlueToolFixup/IntelPrivacyInstaller.hpp',
                        source / 'BrcmPatchRAM/IntelPrivacyInstaller.hpp')
    objects = output / 'objects'
    logs = output / 'logs'
    objects.mkdir(parents=True, exist_ok=True)
    logs.mkdir(exist_ok=True)
    sources = project_inputs(source, project, target)
    if args.component == 'firmware':
        spec = importlib.util.spec_from_file_location('bt_firmware', source / 'scripts/zlib_compress_fw.py')
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        generator.process_files(str(source / 'IntelBluetoothFirmware/FwBinary.cpp'),
                                str(source / 'IntelBluetoothFirmware/fw'))
    version = '2.7.2' if args.component == 'bluetool' else '2.5.1'
    build_version = (version + 'd' + str(args.preview)) if args.preview is not None else version
    identifier = 'as.acidanthera.BlueToolFixup' if args.component == 'bluetool' else 'com.zxystd.IntelBluetoothFirmware'
    flags = [
        '-x', 'c++', '-std=gnu++14', '-arch', 'x86_64', '-O1',
        '-g' if args.configuration == 'Debug' else '-g0',
        '-mmacosx-version-min=15.2', '-mkernel', '-nostdinc++', '-fapple-kext',
        '-fno-exceptions', '-fno-rtti', '-fno-common', '-fno-builtin',
        '-fno-stack-protector', '-mno-red-zone', '-Werror=return-type',
        '-DKERNEL', '-DKERNEL_PRIVATE', '-D__KERNEL__',
        '-DPRODUCT_NAME=' + target, '-DMODULE_VERSION=' + version,
        '-I' + str(ROOT / 'MacKernelSDK/Headers'),
        '-I' + str(source / 'Lilu.kext/Contents/Resources'),
        '-I' + str(source / 'IntelBluetoothFirmware'),
    ]
    if args.configuration == 'Debug':
        flags.append('-DDEBUG=1')
    if args.component == 'bluetool':
        flags.append('-DLILU_CUSTOM_IOKIT_INIT=1')
    module = objects / 'module.cpp'
    entry_points = ''
    start, stop = 'nullptr', 'nullptr'
    if args.component == 'bluetool':
        start, stop = target + '_kern_start', target + '_kern_stop'
        entry_points = f'extern kern_return_t {start}(kmod_info_t *, void *);\nextern kern_return_t {stop}(kmod_info_t *, void *);\n'
    module.write_text('#include <mach/kmod.h>\nextern "C" {\n' + entry_points + f'''
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
KMOD_EXPLICIT_DECL({identifier}, "{build_version}", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = {start};
__private_extern__ kmod_stop_func_t *_antimain = {stop};
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
}}
''')
    sources.append(module)
    compiler = subprocess.check_output(['xcrun', '--find', 'clang++'], text=True).strip()

    def run(command, name):
        log = logs / (name + '.log')
        with log.open('w') as stream:
            result = subprocess.run(list(map(str, command)), cwd=source, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(str(log) + '\n' + log.read_text()[-6000:])

    def compile_one(item):
        index, file = item
        obj = objects / (str(index) + '.o')
        run([compiler, *flags, '-c', file, '-o', obj], 'compile-' + str(index))
        return obj

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        compiled = list(pool.map(compile_one, enumerate(sources)))
    bundle = output / (target + '.kext')
    binary = bundle / 'Contents/MacOS' / target
    binary.parent.mkdir(parents=True, exist_ok=True)
    run(['xcrun', 'ld', '-kext', '-arch', 'x86_64', '-macos_version_min', '15.2',
         '-o', binary, *compiled, sdk / 'Library/x86_64/libkmod.a'], 'link')
    info = bundle / 'Contents/Info.plist'
    template = source / ('BrcmPatchRAM/BlueToolFixup-Info.plist' if args.component == 'bluetool' else 'IntelBluetoothFirmware/Info.plist')
    text = template.read_text()
    for key, value in {'PRODUCT_NAME': target, 'PRODUCT_NAME:rfc1034identifier': target,
                       'DEVELOPMENT_LANGUAGE': 'en', 'EXECUTABLE_NAME': target,
                       'PRODUCT_BUNDLE_IDENTIFIER': identifier, 'MODULE_VERSION': build_version,
                       'CURRENT_PROJECT_VERSION': build_version, 'MACOSX_DEPLOYMENT_TARGET': '15.2'}.items():
        text = text.replace('$(' + key + ')', value).replace('${' + key + '}', value)
    if '$(' in text or '${' in text:
        raise ValueError('Unexpanded Info.plist build setting')
    properties = plistlib.loads(text.encode())
    properties.update(CFBundleExecutable=target, CFBundleIdentifier=identifier,
                      CFBundleVersion=build_version, CFBundleShortVersionString=version)
    properties['IntelSequoiaCompatibilityBuild'] = ('9462-24H32-6' if args.component == 'bluetool' else '9462-24H32-1')
    info.write_bytes(plistlib.dumps(properties, sort_keys=False))
    print(bundle)
    print('Build logs:', logs)


if __name__ == '__main__':
    main()
