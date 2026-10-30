#!/usr/bin/env python3
"""launcher.py - starts the bake (bake.py) in its own Python environment.

The bundle's "Bake Halo" file (halo-psp-bake: bake-halo.sh on Linux, Bake
Halo.command on macOS, Bake Halo.cmd on Windows) finds a Python 3.9 to 3.14,
or downloads one with the player's yes, and runs this with the bake's
arguments. The first time, it makes a Python environment beside the bundle
(.venv) and installs the libraries in requirements.txt (numpy, Pillow,
shapely) from ready-made packages, which needs an internet connection that
once. Then it checks that the bundle's Invader starts on this system, and
runs bake.py.

    Bake Halo                 no arguments: the guided bake (guided.py), from
                              the inputs in the bundle's two folders
    halo-psp-bake <disc> <output> [options]
                              the bake itself, as bake.py takes it
    halo-psp-bake --setup     only set up the environment and check the tools
                              (with --compare-old: the proof's environment)
    halo-psp-bake --help      the bake's options (no setup needed)
    halo-psp-bake --installer the installer window (installer/): the guided
                              bake, then a copy to the Memory Stick

--compare-old (the new bake's proof, docs/bake_changes.md) also installs
scikit-learn and triangle, which have no packages for Python 3.14."""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENV = os.path.join(HERE, '.venv')
WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'
EXE = '.exe' if WINDOWS else ''
REQUIREMENTS = os.path.join(HERE, 'requirements.txt')
# what the environment was made with: a changed requirements.txt (a newer
# bundle unpacked over an older one) installs again
READY = os.path.join(VENV, 'halo-psp-bake-ready.txt')
COMPARE_PACKAGES = ['scikit-learn', 'triangle']
SUPPORTED = ((3, 9), (3, 14))


class Failed(Exception):
    """why the environment or the tools aren't ready, in a sentence"""


def say(text):
    print('halo-psp-bake: ' + text, flush=True)


def fail(text):
    raise Failed(text)


def venv_python():
    return os.path.join(VENV, 'Scripts', 'python.exe') if WINDOWS else os.path.join(VENV, 'bin', 'python')


def run(command, quiet=False, out=None):
    """True when command ran and succeeded; with out, its output goes to
    out.write (the guided bake's log) instead of the screen"""
    try:
        if out is not None:
            out.write('$ ' + ' '.join(command) + '\n')
            r = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out.write(r.stdout.decode('utf-8', 'replace'))
        else:
            r = subprocess.run(command, stdout=subprocess.DEVNULL if quiet else None,
                               stderr=subprocess.DEVNULL if quiet else None)
    except OSError:
        return False
    return r.returncode == 0


def wanted(compare):
    with open(REQUIREMENTS, encoding='utf-8') as f:
        text = f.read()
    return text + ('\n# --compare-old\n' + '\n'.join(COMPARE_PACKAGES) + '\n' if compare else '')


def installed():
    try:
        with open(READY, encoding='utf-8') as f:
            return f.read()
    except OSError:
        return None


def setup(compare, out=None):
    want = wanted(compare)
    have = installed()
    # the proof's environment holds the plain one, so a plain bake reuses it;
    # one whose Python has gone (the folder moved, the system's Python
    # replaced) is made again
    if have is not None and (have == want or (not compare and have == wanted(True))) and \
            run([venv_python(), '-c', 'import numpy, PIL, shapely'], quiet=True):
        return
    if compare and sys.version_info[:2] >= (3, 14):
        fail('--compare-old needs Python 3.13 or older (Triangle has no package for 3.14). Run it with an older '
             'Python, e.g. python3.13 %s ..., after deleting %s' % (os.path.join(HERE, 'launcher.py'), VENV))
    first = have is None
    if out is None:
        say(('first run: setting up %s' if first else 'updating %s') % VENV + ' (needs the internet this once)')

    def failed(text):
        # a setup that fails part way leaves no environment behind, so the
        # next run sets up again instead of baking with libraries missing
        shutil.rmtree(VENV, ignore_errors=True)
        fail(text)

    if not os.path.exists(venv_python()):
        shutil.rmtree(VENV, ignore_errors=True)
        if not run([sys.executable, '-m', 'venv', VENV], out=out):
            failed('making the environment failed (%s -m venv). On Debian or Ubuntu: sudo apt install python3-venv; '
                   'otherwise install Python from python.org' % sys.executable)
    py = venv_python()
    # no pip cache: the bake writes nothing outside the bundle's folder
    pip = [py, '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', '--no-cache-dir']
    # an old pip doesn't know the newer Linux package tags, and would try to
    # build the libraries from source
    if not run(pip + ['--upgrade', 'pip'], out=out):
        failed('updating pip failed (is the internet reachable?)')
    if not run(pip + ['--only-binary', ':all:', '-r', REQUIREMENTS], out=out):
        failed('installing numpy, Pillow and shapely failed: no ready-made packages for this Python (%d.%d) and '
               'system, or no internet' % sys.version_info[:2])
    if compare and not run(pip + COMPARE_PACKAGES, out=out):
        failed('--compare-old: installing scikit-learn and triangle failed')
    with open(READY, 'w', encoding='utf-8') as f:
        f.write(want)
    if out is None:
        say('environment ready')


def check_tools():
    """the bundle's Invader must start here; say why it doesn't"""
    tool = os.path.join(HERE, 'invader', 'bin', 'invader-info' + EXE)
    if not os.path.exists(tool):
        fail('%s is missing: unpack the whole bundle again' % tool)
    if run([tool, '--help'], quiet=True):
        return
    if MACOS and run(['xattr', '-p', 'com.apple.quarantine', tool], quiet=True):
        fail('macOS blocks the bundle\'s tools (they are not signed by an Apple developer). Allow them once, then '
             'run this again:\n    xattr -dr com.apple.quarantine "%s"' % HERE)
    if WINDOWS:
        fail('%s does not start. If Windows or an antivirus blocked it, allow it (right-click the downloaded zip, '
             'Properties, Unblock, then unpack it again)' % tool)
    if MACOS:
        fail('%s does not start on this Mac (it needs macOS 11 or later)' % tool)
    if sys.platform.startswith('linux'):
        fail('%s does not start on this system. The bundle needs an x86-64 Linux with glibc 2.28 or later '
             '(2018 on: Ubuntu 18.10, Debian 10, Fedora 29, RHEL 8 or newer); INVADER.md says how to build it '
             'from source' % tool)
    fail('%s does not start on this system' % tool)


def main():
    try:
        launch(sys.argv[1:])
    except Failed as e:
        say(str(e))
        sys.exit(1)


def launch(args):
    if not SUPPORTED[0] <= sys.version_info[:2] <= SUPPORTED[1]:
        fail('this is Python %d.%d; the bake needs 3.9 to 3.14' % sys.version_info[:2])
    bake = os.path.join(HERE, 'bake.py')
    env = dict(os.environ)
    # every script reads and writes text as UTF-8, whatever the system's
    # code page (Windows' cp1252 can't print every tag name or log line)
    env['PYTHONUTF8'] = '1'
    if args == ['--installer']:
        # the installer window, which runs the guided bake below as its own
        # process
        import installer
        sys.exit(installer.main())
    if not args or '--guided' in args:
        import guided
        sys.exit(guided.main([a for a in args if a != '--guided'], sys.modules[__name__]))
    if '-h' in args or '--help' in args:
        sys.exit(subprocess.call([sys.executable, bake] + args, env=env))
    only_setup = '--setup' in args and set(args) <= {'--setup', '--compare-old'}
    setup('--compare-old' in args)
    check_tools()
    if only_setup:
        say('the bundle\'s tools start, and the environment is ready: %s' % VENV)
        return
    command = [venv_python(), bake] + args
    if WINDOWS:
        try:
            sys.exit(subprocess.call(command, env=env))
        except KeyboardInterrupt:
            sys.exit(130)
    os.execve(command[0], command, env)


if __name__ == '__main__':
    main()
