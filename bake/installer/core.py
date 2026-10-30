"""The installer's steps, without the window: check the player's two files,
run the guided bake on them (guided.py, unchanged), and copy what it made to
a PSP Memory Stick.

What it holds to, each checked here rather than trusted to the window:
- only a removable FAT or exFAT drive that looks like a PSP Memory Stick
  (MEMSTICK.IND or a PSP folder) is written;
- it writes only PSP/GAME/<the game's folder>/, halo/ (and the multiplayer
  build's halomp/), merged with what is there, as the bake's README says to
  copy by hand; ARK-4's files, SEPLUGINS, other games and anything else on
  the stick are never touched, and a link anywhere on the way to a target is
  refused rather than followed;
- every file goes to SIERRA-III-STAGING/ on the stick first and is read
  back before anything is moved into place, so an interrupted copy never
  leaves a half-written file where the game reads it;
- a player's saves are never replaced; the old halo/maps is removed only once
  the new maps are checked; the old halo/bake.txt, the stamp the game reads,
  comes out first and the new one goes in last, so a copy interrupted while
  files move into place leaves maps the game refuses until it is run again.
"""
import hashlib
import os
import re
import shutil
import stat
import string
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]      # the bake bundle (or bake/ in a checkout)
LAUNCHER = HERE / 'launcher.py'
# the installer's own output folder in the bundle: short, as Windows counts
# every character of the paths the bake writes inside it (guided.py)
OUT = HERE / 'out'
OUT_MARK = 'made-by-the-installer.txt'
# the guided bake's own lock in the bundle's folder, held while the installer
# bakes: a "Bake Halo" or a second installer then waits its turn, and none
# of them sets up .venv under another
LOCK = HERE / '.bake-lock'
STAGING = 'SIERRA-III-STAGING'
STAGING_MARK = 'unfinished-install.txt'
# where a game can come from: the Xbox disc today; the PC game is shown in
# the window, switched off, until its support lands
SOURCES = {
    'xbox': {'title': 'Halo: Combat Evolved for the original Xbox',
             'detail': 'Your own disc image, NTSC (USA) 01.10.12.2276, or a folder of its files.', 'ready': True},
    'pc': {'title': 'Halo: Combat Evolved for PC', 'detail': 'Not available in this version.', 'ready': False},
}
# the two games a release holds, and the stick's top folders each one's
# files go in: halo/ (the maps, and the campaign's data), and the
# multiplayer build's own halomp/ (psp/include/psp_paths.h). Nothing else
# is ever a game's folder, whatever a zip names
GAMES = {'halo': ['halo'], 'halomp': ['halo', 'halomp']}
GAME_PATH = re.compile(r'^PSP/GAME/(halo|halomp)/EBOOT\.PBP$')
# a name the Memory Stick's FAT file system stores as it is
NAME_PART = re.compile(r"^[A-Za-z0-9 _.()+,&'\-]{1,64}$")
RESERVED = {'con', 'prn', 'aux', 'nul', *('com%d' % n for n in range(10)), *('lpt%d' % n for n in range(10))}
GAME_DATA = ('.map', '.iso', '.xbe')
FILE_SYSTEMS = {'fat', 'fat12', 'fat16', 'fat32', 'exfat', 'vfat', 'msdos'}
STAGE = re.compile(r'^\[(\d)/4\]')
MAPS_DONE = re.compile(r'(\d+) of (\d+) done')


class InstallError(Exception):
    """what is wrong, in a sentence for the player"""


class Cancelled(InstallError):
    pass


class Context:
    """progress and cancelling for one step; say() and stage() go to the window"""

    def __init__(self, emit=None):
        self.emit = emit or (lambda kind, **data: None)
        self.cancelled = False
        self.process = None

    def say(self, text):
        self.emit('log', text=text)

    def stage(self, number, fraction=None):
        self.emit('stage', number=number, fraction=fraction)

    def cancel(self):
        self.cancelled = True
        stop(self.process)

    def check(self):
        if self.cancelled:
            raise Cancelled('Stopped. Nothing was half-copied; start the step again when you like.')


def stop(process):
    """the bake and everything it started (Python, Invader, atracdenc)"""
    if process is None or process.poll() is not None:
        return
    if sys.platform == 'win32':
        taskkill = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'taskkill.exe')
        subprocess.run([taskkill, '/T', '/F', '/PID', str(process.pid)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    else:
        try:
            os.killpg(process.pid, 9)
        except OSError:
            process.kill()
    try:
        process.wait(10)
    except subprocess.TimeoutExpired:
        pass


def _home_forms():
    folder = os.path.expanduser('~')
    forms = {folder}
    if sys.platform == 'win32':
        import ctypes
        buffer = ctypes.create_unicode_buffer(512)
        if ctypes.windll.kernel32.GetShortPathNameW(folder, buffer, 512):
            forms.add(buffer.value)
    forms |= {f.replace('\\', '/') for f in forms}
    return sorted((f for f in forms if len(f) > 3), key=len, reverse=True)


# the home folder as a whole name: not the start of a longer one
HOME = re.compile('(?:%s)(?![^\\\\/\\s\'"])' % ('|'.join(re.escape(f) for f in _home_forms()) or r'(?!)'),
                  re.IGNORECASE)


def home(text):
    """the player's home folder written as ~, as bake-log.txt writes it (in
    every form Windows writes it: any case, 8.3 short, either slash)"""
    return HOME.sub('~', text)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def size_text(n):
    return '%.1f GB' % (n / 1e9) if n >= 1e9 else '%d MB' % max(1, round(n / 1e6))


def linked(path):
    """a symbolic link, a junction or another name surrogate (a mount
    point): never followed. OneDrive's placeholders are reparse points but
    not links, and pass"""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    if stat.S_ISLNK(info.st_mode):
        return True
    reparse = getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
    return bool(reparse and getattr(info, 'st_reparse_tag', 0) & 0x20000000)  # IsReparseTagNameSurrogate


def no_links(path, top):
    """path and every folder between it and top, none of them a link"""
    path, top = Path(path), Path(top)
    for part in [path, *path.parents]:
        if part == top or len(part.parts) < len(top.parts):
            return
        if linked(part):
            raise InstallError('%s is a link, which the installer never follows: remove it, then try again.'
                               % part.name)


def lock(path):
    """the file held while a bake runs (as guided.py holds it), or None when
    another holds it; the system lets it go if the holder dies"""
    f = open(path, 'a+')
    try:
        if sys.platform == 'win32':
            import msvcrt
            f.seek(0)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return None
    return f


# --- the player's two files ---------------------------------------------------

def check_disc(path):
    """the disc image, or a folder of the disc's files; the bake checks every
    map against the retail disc's hashes before it starts"""
    if not path or not os.path.exists(path):
        raise InstallError('Choose your disc image (or the folder of the disc\'s files).')
    if os.path.isdir(path):
        if not (os.path.isfile(os.path.join(path, 'default.xbe')) or os.path.isdir(os.path.join(path, 'maps'))):
            raise InstallError('That folder has no default.xbe or maps folder: choose the folder of the disc\'s files.')
        return 'folder'
    if path.lower().endswith(('.zip', '.7z', '.rar', '.gz', '.xz', '.bz2', '.tar')):
        raise InstallError('That is a compressed archive. If you compressed your own disc image, unpack it first '
                           'and choose the .iso.')
    if os.path.getsize(path) < 500_000_000:
        raise InstallError('That file is too small to be an Xbox disc image.')
    return 'image'


def check_name(name):
    """a path the stick will store as it is: no part FAT or Windows would read
    differently, nothing that climbs out of its folder"""
    parts = name.replace('\\', '/').split('/')
    if not name or name.startswith('/') or ':' in name:
        raise InstallError('The release holds an unsafe path: %r' % name)
    for part in parts:
        if not NAME_PART.match(part) or part in ('.', '..') or part != part.strip(' ') or part.endswith('.') or \
                part.split('.')[0].strip().lower() in RESERVED:
            raise InstallError('The release holds a path the Memory Stick would read differently: %r' % name)
    return parts


def data_folders(game):
    """the stick's top folders a game's files go in (GAMES)"""
    if game not in GAMES:
        raise InstallError('%s isn\'t one of the games a release holds (halo, halomp).' % game)
    return GAMES[game]


def check_zip(path):
    """the release zip (halo_psp_bNNN.zip): the game's folder in it, and only
    PSP/GAME/<game>/ and its data folders; returns the game's folder (halo, or
    halomp for the multiplayer build)"""
    if not path or not os.path.isfile(path):
        raise InstallError('Choose the release zip (halo_psp_bNNN.zip, from the release page).')
    try:
        with zipfile.ZipFile(path) as z:
            infos = [i for i in z.infolist() if not i.is_dir()]
    except (zipfile.BadZipFile, OSError):
        raise InstallError('%s isn\'t a zip that can be read (an unfinished download?): download it again.'
                           % os.path.basename(path))
    games = [m.group(1) for m in (GAME_PATH.match(i.filename) for i in infos) if m]
    if len(games) != 1:
        raise InstallError('%s doesn\'t hold the game (one PSP/GAME/halo/EBOOT.PBP, or halomp/): it needs the '
                           'release zip, halo_psp_bNNN.zip.' % os.path.basename(path))
    game = games[0]
    allowed = tuple(['psp/game/%s/' % game.casefold()] + [f.casefold() + '/' for f in data_folders(game)])
    seen = set()
    for info in infos:
        parts = check_name(info.filename)
        key = '/'.join(parts).casefold()
        if key in seen:
            raise InstallError('The release names one file twice: %s' % info.filename)
        seen.add(key)
        if not key.startswith(allowed):
            raise InstallError('The release holds a file outside PSP/GAME/%s and its data folders: %s'
                               % (game, info.filename))
        if key.endswith(GAME_DATA):
            raise InstallError('The release holds game data, which it never should: %s' % info.filename)
    return game


# --- the bake ---------------------------------------------------------------

def bake_command(disc, game_zip, out):
    """the guided bake with the two inputs named (guided.py): it checks the
    disc, sets up the first time, bakes, and lays the stick's files out in
    out. Whole paths, so none can be read as an option"""
    return [sys.executable, str(LAUNCHER), os.path.abspath(disc), os.path.abspath(out), '--guided', '--zip',
            os.path.abspath(game_zip)]


def clear_out(out):
    """the installer's output folder, made afresh: removed only when it is ours"""
    if linked(out):
        raise InstallError('%s is a link: remove it, then try again.' % out.name)
    if out.exists():
        if not (out / OUT_MARK).is_file():
            raise InstallError('%s is in the way and wasn\'t made by the installer: move it away first.' % out.name)
        shutil.rmtree(out)
        if out.exists():
            raise InstallError('%s, from an earlier bake, can\'t be removed: close anything using it, then try '
                               'again.' % out.name)
    out.mkdir(parents=True)
    (out / OUT_MARK).write_text('The installer\'s bake output. The installer replaces it on each bake.\n',
                                encoding='utf-8')


def bake(disc, game_zip, ctx=None, out=OUT):
    """the guided bake, run as a separate process; its lines go to the
    window as they come"""
    ctx = ctx or Context()
    check_disc(disc)
    game = check_zip(game_zip)
    out = Path(out)
    held = lock(LOCK)
    if held is None:
        raise InstallError('Another bake is already running in this folder: let it finish, or close its window '
                           'first.')
    try:
        clear_out(out)
        env = dict(os.environ, PYTHONUTF8='1')
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if sys.platform == 'win32' else 0
        ctx.process = subprocess.Popen(bake_command(disc, game_zip, out), cwd=str(HERE), stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, text=True,
                                       encoding='utf-8', errors='replace', creationflags=flags,
                                       start_new_session=sys.platform != 'win32')
        said = []
        try:
            ctx.check()  # a cancel that came before the process existed
            for line in ctx.process.stdout:
                ctx.check()
                line = home(line.rstrip())
                if not line:
                    continue
                stage = STAGE.match(line)
                if stage:
                    done = MAPS_DONE.search(line)
                    ctx.stage(int(stage.group(1)), int(done.group(1)) / int(done.group(2)) if done else None)
                said = (said + [line])[-3:]
                ctx.say(line)
            code = ctx.process.wait()
            ctx.check()
        finally:
            stop(ctx.process)
    finally:
        held.close()
    if code:
        raise InstallError(said[-2] if len(said) > 1 and said[-1].startswith('(Details') else
                           (said[-1] if said else 'The bake stopped (exit %d).' % code))
    return check_baked(out, game)


def check_baked(folder, game=None):
    """a folder the bake laid out for the stick: PSP/GAME/<game>/EBOOT.PBP and
    halo/ with its maps (the menus' ui.map among them) and stamp. The
    installer's own output, or the guided bake's COPY-TO-MEMORY-STICK"""
    folder = Path(folder or '')
    games = [g for g in GAMES if (folder / 'PSP' / 'GAME' / g / 'EBOOT.PBP').is_file()]
    if game is not None:
        games = [g for g in games if g == game]
    if len(games) != 1:
        raise InstallError('That folder doesn\'t hold one baked game (PSP/GAME/halo/EBOOT.PBP, or halomp/): '
                           'choose the COPY-TO-MEMORY-STICK folder the bake made.')
    maps_folder = folder / 'halo' / 'maps'
    maps = [p for p in maps_folder.iterdir() if p.is_file() and p.suffix.lower() == '.map'] \
        if maps_folder.is_dir() else []
    if not (folder / 'halo' / 'bake.txt').is_file() or not any(p.name.lower() == 'ui.map' for p in maps):
        raise InstallError('That folder has no complete bake (halo/maps with ui.map, and halo/bake.txt): bake '
                           'first.')
    return {'folder': str(folder), 'game': games[0], 'maps': len(maps),
            'bytes': sum(p.stat().st_size for p in files(folder, games[0]))}


def files(folder, game):
    """every file of the bake's output the stick takes; a link anywhere, or a
    junction, is refused rather than followed"""
    folder = Path(folder)
    out = []
    for top in [folder / 'PSP' / 'GAME' / game] + [folder / f for f in data_folders(game)]:
        no_links(top, folder)
        if not top.is_dir():
            continue
        for root, dirs, names in os.walk(top, followlinks=False):
            for name in sorted(dirs + names):
                if linked(os.path.join(root, name)):
                    raise InstallError('The bake output holds a link, which the installer never copies: %s' % name)
            dirs.sort()
            out += [Path(root) / name for name in sorted(names)]
    return out


# --- the Memory Stick -------------------------------------------------------

def _windows_drive(root):
    """removable, and FAT or exFAT"""
    import ctypes
    kernel = ctypes.windll.kernel32
    if kernel.GetDriveTypeW(ctypes.c_wchar_p(root)) != 2:  # DRIVE_REMOVABLE
        return False
    name = ctypes.create_unicode_buffer(64)
    if not kernel.GetVolumeInformationW(ctypes.c_wchar_p(root), None, 0, None, None, None, name, 64):
        return False
    return name.value.lower() in FILE_SYSTEMS


def _mac_drive(root):
    import plistlib
    try:
        info = plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', root], capture_output=True,
                                             timeout=20).stdout)
    except (OSError, subprocess.SubprocessError, plistlib.InvalidFileException, ValueError):
        return False
    removable = info.get('RemovableMedia') or info.get('Removable') or info.get('Ejectable')
    return bool(removable) and str(info.get('FilesystemType', '')).lower() in FILE_SYSTEMS


def _linux_drive(root):
    kind = None
    try:
        with open('/proc/mounts', encoding='utf-8') as f:
            for line in f:
                fields = line.split()
                if len(fields) > 2 and fields[1].replace('\\040', ' ') == os.path.realpath(root):
                    kind = fields[2]
        device = os.stat(root).st_dev
        path = os.path.realpath('/sys/dev/block/%d:%d' % (os.major(device), os.minor(device)))
    except OSError:
        return False
    if (kind or '').lower() not in FILE_SYSTEMS:
        return False
    # a card reader on USB, or the system's own SD slot
    if '/usb' in path or '/mmc_host/' in path:
        return True
    while path.startswith('/sys/devices/'):
        if os.path.isfile(os.path.join(path, 'removable')):
            with open(os.path.join(path, 'removable')) as f:
                return f.read().strip() == '1'
        path = os.path.dirname(path)
    return False


def removable(root):
    """a removable drive with the PSP's file system (FAT or exFAT), which
    can't hold links either"""
    try:
        if sys.platform == 'win32':
            return _windows_drive(root)
        if sys.platform == 'darwin':
            return _mac_drive(root)
        return _linux_drive(root)
    except (OSError, ValueError, AttributeError):
        return False


def looks_like_stick(root):
    root = Path(root)
    return (root / 'MEMSTICK.IND').is_file() or (root / 'PSP').is_dir()


def find_sticks():
    """removable drives that look like a PSP Memory Stick, with their free space"""
    if sys.platform == 'win32':
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        # only the removable drive letters are looked at: a disconnected network
        # drive can take a long time to answer
        roots = ['%s:\\' % letter for n, letter in enumerate(string.ascii_uppercase) if mask >> n & 1 and
                 ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p('%s:\\' % letter)) == 2]
    else:
        user = os.environ.get('USER', '')
        roots = [str(p) for base in ('/Volumes', '/media/' + user, '/run/media/' + user) if os.path.isdir(base)
                 for p in Path(base).iterdir() if p.is_dir()]
    found = []
    for root in roots:
        try:
            if looks_like_stick(root) and removable(root):
                found.append({'root': root, 'free': shutil.disk_usage(root).free})
        except OSError:
            continue
    return found


def check_stick(root):
    if not root or not os.path.isdir(root):
        raise InstallError('Choose your PSP\'s Memory Stick.')
    if not looks_like_stick(root):
        raise InstallError('That drive has no MEMSTICK.IND or PSP folder, so it doesn\'t look like a PSP Memory '
                           'Stick.')
    if not removable(root):
        raise InstallError('That drive isn\'t a removable FAT32 or exFAT drive: choose the PSP\'s Memory Stick.')
    return shutil.disk_usage(root).free


def plan(baked, stick, game):
    """[(source, final path on the stick, group)] for every file to write.
    Groups, in the order they go into place: 'game' (PSP/GAME/<game>/,
    merged), 'data' (halo/ beside the maps, and halomp/; saves already there
    are kept), 'maps' (halo/maps, replacing the old folder), 'stamp'
    (halo/bake.txt)"""
    out = []
    for source in files(baked, game):
        relative = source.relative_to(baked)
        parts = check_name(relative.as_posix())
        final = stick.joinpath(*parts)
        key = relative.as_posix().casefold()
        no_links(final, stick)
        if key.startswith('psp/'):
            group = 'game'
        elif key == 'halo/bake.txt':
            group = 'stamp'
        elif key.startswith('halo/maps/'):
            group = 'maps'
        elif '/save/' in key and final.exists():
            continue  # a player's save is never replaced
        else:
            group = 'data'
        out.append((source, final, group))
    return out


def clean_staging(stick):
    """a staging folder left by an interrupted install, when it is ours"""
    staging = stick / STAGING
    if linked(staging):
        raise InstallError('%s on the stick is a link: remove it, then try again.' % STAGING)
    if staging.is_dir() and (staging / STAGING_MARK).is_file():
        shutil.rmtree(staging)
    elif staging.exists():
        raise InstallError('%s is on the stick but wasn\'t made by the installer: move it away first.' % STAGING)


def flush():
    """what the system holds for the stick, written to it (POSIX; Windows
    writes removable drives through by default)"""
    if hasattr(os, 'sync'):
        os.sync()


def read_back(path):
    """the file's SHA-256 as the stick holds it: on Linux the system's copy
    is dropped first, so the read comes from the stick"""
    with open(path, 'rb') as f:
        if hasattr(os, 'posix_fadvise'):
            os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
        digest = hashlib.sha256()
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def install(baked, stick, ctx=None):
    """the bake's output onto the stick: staged, read back, then moved into
    place; returns what was written"""
    ctx = ctx or Context()
    check_stick(stick)
    report = check_baked(baked)
    baked, stick, game = Path(baked), Path(stick), report['game']
    clean_staging(stick)
    free = shutil.disk_usage(stick).free
    work = plan(baked, stick, game)
    if not any(group == 'maps' for _, _, group in work):
        raise InstallError('The bake output has no maps to copy: bake again.')
    need = sum(source.stat().st_size for source, _, _ in work)
    if need + 50_000_000 > free:
        raise InstallError('The Memory Stick needs %s free for this, and has %s: make some room, then try again.'
                           % (size_text(need + 50_000_000), size_text(free)))
    staging = stick / STAGING
    staging.mkdir()
    try:
        (staging / STAGING_MARK).write_text('An unfinished install. The installer removes this folder when it '
                                            'next runs; deleting it yourself is safe too.\n', encoding='utf-8')
        ctx.say('Copying %d files (%s) to the Memory Stick.' % (len(work), size_text(need)))
        staged, written = [], 0
        for number, (source, final, group) in enumerate(work):
            ctx.check()
            target = staging / 'files' / str(number)
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            with open(source, 'rb') as src, open(target, 'wb') as dst:
                for block in iter(lambda: src.read(1 << 20), b''):
                    digest.update(block)
                    dst.write(block)
                dst.flush()
                os.fsync(dst.fileno())
            staged.append((target, final, group, digest.hexdigest()))
            written += source.stat().st_size
            ctx.stage(1, written / max(1, need))
        flush()
        ctx.say('Reading every file back from the Memory Stick to check it.')
        for number, (target, final, _, want) in enumerate(staged):
            ctx.check()
            if read_back(target) != want:
                raise InstallError('%s didn\'t read back the same from the Memory Stick: try another stick.'
                                   % final.name)
            ctx.stage(2, (number + 1) / len(staged))
        ctx.check()
    except BaseException:
        # nothing has moved into place yet: the stick is as it was
        shutil.rmtree(staging, ignore_errors=True)
        raise
    # into place, the old stamp out first and the new one last: the game
    # refuses maps without it, so an interruption here asks for the install
    # again rather than running the new game on the old maps
    ctx.stage(3)
    stamp = stick / 'halo' / 'bake.txt'
    if stamp.exists():
        stamp.unlink()
        flush()
    for group in ('game', 'data', 'maps', 'stamp'):
        if group == 'maps' and (stick / 'halo' / 'maps').exists():
            ctx.say('Replacing the old baked maps (halo/maps) with the checked new ones.')
            shutil.rmtree(stick / 'halo' / 'maps')
        if group == 'stamp':
            flush()
        for target, final, kind, _ in staged:
            if kind == group:
                final.parent.mkdir(parents=True, exist_ok=True)
                os.replace(target, final)
    shutil.rmtree(staging)
    flush()
    ctx.stage(4)
    return {'game': game, 'files': len(staged), 'bytes': need, 'folder': 'PSP/GAME/' + game}
