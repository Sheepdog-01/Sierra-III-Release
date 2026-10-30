# halo-psp-bake

Turns **your own copy** of Halo: Combat Evolved for the original Xbox into the
map files the PSP build reads, and puts them together with the game, ready to
copy to the Memory Stick. Nothing from the game is included here.

## What you need

- **A computer:** Windows 10 or 11 (x86-64), or Linux (x86-64) with glibc 2.39
  or later (Ubuntu 24.04, Debian 13, Fedora 40, a current rolling release). A
  macOS bundle comes with a release only when its notes list one (b143 has
  none).
- **An image of your Halo disc:** NTSC (USA), version 01.10.12.2276, the one
  the build was made for (the bake checks every map). A bare game-partition
  image (what most Xbox dumping tools and extract-xiso make), a full Redump
  image, or a folder of the disc's files (the one holding `default.xbe` and
  `maps`).
- **The release zip** (`halo_psp_bNNN.zip`) from the same release as this
  bundle.
- **Free disk space:** the bake says how much before it starts, and stops if
  there isn't enough (about 8 GB). The result is about 2.2 GB for the
  campaign and the menus, 2.9 GB with the multiplayer maps.
- **The internet, the first time:** the bake fetches its Python libraries
  (numpy, Pillow, shapely, about 30 MB). Python itself it uses from your
  computer if it has 3.9 to 3.14; if not, it offers to download a portable
  Python 3.12 (python-build-standalone's official release, about 25 to 35 MB,
  checked against the SHA-256 the bake carries) into `.python` in this folder.
  Nothing is installed anywhere else. The bake keeps what it makes (palettes
  and re-encoded sounds) in your user cache folder, `halo-psp-bake` in
  `%LOCALAPPDATA%` on Windows, `~/Library/Caches` on macOS, `~/.cache` on
  Linux, so baking a new release takes minutes instead of the full time. It
  says where and how big; deleting it is always safe.

## Bake in four steps

1. **Unpack the bundle.** Windows: right-click the zip, *Extract All*. Keep the
   folder's path short (Windows counts every character of the paths the bake
   writes inside it; the bake says if it's too long, and `C:\halo-psp-bake`
   always works).
2. **Put in your two files:**
   - your disc image in `1-PUT-YOUR-DISC-IMAGE-HERE`;
   - the release zip, as downloaded, in `2-PUT-THE-HALO-ZIP-HERE`.
3. **Double-click the bake:**
   - **Windows:** `Bake Halo.cmd`. If *Windows protected your PC* appears,
     choose *More info*, then *Run anyway*.
   - **macOS:** `Bake Halo.command`. The first time, macOS says it can't check
     the file (it isn't signed by an Apple developer): choose *Done*, open
     *System Settings*, *Privacy & Security*, scroll down to `Bake Halo.command`
     and choose *Open Anyway*, then double-click it again. On macOS 14 or
     earlier: right-click it, *Open*, *Open*.
   - **Linux:** `bake-halo.sh` (if your file manager asks, *Run*; or
     right-click, *Run as a Program*). If it only opens in a text editor, open
     a terminal in this folder and type `./bake-halo.sh`.

   A window opens and shows what it found, then four stages:

   | Stage | What happens | How long |
   |---|---|---|
   | 1/4 Reading your disc | each map read and checked against the retail disc's hashes | a minute or two |
   | 2/4 Setting up | the bake's Python libraries, the first time only | a minute or two (then seconds) |
   | 3/4 Baking the maps | each map rebuilt for the PSP's memory, its textures and wall decals baked; several maps at once, with the time left | the long part: about 10 minutes on an 8-core PC, longer on fewer cores |
   | 4/4 Putting it together | the game, its menu sound and the maps into `COPY-TO-MEMORY-STICK` | seconds |

   It ends by saying what to copy where. Press Enter to close the window.
4. **Copy to the Memory Stick:** the two folders inside `COPY-TO-MEMORY-STICK`
   (`PSP` and `halo`) go to the top of the stick, **merged** with the folders
   already there. Windows merges by itself (if it asks about files with the
   same name, *Replace the files in the destination*). On macOS use Terminal,
   not Finder, which can replace the stick's whole `PSP` folder, ARK-4's
   files with it: `cp -R ~/Downloads/halo-psp-bake/COPY-TO-MEMORY-STICK/.
   /Volumes/<the stick's name>/`.

The campaign build's bake makes the ten campaign levels and the menus. Put
the multiplayer build's zip (`halo_psp_bNNNmp.zip`) in step 2 instead and it
bakes the thirteen multiplayer maps too, and makes that build's menu sound.

To bake again (a new release), put the new zip in step 2 and double-click
again: `COPY-TO-MEMORY-STICK` is made afresh each time. On the stick, delete
`halo/maps` before you copy the new one in, so no old pack is left behind.

## Or with a window: the installer

`Install to PSP.cmd` (Windows), `Install to PSP.command` (macOS) or
`install-to-psp.sh` (Linux) opens the same bake in a window, and then copies
it to the Memory Stick for you:

1. **Your game:** where the maps come from (the Xbox disc; a Halo PC option
   is shown but switched off for now), and that you own it.
2. **Your files:** your disc image and the release zip, chosen with a button
   instead of put in a folder. Already baked with *Bake Halo*? Tick the box
   and choose its `COPY-TO-MEMORY-STICK` instead.
3. **Bake the maps:** the guided bake above, unchanged, with its four stages
   shown as it goes. Its output and `bake-log.txt` go to `out` in this folder.
4. **Memory Stick:** only removable FAT32 or exFAT drives with
   `MEMSTICK.IND` or a `PSP` folder are listed. It writes `PSP/GAME/halo/`
   and `halo/` (the multiplayer build: `PSP/GAME/halomp/`, `halomp/` and the
   maps in `halo/`), merged with what is there: an older `halo/maps` is
   replaced, saves already on the stick are kept, and ARK-4's files,
   `SEPLUGINS` and other games are never touched. Every file is copied to
   `SIERRA-III-STAGING` on the stick first and read back before anything is
   moved into place; a copy stopped or failed before then leaves the stick as it
   was. The old `halo/bake.txt` comes out first and the new one goes in last,
   so a copy interrupted while files move into place leaves maps the game
   refuses until you install again.
5. **Play:** eject the stick safely, then start the game from the PSP's
   *Game* menu.

It uses the Python the bake finds, with its Tk. Some Linux systems' Python
comes without Tk (`python3-tk`); the installer then says so, and *Bake Halo*
works as before.

## Which disc image it was made from

Every bake fingerprints what it reads and says so at the start of its log, then writes the same lines into `halo/bake.txt` beside the format stamp:

- the disc image's name, size, and hashes of the whole file (SHA-1, MD5, CRC32, as the disc-preservation databases list them);
- `default.xbe`'s SHA-256 and the title's own record in it: title id, name, version, regions and disc number;
- how many of the maps are the retail ones this release knows (`hashes.json`);
- whether the image is one the releases were tested with (`tested_images.json`).

It never refuses an image for this. An image nobody has tested still bakes; the log and the stamp just say it isn't on the tested list. If something looks wrong in play, send those lines with your report: they tell us which image it came from, so problems can be traced to the images that cause them.

**Tested with:** the images in `tested_images.json`, which each release fills in with the image it was baked from and tested on. So far one:

| Image | default.xbe | Builds |
|---|---|---|
| Halo - Combat Evolved (USA) (Rev 2), as an xiso: SHA-1 `bf3e1071bf8b7f0080cae86adc32b44233dbe8aa`, 3,625,713,664 bytes | title 4D530004 version 9, North America; SHA-256 `4094e994…d95aae` | b146 |

An xiso is the disc's file system repacked, so its whole-file hashes are its own; the `default.xbe` lines are what match another copy of the same disc. A bake counts an image as tested when either matches.

## When it stops

Every problem ends in one sentence saying what's wrong and what to do, and
the details go to `bake-log.txt` in this folder. The common ones:

| It says | What to do |
|---|---|
| Put your Halo disc image (or the release zip) in the folder ... | that folder is empty |
| There are 2 things in ... | leave only the disc image (or the zip) in that folder |
| ... is a compressed archive | unpack it first; put the `.iso` inside it in the folder |
| ... isn't an Xbox disc image this bake can read | it isn't a disc image, or not a whole one: make it again from your disc |
| This disc is another version of Halo ... | another version or region: the bake needs NTSC 01.10.12.2276 |
| N of the disc's maps (...) isn't / aren't the retail ones ... | a damaged image or changed maps: make it again from your disc |
| This folder's path is too long for Windows ... | move the whole folder to `C:\halo-psp-bake` |
| There is ... free on this drive, and the bake needs about ... | free some space, or move the folder to a drive with more |
| Setting up failed: ... | usually no internet the first time; the sentence says |
| The bake of ... failed | please report it, with `bake-log.txt` |

Two notes don't stop the bake: the **menu sound** (the music the PSP plays on
the game's entry in its menu, cut from your disc) and the **loading screen's
chant** (read from your disc's `default.xbe`). If either can't be made, the
game plays all the same, and the end of the bake says why.

`bake-log.txt` is what to attach to an issue. It holds the names, sizes and
hashes of your disc's maps and everything the bake printed, with your home
folder written as `~`; never anything from the disc itself.

## The command line

The same file takes the bake's own arguments, for a different output folder
or the options below (on Windows `"Bake Halo.cmd" ...`, on macOS
`./Bake\ Halo.command ...`):

```
./bake-halo.sh <disc image or folder> <output folder> [options]
```

The output folder then holds `halo/maps/` and `halo/chant.adp` (and, with
`--eboot`, `PSP/GAME/halo/EBOOT.PBP`): copy its contents to the stick. Unlike
the double-click, it doesn't check the disc first (a map that doesn't match
is skipped, or baked anyway with `--force`), and like it, it bakes the sounds
the way the release is tested (`--sound s9`; `--sound none` keeps the disc's
as they are).

| Option | What it does |
|---|---|
| `--maps c20,bloodgulch,ui` | only these maps (always include `ui`, the menus) |
| `--multiplayer` | the thirteen multiplayer maps too |
| `--no-clut4-look` | keep lightmaps and HUD sheets at 256 colours (by default the ones that look the same at 16 colours, under a strict error bar, are baked at 16: smaller maps, a little faster to draw); `--no-clut-look` is the same |
| `--no-cache` | make every palette afresh (by default a palette made once is kept in `<output>/work/palettes` for the textures that come up again in the asset track's pack and in other maps; the maps come out the same either way) |
| `--jobs N` | bake N maps at once (default: one per CPU, at most 4, fewer with under 8 GB of memory free; the maps come out the same either way); each map's own log is in `<output>/work/<map>.log` |
| `--detail fp,vehicles` | also bake `_detail.ptex`, those classes' textures at full size (`fp`, `weapons`, `vehicles`, `characters`), for the launch menu's "asset detail" |
| `--sound s9` | the default: every sound at 22,050 Hz at most, effects mono, music stereo (`none`: the disc's sounds as they are) |
| `--eboot PATH` | the release's `EBOOT.PBP`: a copy with the menu sound goes to `<output>/PSP/GAME/halo/` (`halomp/` for the multiplayer build's) |
| `--music [MAP:]TAG[@START][+LENGTH]` | another clip for the menu sound; `--maps ui --list-music` lists a map's music and lengths |
| `--setup` | only set up the Python environment and check the tools |
| `--help` | every option |

The menu sound is encoded with atracdenc, which every bundle carries. If it
won't start, the bake warns, loudly, and copies the EBOOT as it is.

Maps from this version of the tool are for **build 139 or later** (its
palettes and wall decals are made differently: Sierra-III's
`docs/bake_changes.md`). Bake again when a release says its bundle changed.

The bake leaves a stamp beside the maps, `halo/bake.txt`: the bake format
they are and the bundle that made them. The game reads it as it starts. If
the maps are from a bundle whose format this build doesn't read (or have no
stamp: bakes before build 139), it says so on the screen, "YOUR MAPS NEED
BAKING AGAIN" with the bundle it needs, and START quits: bake again with the
bundle that came with the build and copy the new `halo` folder over the old
one. Builds that read the same format share a bake.

## What's inside, and the licences

- `Bake Halo.cmd`, `Bake Halo.command` or `bake-halo.sh` (and on Linux
  `halo-psp-bake`, the same file under the command's own name),
  `launcher.py`, `guided.py`, `bake.py`, `xiso.py`, `lib/`: the bake itself
  (the Halo PSP port's own scripts).
- `Install to PSP.cmd`, `Install to PSP.command` or `install-to-psp.sh`, and
  `installer/`: the installer window (the port's own scripts too; its look
  is drawn by Tk, with no image or font files).
- `invader/bin`: Invader (`invader-build`, `invader-extract`, `invader-info`),
  the port's fork, GPLv3. Its full source is `source/invader-src.tar.gz`, and
  `source/invader-psp-fork.patch` is the change against upstream Invader
  (commit d43ce55). INVADER.md says how to rebuild it.
- `atracdenc/bin`: atracdenc, LGPL-2.1, with libsndfile (LGPL-2.1) linked in;
  their source is in `source/`.
- Every program is linked statically, so nothing else needs installing.
- `LICENSES/README.txt` lists every component, its licence and where its
  source is; the licence texts are beside it.
- Made by the bake, in this folder only: `.venv` (the Python libraries),
  `.python` (the portable Python, if it downloaded one), `temp` (while it
  works), `COPY-TO-MEMORY-STICK` and `bake-log.txt`; the installer's bake
  goes to `out` instead.
