import pathlib
import platform
import socket
import tempfile
import threading
import unittest
from unittest.mock import Mock

import plan9


def descriptor(label='Projects', auto=True, ro=False, port=50001):
    return {'Name': label, 'Port': port, 'ReadOnly': ro, 'AutoMount': auto}


class ProtocolTests(unittest.TestCase):
    def test_names_and_ports(self):
        self.assertEqual(plan9.share(descriptor('项目资料'))['Name'], '项目资料')
        for label in ('', '..', 'a/b', 'a\\b', 'a,b', 'a:b', ' x', 'x\n'):
            with self.assertRaises(ValueError):
                plan9.share(descriptor(label))
        for port in (0, -1, 0xffffffff, 27453, 27456, True):
            with self.assertRaises(ValueError):
                plan9.share(descriptor(port=port))

    def test_manifest(self):
        request = {'Version': 1, 'ResourceType': 'Plan9Shares', 'RequestType': 'Update',
                   'Settings': {'Shares': [descriptor()]}}
        self.assertEqual(plan9.manifest(request), {'Projects': descriptor()})
        request['Settings']['Shares'].append(descriptor('projects'))
        with self.assertRaises(ValueError):
            plan9.manifest(request)

    def test_protocol(self):
        manager = Mock()
        manager.status.return_value = {'Shares': []}
        server, client = socket.socketpair()
        with server, client:
            client.settimeout(3)
            worker = threading.Thread(target=plan9.handle, args=(server, manager))
            worker.start()
            plan9.send(client, {'Version': 1, 'ResourceType': 'Plan9Shares',
                                'RequestType': 'Query', 'Settings': {}})
            self.assertEqual(plan9.receive(client), {'Version': 1, 'Success': True, 'Data': {'Shares': []}})
            worker.join(3)
            self.assertFalse(worker.is_alive())
            manager.update.assert_not_called()


@unittest.skipUnless(platform.system() == 'Linux', 'Requires Linux state locking')
class MountTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.rows = {}
        self.calls = []
        self.nextid = 1

        def mount(item, target):
            self.calls.append(('mount', item['Name'], target))
            self.rows[target] = [{'Id': self.nextid, 'Type': '9p', 'Source': plan9.source(item),
                                  'ReadOnly': item['ReadOnly']}]
            self.nextid += 1

        def unmount(target):
            self.calls.append(('unmount', target))
            del self.rows[target]

        self.manager = plan9.Manager(self.root / 'state', self.root / 'media',
                                     lambda: self.rows, mount, unmount, lambda target: False)

    def test_auto_and_manual(self):
        items = {'Projects': descriptor(), 'Manual': descriptor('Manual', False, True)}
        self.manager.update(items)
        self.manager.reconcile()
        target = str(self.root / 'media' / 'Projects')
        self.assertIn(target, self.rows)
        self.assertEqual(len(self.calls), 1)
        self.manager.reconcile()
        self.assertEqual(len(self.calls), 1)
        self.manager.manual_unmount('Projects')
        self.manager.reconcile()
        self.assertNotIn(target, self.rows)
        self.manager.update(items)
        self.manager.reconcile()
        self.assertNotIn(target, self.rows)
        custom = str(self.root / 'custom')
        self.manager.manual_mount('Projects', custom)
        self.manager.manual_mount('Manual', readonly=True)
        self.assertIn(custom, self.rows)
        self.assertTrue(self.rows[str(self.root / 'media' / 'Manual')][0]['ReadOnly'])
        self.manager.update({})
        self.manager.reconcile()
        self.assertEqual(self.rows, {})

    def test_bounded_mount_retry(self):
        self.manager.update({'Projects': descriptor(), 'Second': descriptor('Second')})
        original = self.manager.mount

        def fail_first(item, target):
            if item['Name'] == 'Projects':
                raise OSError('server unavailable')
            original(item, target)

        self.manager.mount = fail_first
        self.manager.reconcile(max_mounts=1)
        self.assertIn('server unavailable', self.manager.status()['Shares'][0]['Error'])
        self.manager.reconcile(max_mounts=1)
        self.assertIn(str(self.root / 'media' / 'Second'), self.rows)
        self.manager.mount = original
        self.manager.retry_at.clear()
        self.manager.reconcile(max_mounts=1)
        self.assertIn(str(self.root / 'media' / 'Projects'), self.rows)

    def test_prepare_remove(self):
        self.manager.update({'Projects': descriptor()})
        self.manager.reconcile()
        target = str(self.root / 'media' / 'Projects')
        old_unmount = self.manager.unmount
        self.manager.unmount = Mock(side_effect=OSError('busy'))
        with self.assertRaises(OSError):
            self.manager.prepare_remove('Projects', 50001)
        self.assertIn(target, self.rows)
        self.assertTrue(self.manager.status()['Shares'][0]['Available'])
        self.manager.unmount = old_unmount
        with self.assertRaises(ValueError):
            self.manager.prepare_remove('Projects', 50002)
        self.manager.prepare_remove('Projects', 50001)
        self.assertEqual(self.rows, {})
        self.assertEqual(self.manager.status()['Shares'], [])
        # If the host operation fails, re-publishing restores the old mount.
        self.manager.update({'Projects': descriptor()})
        self.manager.reconcile()
        self.assertIn(target, self.rows)

    def test_foreign_mount_and_busy(self):
        self.manager.update({'Projects': descriptor()})
        target = str(self.root / 'media' / 'Projects')
        self.rows[target] = [{'Id': 10, 'Type': 'tmpfs', 'Source': 'tmpfs', 'ReadOnly': False}]
        self.manager.reconcile()
        self.assertIn('occupied', self.manager.status()['Shares'][0]['Error'])
        self.assertFalse(self.calls)
        del self.rows[target]
        self.manager.reconcile()
        self.manager.unmount = Mock(side_effect=OSError('busy'))
        self.manager.update({})
        self.manager.reconcile()
        self.assertIn('busy', self.manager.status()['Shares'][0]['Error'])
        self.assertIn(target, self.rows)

    def test_read_only_override_and_restart(self):
        self.manager.update({'Projects': descriptor(auto=False)})
        self.manager.manual_mount('Projects', readonly=True)
        self.manager.reconcile()
        self.assertTrue(self.manager.status()['Shares'][0]['Mounted'])
        self.manager.cleanup()
        self.assertEqual(self.rows, {})

    def test_mountinfo_escape(self):
        self.assertEqual(plan9.unescape_mountinfo(r'/media/Shared\040Files'), '/media/Shared Files')


if __name__ == '__main__':
    unittest.main()
