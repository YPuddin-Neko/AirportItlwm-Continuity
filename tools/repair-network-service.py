#!/usr/bin/env python3
"""Repair the 9462's saved en1 service through SystemConfiguration."""

import argparse
import copy
import ctypes as C
import datetime
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys


def value_at(root, path):
    value = root
    for part in path.strip('/').split('/'):
        value = value[part]
    return value


def plan(preferences, interfaces):
    """Keep the primary service UUID and protocols; unlink auxiliary services."""
    primary = [i for i in interfaces.get('Interfaces', []) if i.get('BSD Name') == 'en1']
    if len(primary) != 1 or 'AirportItlwm/en1' not in primary[0].get('IOPathMatch', ''):
        raise ValueError('en1 不是本机的 AirportItlwm 主接口，未修改。')
    current = preferences['CurrentSet']
    network = value_at(preferences, current + '/Network')
    members = network['Service']
    services = preferences['NetworkServices']
    primary_ids = [sid for sid in members
                   if services[sid].get('Interface', {}).get('DeviceName') == 'en1']
    if len(primary_ids) != 1:
        raise ValueError('当前网络位置中 en1 服务不是唯一一条，未修改。')
    sid = primary_ids[0]
    awdl_names = {i.get('BSD Name') for i in interfaces.get('Interfaces', [])
                  if 'AirportItlwmAWDLInterface/' in i.get('IOPathMatch', '')}
    obsolete = {key for key in members if key != sid and (
        services[key].get('Interface', {}).get('DeviceName') in awdl_names or
        services[key].get('Interface', {}).get('DeviceName', '').startswith('awdl'))}
    if any(services[key].get('UserDefinedName') == 'Wi-Fi'
           for key in members if key != sid and key not in obsolete):
        raise ValueError('当前网络位置已有另一条 Wi-Fi 服务，未修改。')
    service = copy.deepcopy(services[sid])
    if service['Interface'].get('Hardware') not in ('Ethernet', 'AirPort'):
        raise ValueError('en1 的服务类型与已诊断状态不符，未修改。')
    service['UserDefinedName'] = 'Wi-Fi'
    service['Interface']['Hardware'] = 'AirPort'
    service['Interface']['UserDefinedName'] = 'Wi-Fi'
    updated_members = {key: copy.deepcopy(v) for key, v in members.items() if key not in obsolete}
    ipv4 = copy.deepcopy(network['Global']['IPv4'])
    order = ipv4['ServiceOrder']
    if sid not in order:
        raise ValueError('en1 不在当前服务顺序中，未修改。')
    replacement_index = min(i for i, key in enumerate(order) if key == sid or key in obsolete)
    order = [key for key in order if key != sid and key not in obsolete]
    order.insert(replacement_index, sid)
    ipv4['ServiceOrder'] = order
    replacements = [(f'/NetworkServices/{sid}', service),
                    (current + '/Network/Service', updated_members),
                    (current + '/Network/Global/IPv4', ipv4)]
    changes = [{'Path': path, 'Before': copy.deepcopy(value_at(preferences, path)), 'After': after}
               for path, after in replacements if value_at(preferences, path) != after]
    return changes, sorted(obsolete)


class Preferences:
    """SCPreferences transaction. A separate file is used by offline tests."""
    def __init__(self, path=None):
        self.cf = C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.sc = C.CDLL('/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration')
        p, n = C.c_void_p, C.c_long
        declarations = [
            (self.cf, 'CFStringCreateWithCString', p, [p, C.c_char_p, C.c_uint32]),
            (self.cf, 'CFDataCreate', p, [p, C.c_void_p, n]),
            (self.cf, 'CFDataGetLength', n, [p]),
            (self.cf, 'CFDataGetBytePtr', p, [p]),
            (self.cf, 'CFPropertyListCreateWithData', p, [p, p, C.c_ulong, p, p]),
            (self.cf, 'CFPropertyListCreateData', p, [p, p, n, C.c_ulong, p]),
            (self.cf, 'CFRelease', None, [p]),
            (self.sc, 'SCPreferencesCreate', p, [p, p, p]),
            (self.sc, 'SCPreferencesCopyKeyList', p, [p]),
            (self.sc, 'SCPreferencesGetValue', p, [p, p]),
            (self.sc, 'SCPreferencesPathSetValue', C.c_bool, [p, p, p]),
            (self.sc, 'SCPreferencesLock', C.c_bool, [p, C.c_bool]),
            (self.sc, 'SCPreferencesUnlock', C.c_bool, [p]),
            (self.sc, 'SCPreferencesCommitChanges', C.c_bool, [p]),
            (self.sc, 'SCPreferencesApplyChanges', C.c_bool, [p]),
            (self.sc, 'SCError', C.c_int, []),
            (self.sc, 'SCErrorString', C.c_char_p, [C.c_int]),
        ]
        for lib, name, result, args in declarations:
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = result, args
        name, filename = self.string('AirportItlwm network repair'), self.string(path) if path else None
        self.ref = self.sc.SCPreferencesCreate(None, name, filename)
        self.cf.CFRelease(name)
        if filename:
            self.cf.CFRelease(filename)
        if not self.ref:
            self.fail('打开网络配置')
        self.locked = False

    def fail(self, action):
        raise RuntimeError(action + ': ' + self.sc.SCErrorString(self.sc.SCError()).decode())

    def string(self, text):
        return self.cf.CFStringCreateWithCString(None, str(text).encode(), 0x08000100)

    def decode(self, ref):
        if not ref:
            return None
        data = self.cf.CFPropertyListCreateData(None, ref, 200, 0, None)
        if not data:
            raise ValueError('无法读取配置属性')
        try:
            return plistlib.loads(C.string_at(self.cf.CFDataGetBytePtr(data), self.cf.CFDataGetLength(data)))
        finally:
            self.cf.CFRelease(data)

    def snapshot(self):
        keys = self.sc.SCPreferencesCopyKeyList(self.ref)
        try:
            result = {}
            for key in self.decode(keys) or []:
                key_ref = self.string(key)
                try:
                    result[key] = self.decode(self.sc.SCPreferencesGetValue(self.ref, key_ref))
                finally:
                    self.cf.CFRelease(key_ref)
            return result
        finally:
            if keys:
                self.cf.CFRelease(keys)

    def lock(self):
        if not self.sc.SCPreferencesLock(self.ref, False):
            self.fail('锁定网络配置')
        self.locked = True

    def set(self, path, value):
        raw = plistlib.dumps(value, fmt=plistlib.FMT_BINARY)
        data = self.cf.CFDataCreate(None, raw, len(raw))
        prop = self.cf.CFPropertyListCreateWithData(None, data, 0, None, None)
        key = self.string(path)
        try:
            if not prop or not self.sc.SCPreferencesPathSetValue(self.ref, key, prop):
                self.fail('写入 ' + path)
        finally:
            self.cf.CFRelease(key)
            if prop:
                self.cf.CFRelease(prop)
            self.cf.CFRelease(data)

    def commit(self, apply=True):
        if not self.sc.SCPreferencesCommitChanges(self.ref):
            self.fail('保存配置')
        if apply and not self.sc.SCPreferencesApplyChanges(self.ref):
            self.fail('配置已保存，但刷新网络服务失败；备份可用于恢复')

    def close(self):
        if self.locked:
            self.sc.SCPreferencesUnlock(self.ref)
        self.cf.CFRelease(self.ref)


def validate_driver():
    data = plistlib.loads(subprocess.check_output([
        '/usr/sbin/ioreg', '-r', '-c', 'AirportItlwmAWDLInterface', '-a', '-l']))
    def has_legacy(node):
        return node.get('IOObjectClass') == 'IOSkywalkLegacyEthernet' or any(
            has_legacy(c) for c in node.get('IORegistryEntryChildren', []))
    if not data or any(has_legacy(node) for node in data):
        raise ValueError('请先启动 Preview 28 或之后修复了重复 AWDL 网卡的驱动，再运行此工具。')


def restore(preferences, receipt):
    current = preferences.snapshot()
    for change in receipt['Changes']:
        if value_at(current, change['Path']) != change['After']:
            raise ValueError('修复后网络配置又有变化，未覆盖当前设置。')
    for change in receipt['Changes']:
        preferences.set(change['Path'], change['Before'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--apply', action='store_true')
    modes.add_argument('--restore', type=Path)
    modes.add_argument('--restore-latest', action='store_true')
    args = parser.parse_args()
    writing = args.apply or args.restore or args.restore_latest
    if writing and os.geteuid() != 0:
        raise ValueError('请双击修复或恢复工具，在 macOS 弹窗中输入管理员密码。')
    preferences = Preferences()
    try:
        if writing:
            preferences.lock()
        if args.restore_latest:
            backups = sorted((Path(__file__).resolve().parent / 'Backup').glob('*.plist'))
            if not backups:
                raise ValueError('没有找到本工具创建的备份。')
            args.restore = backups[-1]
        if args.restore:
            receipt = plistlib.loads(args.restore.read_bytes())
            restore(preferences, receipt)
            preferences.commit()
            print('已恢复本次修复前的网络服务配置。')
            return
        interfaces = plistlib.loads(Path(
            '/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist').read_bytes())
        changes, obsolete = plan(preferences.snapshot(), interfaces)
        print('en1 → Wi-Fi（AirPort）；移出当前网络位置的 AWDL 服务：' + str(len(obsolete)))
        if not args.apply:
            print(json.dumps({'changes': [c['Path'] for c in changes], 'removedServiceIDs': obsolete},
                             ensure_ascii=False, indent=2))
            print('仅检查，未修改网络配置。')
            return
        validate_driver()
        if not changes:
            print('配置已正确，无需重复修复。')
            return
        folder = Path(__file__).resolve().parent / 'Backup'
        folder.mkdir(mode=0o700, exist_ok=True)
        backup = folder / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f') + '.plist')
        receipt = {'Version': 1, 'Changes': changes}
        with os.fdopen(os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as f:
            plistlib.dump(receipt, f)
        print('备份：' + str(backup), flush=True)
        for change in changes:
            preferences.set(change['Path'], change['After'])
        preferences.commit()
        print('已修正 en1 的 Wi-Fi 服务类型和名称，并移出误建的 AWDL 服务。')
        print('请关闭后重新打开系统设置，确认 Wi-Fi 状态。')
    finally:
        preferences.close()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print('未完成：' + str(error), file=sys.stderr)
        sys.exit(1)
