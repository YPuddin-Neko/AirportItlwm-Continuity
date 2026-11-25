"""Exercise network-service migration against an isolated SCPreferences file."""

import copy
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('repair', Path(__file__).resolve().parents[1] /
                                             'tools/repair-network-service.py')
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


def fixture():
    services = {
        'primary': {'UserDefinedName': 'Ethernet', 'Interface': {
            'DeviceName': 'en1', 'Hardware': 'Ethernet', 'Type': 'Ethernet',
            'UserDefinedName': 'Ethernet'}, 'IPv4': {'ConfigMethod': 'DHCP'},
            'IPv6': {'ConfigMethod': 'Automatic'}, 'DNS': {'ServerAddresses': ['10.0.0.1']},
            'Proxies': {'ExceptionsList': ['*.local']}},
        'aux': {'UserDefinedName': 'Wi-Fi', 'Interface': {
            'DeviceName': 'en7', 'Hardware': 'AirPort', 'Type': 'Ethernet'}},
        'direct': {'UserDefinedName': 'Wi-Fi 2', 'Interface': {
            'DeviceName': 'awdl1', 'Hardware': 'AirPort', 'Type': 'Ethernet'}},
        'vpn': {'UserDefinedName': 'VPN', 'Interface': {'DeviceName': 'utun0', 'Type': 'VPN'}},
        'usb': {'UserDefinedName': 'USB', 'Interface': {'DeviceName': 'en2', 'Type': 'Ethernet'}},
    }
    root = {'CurrentSet': '/Sets/current', 'NetworkServices': services, 'Sets': {
        'current': {'Network': {'Service': {sid: {'__LINK__': '/NetworkServices/' + sid}
                                         for sid in services},
                               'Global': {'IPv4': {'ServiceOrder': ['aux', 'primary', 'vpn', 'usb', 'direct'],
                                                   'Unrelated': True}}}},
        'other': {'Network': {'Service': {'aux': {'__LINK__': '/NetworkServices/aux'}}}}}}
    interfaces = {'Interfaces': [
        {'BSD Name': 'en1', 'IOPathMatch': 'IOService:/PCI/AirportItlwm/en1'},
        {'BSD Name': 'en7', 'IOPathMatch': 'IOService:/PCI/AirportItlwm/AirportItlwmAWDLInterface/Legacy/en7'},
    ]}
    return root, interfaces


class NetworkServiceRepair(unittest.TestCase):
    def test_migration_preserves_primary_protocols_and_other_devices(self):
        root, interfaces = fixture()
        original = copy.deepcopy(root)
        changes, removed = repair.plan(root, interfaces)
        self.assertEqual(removed, ['aux', 'direct'])
        updated = copy.deepcopy(root)
        for change in changes:
            parent, key = change['Path'].rsplit('/', 1)
            repair.value_at(updated, parent)[key] = change['After']
        self.assertEqual(root, original)
        self.assertEqual(updated['Sets']['other'], root['Sets']['other'])
        for sid in ['aux', 'direct', 'vpn', 'usb']:
            self.assertEqual(updated['NetworkServices'][sid], root['NetworkServices'][sid])
        primary = updated['NetworkServices']['primary']
        self.assertEqual(primary['Interface']['Hardware'], 'AirPort')
        self.assertEqual(primary['Interface']['Type'], 'Ethernet')
        self.assertEqual(primary['UserDefinedName'], 'Wi-Fi')
        for protocol in ['IPv4', 'IPv6', 'DNS', 'Proxies']:
            self.assertEqual(primary[protocol], root['NetworkServices']['primary'][protocol])
        self.assertEqual(repair.value_at(updated, '/Sets/current/Network/Global/IPv4'),
                         {'ServiceOrder': ['primary', 'vpn', 'usb'], 'Unrelated': True})
        self.assertEqual(repair.plan(updated, interfaces), ([], []))

    def test_conflicting_service_is_not_overwritten(self):
        root, interfaces = fixture()
        root['NetworkServices']['usb']['UserDefinedName'] = 'Wi-Fi'
        with self.assertRaises(ValueError):
            repair.plan(root, interfaces)

    def test_other_hardware_is_not_changed(self):
        root, interfaces = fixture()
        interfaces['Interfaces'][0]['IOPathMatch'] = 'IOService:/SomeOtherDriver/en1'
        with self.assertRaises(ValueError):
            repair.plan(root, interfaces)

    def test_native_preferences_edit_and_restore_in_memory(self):
        root, interfaces = fixture()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'preferences.plist'
            path.write_bytes(plistlib.dumps(root))
            prefs = repair.Preferences(str(path))
            try:
                self.assertEqual(prefs.snapshot(), root)
                changes, _ = repair.plan(prefs.snapshot(), interfaces)
                for change in changes:
                    prefs.set(change['Path'], change['After'])
                saved = prefs.snapshot()
                for change in changes:
                    self.assertEqual(repair.value_at(saved, change['Path']), change['After'])
                repair.restore(prefs, {'Changes': changes})
                self.assertEqual(prefs.snapshot(), root)
            finally:
                prefs.close()
            self.assertEqual(plistlib.loads(path.read_bytes()), root)

    def test_restore_preserves_later_user_changes(self):
        root, interfaces = fixture()
        changes, _ = repair.plan(root, interfaces)
        class Modified:
            def snapshot(self):
                return root
            def set(self, *_):
                raise AssertionError('should not write after a mismatch')
        with self.assertRaises(ValueError):
            repair.restore(Modified(), {'Changes': changes})


if __name__ == '__main__':
    unittest.main()
