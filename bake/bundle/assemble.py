#!/usr/bin/env python3
"""assemble.py - puts a bake bundle together: the bake's scripts from this
checkout, the native tools build_tools.sh made, and every licence.

    assemble.py <tools folder> <bundle folder> [--target linux|macos|windows] [--archive]

<tools folder> is build_tools.sh's output. The bundle folder is made afresh;
--archive also packs it beside itself (.tar.gz, or .zip for Windows, named
as the bundle folder) with one top folder, halo-psp-bake: short, as Windows
counts every character of the paths the bake writes inside it. Pure Python:
runs on every OS."""
import argparse
import glob
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# the tools the bake runs from tools/ (the rest of tools/ is the port's own)
TOOLS = ['texpack.py', 'stencilbake.py', 'dez.py', 'lightmap_masks.py', 'bspread.py', 'levelslim.py']
# a tool in tools/ finds the bake's readers in bake/lib with this line; in the
# bundle they sit side by side, so it goes
READERS_LINE = "'bake', 'lib'))  # the bake's readers"

# What is inside, its licence, where its licence texts are (LICENSES/<folder>)
# and where its source is. Only components whose texts the build found are
# listed in the bundle's README.
COMPONENTS = [
    ('the bake scripts (bake.py, guided.py, launcher.py, xiso.py, lib/, installer/, and the "Bake Halo" and '
     '"Install to PSP" files)', 'Sierra-III, the Halo PSP port: see its repository',
     None, 'here, as they are'),
    ('Invader (invader/bin: invader-build, invader-extract, invader-info), the port\'s fork',
     'GPL-3.0-only (its ADPCM encoder: BSD-3-Clause; its Xbox ADPCM changes: MIT)', 'invader',
     'source/invader-src.tar.gz (upstream d43ce55 with source/invader-psp-fork.patch and '
     'source/invader-no-editor.patch applied); https://github.com/SnowyMouse/invader'),
    ('Invader\'s tag definitions (invader/definitions)', 'GPL-3.0-only, part of Invader', 'invader',
     'source/invader-src.tar.gz'),
    ('Rat In a Tube (Invader\'s script compiler, linked into invader/bin)', 'GPL-3.0-only', 'riat',
     'source/invader-src.tar.gz (ext/riat); https://github.com/SnowyMouse/riat'),
    ('libFLAC (linked into invader/bin)', 'BSD-3-Clause (Xiph)', 'flac', 'https://github.com/xiph/flac (1.4.3)'),
    ('libvorbis, libvorbisenc, libvorbisfile (linked into invader/bin)', 'BSD-3-Clause (Xiph)', 'libvorbis',
     'https://github.com/xiph/vorbis (1.3.7)'),
    ('libogg (linked into invader/bin)', 'BSD-3-Clause (Xiph)', 'libogg', 'https://github.com/xiph/ogg (1.3.5)'),
    ('libsamplerate (linked into invader/bin)', 'BSD-2-Clause', 'libsamplerate',
     'https://github.com/libsndfile/libsamplerate (0.2.2)'),
    ('libsquish (linked into invader/bin)', 'MIT', 'libsquish', 'https://sourceforge.net/projects/libsquish/ (1.15)'),
    ('zlib (linked into invader/bin)', 'zlib', 'zlib', 'https://github.com/madler/zlib (1.3.1)'),
    ('Rust standard library (linked into invader/bin through Rat In a Tube)', 'MIT or Apache-2.0', 'rust-std',
     'https://github.com/rust-lang/rust'),
    ('atracdenc (atracdenc/bin), the XMB menu sound\'s ATRAC3 encoder', 'LGPL-2.1 (its getopt for Windows: ISC)',
     'atracdenc', 'source/atracdenc-src.tar.gz (cb17070); https://github.com/dcherednik/atracdenc'),
    ('libgha (linked into atracdenc)', 'BSD-2-Clause', 'libgha', 'source/atracdenc-src.tar.gz (src/lib/libgha)'),
    ('KISS FFT (linked into atracdenc)', 'BSD-3-Clause', 'kissfft',
     'source/atracdenc-src.tar.gz (src/lib/libgha/src/3rd/kissfft)'),
    ('libsndfile (linked into atracdenc; WAV only, no external codecs)', 'LGPL-2.1-or-later', 'libsndfile',
     'source/libsndfile-1.2.2.tar.xz; https://github.com/libsndfile/libsndfile'),
    ('the GCC runtime (libstdc++, libgcc; linked in on Linux and Windows)',
     'GPL-3.0 with the GCC Runtime Library Exception, which puts no conditions on programs linked with it',
     'gcc-runtime', 'https://gcc.gnu.org'),
    ('the MinGW-w64 runtime and winpthreads (linked in on Windows)', 'ZPL-2.1, MIT and public domain parts',
     'mingw-w64', 'https://www.mingw-w64.org'),
    ('ChupathingyCE (the engine the PSP build runs; no code of it is in this bundle)', 'CC0-1.0', 'chupathingyce',
     'https://github.com/ChupathingyCE/chupathingyce'),
]

LICENSES_README = '''Licences of what is in this bundle
==================================

Every component, its licence, where its licence texts are (in this folder)
and where its source is. The GPL and LGPL pieces are linked statically; their
complete source, exactly as built, is in source/, with build_tools.sh, the
script that built them, so you can change and rebuild them (and relink
atracdenc with another libsndfile).

'''

TARGET_EXE = {'windows': '.exe', 'linux': '', 'macos': ''}
# what the player double-clicks (the wrapper, halo-psp-bake or .cmd, under
# the name each system runs on a double-click)
START = {'linux': 'bake-halo.sh', 'macos': 'Bake Halo.command', 'windows': 'Bake Halo.cmd'}
# the installer window's double-click file: the wrapper above, with --installer
INSTALL = {'linux': 'install-to-psp.sh', 'macos': 'Install to PSP.command', 'windows': 'Install to PSP.cmd'}
INSTALL_TEXT = {
    'windows': '@echo off\nrem The installer window (installer/), with the Python the bake finds.\n'
               '"%%~dp0%s" --installer\n',
    'posix': '#!/bin/sh\n# The installer window (installer/), with the Python the bake finds.\n'
             'exec "$(cd "$(dirname "$0")" && pwd)/%s" --installer\n',
}
TOP = 'halo-psp-bake'
# the guided bake's two input folders (guided.py), each with a note of what goes in it
INPUTS = {
    '1-PUT-YOUR-DISC-IMAGE-HERE': """Put your Halo disc image in this folder: one file, as your Xbox dumping
tool made it (usually ending in .iso). It must be Halo: Combat Evolved for
the original Xbox, NTSC (USA), version 01.10.12.2276; the bake checks.

A folder with the disc's files works too (the one holding default.xbe and
maps): put that folder here instead.

Then start the bake: %s.
""",
    '2-PUT-THE-HALO-ZIP-HERE': """Put the Halo PSP release zip in this folder, as you downloaded it from
the release page (halo_psp_bNNN.zip). There's no need to unpack it.

The bake adds the menu sound, made from your disc, to the game inside it,
and puts the game and the maps together in COPY-TO-MEMORY-STICK.

Then start the bake: %s.
""",
}
HOW_TO_START = {'linux': 'double-click bake-halo.sh (or run ./bake-halo.sh in a terminal)',
                'macos': 'double-click "Bake Halo.command"', 'windows': 'double-click "Bake Halo.cmd"'}


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def commit():
    """the checkout's commit: GitHub's for its runs (its containers may have
    no git), else git's"""
    if os.environ.get('GITHUB_SHA'):
        return os.environ['GITHUB_SHA']
    try:
        return subprocess.run(['git', '-C', REPO, 'rev-parse', 'HEAD'], capture_output=True, text=True,
                              timeout=30).stdout.strip() or 'unknown'
    except (OSError, subprocess.SubprocessError):
        return 'unknown'


def executable(path):
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def assemble(tools, out, target):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    bake = os.path.join(REPO, 'bake')
    for name in ('bake.py', 'guided.py', 'xiso.py', 'launcher.py', 'requirements.txt', 'hashes.json', 'tested_images.json', 'README.md',
                 'INVADER.md'):
        copy(os.path.join(bake, name), os.path.join(out, name))
    # which bundle this is, for the stamp each bake writes (bake.py's bundle_label)
    with open(os.path.join(out, 'bundle.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('name: %s\ncommit: %s\n' % (os.path.basename(os.path.abspath(out)), commit()))
    newline = '\r\n' if target == 'windows' else '\n'
    wrapper = 'halo-psp-bake.cmd' if target == 'windows' else 'halo-psp-bake'
    with open(os.path.join(bake, wrapper), encoding='utf-8') as f:
        text = f.read().replace('\r\n', '\n')
    # cmd.exe finds its labels reliably only in a file with Windows' line endings
    with open(os.path.join(out, START[target]), 'w', encoding='utf-8', newline=newline) as f:
        f.write(text)
    with open(os.path.join(out, INSTALL[target]), 'w', encoding='utf-8', newline=newline) as f:
        f.write(INSTALL_TEXT['windows' if target == 'windows' else 'posix'] % START[target])
    executable(os.path.join(out, INSTALL[target]))
    for path in sorted(glob.glob(os.path.join(bake, 'installer', '*.py'))):
        if not os.path.basename(path).startswith('test_'):
            copy(path, os.path.join(out, 'installer', os.path.basename(path)))
    executable(os.path.join(out, START[target]))
    if target == 'linux':
        # the command's own name, which automated runs call
        os.symlink(START[target], os.path.join(out, 'halo-psp-bake'))
    for folder, note in INPUTS.items():
        os.makedirs(os.path.join(out, folder))
        with open(os.path.join(out, folder, 'README.txt'), 'w', encoding='utf-8', newline=newline) as f:
            f.write(note % HOW_TO_START[target])
    # every bake/lib script, whatever has been added since
    scripts = sorted(glob.glob(os.path.join(bake, 'lib', '*.py'))) + [os.path.join(REPO, 'tools', t) for t in TOOLS]
    os.makedirs(os.path.join(out, 'lib'))
    for path in scripts:
        with open(path, encoding='utf-8') as f:
            lines = [line for line in f if READERS_LINE not in line]
        with open(os.path.join(out, 'lib', os.path.basename(path)), 'w', encoding='utf-8', newline='\n') as f:
            f.writelines(lines)
    for folder in ('invader', 'atracdenc', 'source'):
        shutil.copytree(os.path.join(tools, folder), os.path.join(out, folder))
    exe = TARGET_EXE[target]
    for path in glob.glob(os.path.join(out, 'invader', 'bin', '*')) + glob.glob(os.path.join(out, 'atracdenc', 'bin', '*')):
        executable(path)
    for tool in ('invader/bin/invader-build', 'invader/bin/invader-extract', 'invader/bin/invader-info',
                 'atracdenc/bin/atracdenc'):
        if not os.path.exists(os.path.join(out, tool + exe)):
            sys.exit('assemble: %s%s is missing from %s' % (tool, exe, tools))
    licenses = os.path.join(out, 'LICENSES')
    shutil.copytree(os.path.join(tools, 'licenses'), licenses)
    # Rust's standard library is linked into Invader on every OS, but MSYS2's
    # Rust ships no licence texts: these are rustup's (MIT and Apache-2.0,
    # the same for every release)
    if not os.path.isdir(os.path.join(licenses, 'rust-std')):
        shutil.copytree(os.path.join(REPO, 'bake', 'bundle', 'licenses', 'rust-std'), os.path.join(licenses, 'rust-std'))
    readme = [LICENSES_README]
    for what, licence, folder, source in COMPONENTS:
        if folder and not os.path.isdir(os.path.join(licenses, folder)):
            continue
        texts = ', '.join(sorted(os.listdir(os.path.join(licenses, folder)))) if folder else '-'
        readme.append('%s\n    licence: %s\n    texts:   %s\n    source:  %s\n\n'
                      % (what, licence, 'LICENSES/%s/ (%s)' % (folder, texts) if folder else texts, source))
    unlisted = sorted(set(os.listdir(licenses)) - {c[2] for c in COMPONENTS if c[2]})
    if unlisted:
        sys.exit('assemble: LICENSES/%s has no line in COMPONENTS' % ', '.join(unlisted))
    with open(os.path.join(licenses, 'README.txt'), 'w', encoding='utf-8', newline='\r\n' if target == 'windows' else '\n') as f:
        f.write(''.join(readme))


def contents(out):
    """each top-level entry and LICENSES/ folder of the bundle, with its size"""
    def size(path):
        if os.path.isfile(path):
            return os.path.getsize(path)
        return sum(os.path.getsize(os.path.join(root, f)) for root, _, files in os.walk(path) for f in files)
    def text(n):
        return '%8.1f MB' % (n / 1e6) if n >= 1e5 else '%8.1f KB' % (n / 1e3)
    lines = ['%-28s %s' % (name + ('/' if os.path.isdir(os.path.join(out, name)) else ''), text(size(os.path.join(out, name))))
             for name in sorted(os.listdir(out))]
    lines.append('LICENSES/: ' + ', '.join(sorted(d for d in os.listdir(os.path.join(out, 'LICENSES'))
                                                  if os.path.isdir(os.path.join(out, 'LICENSES', d)))))
    return lines


def archive(out, target):
    parent, name = os.path.split(os.path.abspath(out))
    if target == 'windows':
        path = os.path.join(parent, name + '.zip')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for root, dirs, files in os.walk(out):
                dirs.sort()
                # the empty input folders too
                if root != out and not files and not dirs:
                    z.write(root, os.path.join(TOP, os.path.relpath(root, out)))
                for f in sorted(files):
                    full = os.path.join(root, f)
                    z.write(full, os.path.join(TOP, os.path.relpath(full, out)))
    else:
        path = os.path.join(parent, name + '.tar.gz')
        with tarfile.open(path, 'w:gz', compresslevel=9) as t:
            t.add(out, arcname=TOP)       # keeps the executable bits and the symlink
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tools')
    ap.add_argument('out')
    ap.add_argument('--target', choices=sorted(TARGET_EXE),
                    default='windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux')
    ap.add_argument('--archive', action='store_true')
    a = ap.parse_args()
    assemble(a.tools, a.out, a.target)
    print('assemble: %s' % a.out)
    lines = contents(a.out)
    print('\n'.join('  ' + line for line in lines))
    if os.environ.get('GITHUB_ACTIONS'):
        # on the run's page, without opening the log
        print('::notice title=bundle contents::' + '%0A'.join(lines))
    if a.archive:
        path = archive(a.out, a.target)
        print('assemble: %s (%.1f MB)' % (path, os.path.getsize(path) / 1e6))


if __name__ == '__main__':
    main()
