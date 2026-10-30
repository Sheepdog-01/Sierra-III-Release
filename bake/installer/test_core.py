"""The installer's steps on a stand-in Memory Stick (a temporary folder):
    cd bake && python -m unittest installer.test_core"""
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from installer import core

RELEASE = {
    'PSP/GAME/halo/EBOOT.PBP': b'\0PBP' + b'x' * 64,
    'PSP/GAME/halo/halo_crash.prx': b'prx',
    'halo/presets.txt': b'# presets\n',
    'halo/save/u/UDATA/1/blam.sav': b'the release profile',
}


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.stick = self.root / 'stick'
        write(self.stick / 'MEMSTICK.IND', b'')
        write(self.stick / 'SEPLUGINS' / 'game.txt', b'ark plugins')
        write(self.stick / 'PSP' / 'GAME' / 'OTHER' / 'EBOOT.PBP', b'another game')
        self.baked = self.root / 'COPY-TO-MEMORY-STICK'
        for name, data in RELEASE.items():
            write(self.baked / name, data)
        write(self.baked / 'halo' / 'maps' / 'a10.map', b'baked map')
        write(self.baked / 'halo' / 'maps' / 'ui.map', b'baked menus')
        write(self.baked / 'halo' / 'chant.adp', b'chant')
        write(self.baked / 'halo' / 'bake.txt', b'format: 3\n')
        write(self.baked / 'bake-log.txt', b'the log, never copied')
        self.removable = core.removable
        core.removable = lambda root: True

    def tearDown(self):
        core.removable = self.removable
        self.temp.cleanup()

    def zip(self, entries, name='halo_psp_b148.zip'):
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as z:
            for entry, data in entries.items():
                z.writestr(entry, data)
        return str(path)

    def test_release_zip(self):
        self.assertEqual(core.check_zip(self.zip(RELEASE)), 'halo')
        mp = {name.replace('/halo/', '/halomp/'): data for name, data in RELEASE.items()}
        self.assertEqual(core.check_zip(self.zip(mp, 'mp.zip')), 'halomp')
        for bad in ({'PSP/GAME/halo/x.prx': b''},                              # no EBOOT
                    dict(RELEASE, **{'SEPLUGINS/game.txt': b''}),             # outside the game's folders
                    dict(RELEASE, **{'halo/../../evil.txt': b''}),            # climbs out
                    dict(RELEASE, **{'halo/maps/a10.map': b''}),              # game data
                    dict(RELEASE, **{'halo/CON.txt': b''}),                   # a device name
                    dict(RELEASE, **{'halo/Presets.TXT': b''}),               # one file twice, as FAT reads it
                    # a zip whose "game" is another folder of the stick
                    {'PSP/GAME/SEPLUGINS/EBOOT.PBP': b'x', 'SEPLUGINS/game.txt': b'plugin'},
                    {'PSP/GAME/PSP/EBOOT.PBP': b'x', 'PSP/SAVEDATA/ULUS10000/DATA.BIN': b'save'},
                    {'PSP/GAME/ARK_Loader/EBOOT.PBP': b'x'},
                    {'PSP/GAME/HALO/EBOOT.PBP': b'x'}):
            with self.assertRaises(core.InstallError):
                core.check_zip(self.zip(bad, 'bad.zip'))

    def test_disc(self):
        with self.assertRaises(core.InstallError):
            core.check_disc(str(self.root / 'none.iso'))
        write(self.root / 'small.iso', b'x' * 1000)
        with self.assertRaises(core.InstallError):
            core.check_disc(str(self.root / 'small.iso'))
        write(self.root / 'disc' / 'default.xbe', b'xbe')
        self.assertEqual(core.check_disc(str(self.root / 'disc')), 'folder')

    def test_bake_command_and_output_folder(self):
        command = core.bake_command('-disc.iso', 'halo.zip', 'out')
        # whole paths, so a name starting with - is never read as an option
        self.assertEqual(command[1:], [str(core.LAUNCHER), os.path.abspath('-disc.iso'), os.path.abspath('out'),
                                       '--guided', '--zip', os.path.abspath('halo.zip')])
        out = self.root / 'out'
        core.clear_out(out)
        write(out / 'halo' / 'old.txt', b'old')
        core.clear_out(out)      # ours: made afresh
        self.assertEqual(sorted(p.name for p in out.iterdir()), [core.OUT_MARK])
        foreign = self.root / 'mine'
        write(foreign / 'keep.txt', b'not the installer\'s')
        with self.assertRaises(core.InstallError):
            core.clear_out(foreign)
        self.assertTrue((foreign / 'keep.txt').exists())

    def test_install(self):
        write(self.stick / 'halo' / 'save' / 'u' / 'UDATA' / '1' / 'blam.sav', b'my own progress')
        write(self.stick / 'halo' / 'maps' / 'old.map', b'an older pack')
        write(self.stick / 'halo' / 'bake.txt', b'format: 2\n')
        write(self.stick / 'PSP' / 'GAME' / 'halo' / 'mine.txt', b'kept: a merge')
        report = core.install(str(self.baked), str(self.stick))
        self.assertEqual(report['game'], 'halo')
        stick = self.stick
        self.assertEqual((stick / 'PSP' / 'GAME' / 'halo' / 'EBOOT.PBP').read_bytes(), RELEASE['PSP/GAME/halo/EBOOT.PBP'])
        self.assertEqual((stick / 'PSP' / 'GAME' / 'halo' / 'mine.txt').read_bytes(), b'kept: a merge')
        self.assertEqual((stick / 'halo' / 'save' / 'u' / 'UDATA' / '1' / 'blam.sav').read_bytes(), b'my own progress')
        self.assertEqual(sorted(p.name for p in (stick / 'halo' / 'maps').iterdir()), ['a10.map', 'ui.map'])
        self.assertEqual((stick / 'halo' / 'bake.txt').read_bytes(), b'format: 3\n')
        self.assertEqual((stick / 'SEPLUGINS' / 'game.txt').read_bytes(), b'ark plugins')
        self.assertEqual((stick / 'PSP' / 'GAME' / 'OTHER' / 'EBOOT.PBP').read_bytes(), b'another game')
        self.assertFalse((stick / core.STAGING).exists())
        self.assertFalse((stick / 'bake-log.txt').exists())

    def test_first_install_takes_the_release_save(self):
        core.install(str(self.baked), str(self.stick))
        self.assertEqual((self.stick / 'halo' / 'save' / 'u' / 'UDATA' / '1' / 'blam.sav').read_bytes(),
                         b'the release profile')

    def test_stopped_install_changes_nothing(self):
        write(self.stick / 'halo' / 'maps' / 'old.map', b'an older pack')
        ctx = core.Context()
        ctx.stage = lambda number, fraction=None: ctx.cancel() if number == 2 else None
        with self.assertRaises(core.Cancelled):
            core.install(str(self.baked), str(self.stick), ctx)
        self.assertEqual(sorted(p.name for p in (self.stick / 'halo' / 'maps').iterdir()), ['old.map'])
        self.assertFalse((self.stick / 'PSP' / 'GAME' / 'halo').exists())
        # the next install clears its own staging folder and finishes
        core.install(str(self.baked), str(self.stick))
        self.assertFalse((self.stick / core.STAGING).exists())

    def test_only_the_two_games_folders(self):
        other = self.root / 'other'
        write(other / 'PSP' / 'GAME' / 'SEPLUGINS' / 'EBOOT.PBP', b'x')
        write(other / 'SEPLUGINS' / 'game.txt', b'a plugin')
        write(other / 'halo' / 'maps' / 'ui.map', b'menus')
        write(other / 'halo' / 'bake.txt', b'format: 3\n')
        with self.assertRaises(core.InstallError):
            core.install(str(other), str(self.stick))
        self.assertEqual((self.stick / 'SEPLUGINS' / 'game.txt').read_bytes(), b'ark plugins')
        with self.assertRaises(core.InstallError):
            core.data_folders('SEPLUGINS')

    def test_refusals(self):
        write(self.stick / core.STAGING / 'theirs.txt', b'not ours')
        with self.assertRaises(core.InstallError):
            core.install(str(self.baked), str(self.stick))
        not_stick = self.root / 'usb'
        not_stick.mkdir()
        with self.assertRaises(core.InstallError):
            core.check_stick(str(not_stick))
        os.remove(self.baked / 'halo' / 'bake.txt')
        with self.assertRaises(core.InstallError):
            core.check_baked(self.baked)

    def test_bad_names_in_the_bake_output(self):
        write(self.baked / 'halo' / 'AUX.txt', b'a device name')
        with self.assertRaises(core.InstallError):
            core.install(str(self.baked), str(self.stick))
        self.assertFalse((self.stick / 'PSP' / 'GAME' / 'halo').exists())

    def link(self, link, target):
        """a junction on Windows (no special rights needed), else a symbolic link"""
        if sys.platform == 'win32':
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        else:
            os.symlink(target, link)

    def test_a_link_on_the_stick_is_never_followed(self):
        outside = self.root / 'outside'
        write(outside / 'maps' / 'precious.dat', b'not on the stick')
        self.link(self.stick / 'halo', outside)
        with self.assertRaises(core.InstallError):
            core.install(str(self.baked), str(self.stick))
        self.assertEqual((outside / 'maps' / 'precious.dat').read_bytes(), b'not on the stick')
        self.assertEqual(sorted(p.name for p in outside.iterdir()), ['maps'])
        self.assertFalse((self.stick / 'PSP' / 'GAME' / 'halo').exists())
        self.assertFalse((self.stick / core.STAGING).exists())

    def test_a_link_in_the_bake_output_is_never_followed(self):
        outside = self.root / 'outside'
        write(outside / 'secret.txt', b'not the bake\'s')
        self.link(self.baked / 'halo' / 'extra', outside)
        with self.assertRaises(core.InstallError):
            core.install(str(self.baked), str(self.stick))
        self.assertFalse((self.stick / 'halo').exists())

    def test_no_maps_never_removes_the_old_ones(self):
        write(self.stick / 'halo' / 'maps' / 'old.map', b'an older pack')
        for path in (self.baked / 'halo' / 'maps').iterdir():
            path.unlink()
        with self.assertRaises(core.InstallError):
            core.install(str(self.baked), str(self.stick))
        self.assertEqual(sorted(p.name for p in (self.stick / 'halo' / 'maps').iterdir()), ['old.map'])

    def test_a_failed_copy_leaves_no_staging(self):
        ctx = core.Context()

        def fail(number, fraction=None):
            if number == 2:
                raise OSError('the stick went away')
        ctx.stage = fail
        with self.assertRaises(OSError):
            core.install(str(self.baked), str(self.stick), ctx)
        self.assertFalse((self.stick / core.STAGING).exists())
        self.assertFalse((self.stick / 'halo').exists())

    def test_multiplayer_build(self):
        mp = {name.replace('PSP/GAME/halo/', 'PSP/GAME/halomp/').replace('halo/save/', 'halomp/save/'): data
              for name, data in RELEASE.items()}
        self.assertEqual(core.check_zip(self.zip(mp, 'mp.zip')), 'halomp')
        baked = self.root / 'mp'
        for name, data in mp.items():
            write(baked / name, data)
        write(baked / 'halo' / 'maps' / 'ui.map', b'baked menus')
        write(baked / 'halo' / 'bake.txt', b'format: 3\n')
        core.install(str(baked), str(self.stick))
        self.assertTrue((self.stick / 'PSP' / 'GAME' / 'halomp' / 'EBOOT.PBP').is_file())
        self.assertTrue((self.stick / 'halomp' / 'save' / 'u' / 'UDATA' / '1' / 'blam.sav').is_file())
        self.assertTrue((self.stick / 'halo' / 'maps' / 'ui.map').is_file())


if __name__ == '__main__':
    unittest.main()
