#!/usr/bin/env python3
"""Link the fixed 24H32 native privacy helper and emit its page-patch table."""
import argparse
import json
import pathlib
import struct
import subprocess
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE = 0x100000000
TEXT_START = BASE + 0x9da370
TEXT_END = BASE + 0x9db000
RUNTIME = BASE + 0xa6aef8
NATIVE_UUID = '9D4727A2-1FC2-3777-BE26-7BAD0ED65EF0'
SOURCES = ROOT / 'bluetooth/BlueToolFixup'

# The bytes after each patch remain untouched and qualify its native context.
HOOKS = [
    ('resolve', 0x1f1b01, 'e8ed3e0000', '85c07515807dd701750f8b45', 'intelPrivacyResolve'),
    ('admission', 0x642526, 'e81d900400', '488d45b84c8b68f84939c50f', 'intelPrivacyBuildFilterSet'),
    ('reset_start', 0x143985, 'e88a2e0000', '5b415e5dc34c89f7e8aa3860', 'intelPrivacyReset'),
    ('reset_recovery', 0x143fd5, 'e83a280000', 'c6056b6292000085c00f8492', 'intelPrivacyReset'),
    ('reset_request', 0x4a2156, 'e8b946caff', '488d7defe8f8cfddff498b1e', 'intelPrivacyReset'),
    ('sleep', 0x69bc30, '554889e54156', '534883ec304889fb488d0543', 'intelPrivacySleep'),
    ('wake', 0x69c024, '554889e54156', '534883ec504889fb488b05cf', 'intelPrivacyWake'),
    ('start', 0x69c400, '554889e54157', '415641554154534883ec4848', 'intelPrivacyStart'),
    ('stop', 0x69d94e, '554889e54157', '4156534883ec384889fb4c8d', 'intelPrivacyStop'),
    ('remove', 0x1f61f1, '0fb70dc89f8600', 'b8780000004885c90f84fd00', 'intelPrivacyRemove'),
    ('clear', 0x1f6304, '488b3dad9e8600', '4885ff740a554889e5e8a50c', 'intelPrivacyClear'),
    ('retry', 0x650a88, 'e8a3a6ffff', 'e9cb29000048833d2e0f3f00', 'intelPrivacyRetry'),
]
NATIVE = {
    'intelPrivacyNativeSleepContinue': BASE + 0x69bc36,
    'intelPrivacyNativeWakeContinue': BASE + 0x69c02a,
    'intelPrivacyNativeStartContinue': BASE + 0x69c406,
    'intelPrivacyNativeStopContinue': BASE + 0x69d954,
    'intelPrivacyNativeRemoveContinue': BASE + 0x1f61f8,
    'intelPrivacyNativeClearContinue': BASE + 0x1f630b,
    'intelPrivacyNativeIRKCount': BASE + 0xa601c0,
    'intelPrivacyNativeIRKTable': BASE + 0xa601b8,
    'intelPrivacyNativeReset': BASE + 0x146814,
    'intelPrivacyNativeRetry': BASE + 0x64b130,
}
EXTRA_GUARDS = [(0x650a7c, 'e8f3c3ffff488bbd30fdffff')]


class MachO:
    def __init__(self, path):
        self.path = pathlib.Path(path)
        self.data = self.path.read_bytes()
        header = struct.unpack_from('<8I', self.data)
        if header[0] != 0xfeedfacf or header[1] != 0x1000007:
            raise ValueError('Expected a thin x86_64 Mach-O')
        self.filetype = header[3]
        self.sections, self.segments, self.symbols = [], [], {}
        self.uuid = None
        self.commands = []
        symtab = None
        offset = 32
        for _ in range(header[4]):
            command, size = struct.unpack_from('<II', self.data, offset)
            if size < 8 or offset + size > 32 + header[5]:
                raise ValueError('Invalid Mach-O load-command bounds')
            self.commands.append(command)
            if command == 0x19:
                name = self.data[offset+8:offset+24].split(b'\0')[0].decode()
                va, vs, fo, fs = struct.unpack_from('<QQQQ', self.data, offset+24)
                self.segments.append(dict(name=name, address=va, size=vs,
                                          offset=fo, file_size=fs))
                count = struct.unpack_from('<I', self.data, offset+64)[0]
                for i in range(count):
                    s = offset + 72 + i * 80
                    sn = self.data[s:s+16].split(b'\0')[0].decode()
                    address, length, fileoff, alignment, reloff, nreloc, flags = \
                        struct.unpack_from('<QQIIIII', self.data, s+32)
                    self.sections.append(dict(segment=name, name=sn, address=address,
                                              size=length, offset=fileoff, flags=flags,
                                              nreloc=nreloc, alignment=alignment))
            elif command == 0x1b:
                self.uuid = str(uuid.UUID(bytes=self.data[offset+8:offset+24])).upper()
            elif command == 2:
                symtab = struct.unpack_from('<4I', self.data, offset+8)
            offset += size
        self.undefined = []
        if symtab:
            so, count, strings, stringsize = symtab
            for i in range(count):
                name, kind, section, desc, value = struct.unpack_from('<IBBHQ', self.data, so+i*16)
                end = self.data.find(b'\0', strings+name, strings+stringsize)
                label = self.data[strings+name:end].decode()
                if (kind & 0x0e) == 0 and not kind & 0xe0:
                    self.undefined.append(label)
                else:
                    self.symbols[label] = value

    def read(self, address, size):
        for segment in self.segments:
            relative = address - segment['address']
            if 0 <= relative and relative+size <= segment['file_size']:
                start = segment['offset'] + relative
                return self.data[start:start+size]
        raise ValueError('Address is not file-backed: %#x' % address)


def run(command):
    subprocess.run([str(x) for x in command], check=True)


def replacement(offset, original, target):
    opcode = 0xe8 if original[0] == 0xe8 else 0xe9
    delta = target - (BASE + offset + 5)
    return bytes([opcode]) + struct.pack('<i', delta) + b'\x90' * (len(original)-5)


def config_assembly(symbols=None):
    symbols = symbols or {}
    config = symbols.get('_intelPrivacyConfig', 0)
    controller = symbols.get('_intelPrivacyControllerConfig', 0)
    retry = symbols.get('_intelPrivacyRetryConfig', 0)
    markers = symbols.get('_intelPrivacyHookMarkers', 0)
    values = [BASE+0x1f59f3, BASE+0x68b548, BASE+0x7e5236, BASE+0x7e5284,
              BASE+0x7e51b8, symbols.get('_intelPrivacyInstalledControllerGate', 0), BASE+0x7e5266]
    parts = ['.section __TEXT,__const', '.p2align 3', '.globl _intelPrivacyConfig',
             '_intelPrivacyConfig:']
    parts += ['.quad %d' % (v-config if symbols else 0) for v in values]
    parts += ['.quad 0', '.quad 5000000', '.globl _intelPrivacyControllerConfig',
              '_intelPrivacyControllerConfig:']
    for address in [0x7e5710, 0x7e5716, 0xa4b480, 0xa4b518, 0xa4b428, 0xa4b478]:
        parts += ['.quad %d' % (BASE+address-controller if symbols else 0)]
    parts += ['.globl _intelPrivacyRetryConfig', '_intelPrivacyRetryConfig:']
    for address in [0xa44ff0,0x27e472,0x7e54ee,0x9dc0a8,0x68b608,0x67ef7c,
                    0x64df4c,0x64ddba,0x202fc0,0x0cbbb1,0x0cbc7e]:
        relative = BASE+address-retry if symbols else 0
        if not -(1 << 31) <= relative < (1 << 31):
            raise ValueError('Native retry offset exceeds its signed relative field')
        parts += ['.long %d' % relative]
    parts += ['.globl _intelPrivacyHookMarkers', '_intelPrivacyHookMarkers:']
    for role, offset, original, guard, export in HOOKS:
        original = bytes.fromhex(original)
        patch = replacement(offset, original, symbols['_'+export]) if symbols else bytes(len(original))
        parts += ['.quad %d' % (BASE+offset-markers if symbols else 0),
                  '.quad 0x%x' % int.from_bytes(patch.ljust(8,b'\0'), 'little'),
                  '.byte %d' % len(patch)]
    parts += ['.globl _intelPrivacyHookMarkerCount', '_intelPrivacyHookMarkerCount:',
              '.long %d' % len(HOOKS)]
    for name, address in NATIVE.items():
        parts += ['.globl _'+name, '.set _%s, 0x%x' % (name,address)]
    return '\n'.join(parts)+'\n'


def validate_native(path):
    native = MachO(path)
    if native.uuid != NATIVE_UUID:
        raise ValueError('Native UUID does not match the fixed 24H32 profile')
    for role, offset, original, guard, export in HOOKS:
        expected = bytes.fromhex(original+guard)
        if native.read(BASE+offset,len(expected)) != expected:
            raise ValueError('Native hook/guard bytes differ: '+role)
    for offset, expected in EXTRA_GUARDS:
        if native.read(BASE+offset, len(bytes.fromhex(expected))) != bytes.fromhex(expected):
            raise ValueError('Native retry callsite context differs')
    if native.read(TEXT_START,TEXT_END-TEXT_START) != bytes(TEXT_END-TEXT_START):
        raise ValueError('Native RX padding is not zero')
    text_sections = [s for s in native.sections if s['segment']=='__TEXT']
    data_sections = [s for s in native.sections if s['segment']=='__DATA']
    if max(s['address']+s['size'] for s in text_sections) != TEXT_START:
        raise ValueError('Native RX padding boundary differs')
    if max(s['address']+s['size'] for s in data_sections) != RUNTIME:
        raise ValueError('Native RW padding boundary differs')
    return dict(uuid=native.uuid, hooks=len(HOOKS), raw_bytes_matched=True,
                zero_rx_padding=True, section_boundaries_matched=True)


def cpp_bytes(value):
    return ', '.join('0x%02x' % b for b in value)


def validate_linked_branches(macho):
    symbols = macho.symbols
    checks = []
    for name in ('Sleep','Stop'):
        checks += [(name,3,0xe8,'SetEnabled'),(name,9,0xe9,name+'Original')]
    for name in ('Wake','Start'):
        checks += [(name,1,0xe8,name+'Original'),(name,11,0xe8,'SetEnabled')]
    checks += [('Remove',1,0xe8,'Invalidate'),('Remove',7,0xe9,'RemoveOriginal'),
               ('Clear',4,0xe8,'Invalidate'),('Clear',13,0xe9,'ClearOriginal'),
               ('Reset',6,0xe8,'Invalidate'),('Reset',19,0xe9,'NativeReset'),
               ('InstalledControllerGate',69,0xe9,'ActiveController')]
    checks += [('Retry',1,0xe8,'RefreshRetry'),('Retry',11,0xe9,'NativeRetry')]
    for name in ('Sleep','Wake','Start','Stop','Remove','Clear'):
        checks += [(name+'Original',7 if name in ('Remove','Clear') else 6,
                    0xe9,'Native'+name+'Continue')]
    verified = []
    for source,offset,opcode,destination in checks:
        address = symbols['_intelPrivacy'+source]+offset
        instruction = macho.read(address,5)
        target = address+5+struct.unpack_from('<i',instruction,1)[0]
        expected = symbols['_intelPrivacy'+destination]
        if instruction[0] != opcode or target != expected:
            raise ValueError('Linked branch differs: '+source+' -> '+destination)
        verified.append(dict(source=address,opcode=opcode,target=target,
                             destination='intelPrivacy'+destination))
    return verified


def write_header(path, image, symbols):
    lines = ['// Generated by scripts/build-intel-privacy.py; fixed 24H32 x86_64 profile.',
             '#pragma once', '#include <stddef.h>', '#include <stdint.h>',
             'namespace IntelPrivacyImage {',
             'static constexpr uint32_t ImageFileOffset = 0x9da370;',
             'static constexpr uint32_t RuntimeVmOffset = 0xa6aef8;',
             'static constexpr uint8_t ImageBytes[] = {']
    lines += ['    '+cpp_bytes(image[i:i+12])+',' for i in range(0,len(image),12)]
    lines += ['};', 'static constexpr size_t ImageSize = sizeof(ImageBytes);',
              'struct HookPatch { uint32_t fileOffset; uint8_t length; uint8_t original[16]; uint8_t replacement[16]; };',
              'static constexpr HookPatch Hooks[] = {']
    for role,offset,original,guard,export in HOOKS:
        original = bytes.fromhex(original)
        patch = replacement(offset,original,symbols['_'+export])
        lines += ['    {0x%x, %d, {%s}, {%s}}, // %s' %
                  (offset,len(original),cpp_bytes(original),cpp_bytes(patch),role)]
    lines += ['};','static constexpr size_t HookCount = sizeof(Hooks) / sizeof(Hooks[0]);',
              'struct Guard { uint32_t fileOffset; uint8_t length; uint8_t bytes[32]; };',
              'static constexpr Guard Guards[] = {']
    for role,offset,original,guard,export in HOOKS:
        guard = bytes.fromhex(guard)
        lines += ['    {0x%x, %d, {%s}}, // %s' %
                  (offset+len(bytes.fromhex(original)),len(guard),cpp_bytes(guard),role)]
    for offset,guard in EXTRA_GUARDS:
        guard = bytes.fromhex(guard)
        lines += ['    {0x%x, %d, {%s}}, // native retry context' %
                  (offset,len(guard),cpp_bytes(guard))]
    lines += ['};','static constexpr size_t GuardCount = sizeof(Guards) / sizeof(Guards[0]);','}']
    path.write_text('\n'.join(lines)+'\n')


def build(output, native_binary=None):
    output.mkdir(parents=True,exist_ok=True)
    native_result = validate_native(native_binary) if native_binary else None
    prefix = output/'IntelPrivacyPrefix.S'
    prefix.write_text('.section __TEXT,__pad,regular\n.space 0x370\n'
                      '.zerofill __DATA,__pad,_intelPrivacyDataPadding,0xef8,0\n')
    common = ['xcrun','clang++','-arch','x86_64','-std=c++17','-Oz','-ffreestanding',
              '-fno-exceptions','-fno-rtti','-fno-stack-protector','-fno-builtin',
              '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-fvisibility=hidden']
    objects = []
    for source in [prefix,SOURCES/'IntelPrivacy.cpp',SOURCES/'IntelPrivacyRetry.cpp',SOURCES/'IntelPrivacyController.cpp',
                   SOURCES/'IntelPrivacyWrappers.S']:
        target = output/(source.stem+'.o')
        command = common if source.suffix=='.cpp' else ['xcrun','clang','-arch','x86_64']
        run(command+['-c',source,'-o',target])
        objects.append(target)
    config = output/'IntelPrivacyConfig.S'
    config_object = output/'IntelPrivacyConfig.o'
    linked = output/'IntelPrivacy.macho'
    link = ['xcrun','ld','-preload','-arch','x86_64','-no_uuid',
            '-segaddr','__TEXT','0x1009da000','-segaddr','__DATA','0x100a6a000',
            '-section_order','__TEXT','__pad:__text:__const',
            '-section_order','__DATA','__pad:__common',
            '-e','_intelPrivacyResolve','-o',linked]+objects+[config_object]
    symbols = None
    for stage in range(2):
        config.write_text(config_assembly(symbols))
        run(['xcrun','clang','-arch','x86_64','-c',config,'-o',config_object])
        run(link)
        macho = MachO(linked)
        if symbols is not None and symbols != macho.symbols:
            raise ValueError('Configuration changed the first-pass symbol layout')
        symbols = macho.symbols
    if macho.filetype != 5 or macho.undefined:
        raise ValueError('Payload is not fully resolved MH_PRELOAD')
    if any(s['nreloc'] for s in macho.sections):
        raise ValueError('Payload retains section relocations')
    if any(c in macho.commands for c in (0x22,0x80000022,0x80000034)):
        raise ValueError('Payload unexpectedly needs dyld fixups')
    if symbols['_intelPrivacyRuntime'] != RUNTIME:
        raise ValueError('Runtime zero-fill address differs')
    runtime = next(s for s in macho.sections if s['segment']=='__DATA' and s['name']=='__common')
    if runtime['size'] != 24 or (runtime['flags'] & 0xff) != 1:
        raise ValueError('Runtime must remain 24 zero-fill bytes')
    end = max(s['address']+s['size'] for s in macho.sections if s['segment']=='__TEXT')
    if symbols['_intelPrivacyResolve'] < TEXT_START or end > TEXT_END:
        raise ValueError('Payload exceeds native RX padding')
    image = macho.read(TEXT_START,end-TEXT_START)
    # Verify the native-specific copied and reassembled prologues in the linked image.
    for role, offset, original, guard, export in HOOKS[5:9]:
        if macho.read(symbols['_'+export+'Original'],6) != bytes.fromhex(original):
            raise ValueError('Observer trampoline prologue differs: '+role)
    for name,prefix_bytes,target in [('Remove','0fb70d',BASE+0xa601c0),('Clear','488b3d',BASE+0xa601b8)]:
        address = symbols['_intelPrivacy'+name+'Original']
        instruction = macho.read(address,7)
        if instruction[:3] != bytes.fromhex(prefix_bytes) or address+7+struct.unpack_from('<i',instruction,3)[0] != target:
            raise ValueError('RIP-relative trampoline target differs: '+name)
    for role,offset,original,guard,export in HOOKS:
        patch = replacement(offset,bytes.fromhex(original),symbols['_'+export])
        if (offset & 0xfff)+len(patch)>4096 or (offset+len(patch)&0xfff)+len(bytes.fromhex(guard))>4096:
            raise ValueError('Hook/guard crosses a page: '+role)
    branches = validate_linked_branches(macho)
    disassembly = subprocess.check_output([
        'xcrun','llvm-objdump','-d','--start-address=%#x' % symbols['_intelPrivacyRetry'],
        '--stop-address=%#x' % symbols['_intelPrivacyConfig'],str(linked)],text=True)
    (output/'IntelPrivacy-wrappers.asm').write_text(disassembly)
    header = output/'IntelPrivacyImage.hpp'
    write_header(header,image,symbols)
    (output/'IntelPrivacy.bin').write_bytes(image)
    report = dict(profile_uuid=NATIVE_UUID, image_file_offset=TEXT_START-BASE,
                  image_size=len(image), image_limit=TEXT_END-TEXT_START,
                  runtime_vm_offset=RUNTIME-BASE, runtime_size=24,
                  hooks=len(HOOKS), guards=len(HOOKS)+len(EXTRA_GUARDS), two_pass_layout_identical=True,
                  undefined_symbols=macho.undefined, section_relocations=0,
                  dyld_fixups=False, native_validation=native_result,
                  verified_direct_branches=branches,
                  exports={k:v for k,v in symbols.items() if k.startswith('_intelPrivacy')},
                  header=str(header), no_hashes=True)
    (output/'IntelPrivacy-build.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('image_size','runtime_size','hooks','header')}))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=pathlib.Path,default=ROOT/'build/intel-privacy')
    parser.add_argument('--native-binary',type=pathlib.Path)
    args = parser.parse_args()
    build(args.output_dir,args.native_binary)


if __name__ == '__main__':
    main()
