# Playing Sierra-III: from a stock PSP to a playable game

This guide takes you from a PSP and your own Halo: Combat Evolved disc for the original Xbox to the game running on the PSP. Follow the steps in order. Each step says what you need, what to type, and how to tell it worked (the **Check:** lines).

Sierra-III contains nothing from the game. You bake the maps yourself, from your own disc, on your own PC. Never download game files, and never share the ones you bake.

These steps are for **release b148**. It is the campaign only: multiplayer isn't part of it (step 7).

## The short version

1. Get a PSP with custom firmware (ARK-4) and a Memory Stick with 3.5 GB free (steps 1 and 2).
2. Make an image of your own Halo disc for the original Xbox, NTSC, version 01.10.12.2276 (step 3).
3. From the release page, download the game zip and the bake tool for Windows or Linux (step 4).
4. Unpack the bake tool, put the disc image and the game zip in its two folders, and double-click the bake. It takes about 10 minutes on an 8-core PC (step 4).
5. Copy what it made to the root of the Memory Stick (step 5).
6. On the PSP: Game, Memory Stick, Sierra-III: Halo CE port (step 6).

**What to expect:** the whole campaign plays, but only the lightest levels run near full speed. Most run between 12 and 20 frames a second and the heaviest, *The Silent Cartographer*, about 6 (step 6, "What to expect"). It is a work in progress.

**Commands** are for a terminal on Linux or macOS (bash, zsh or dash all work). On Windows, each step says what to do in File Explorer or the Command Prompt instead. Three names stand for paths on your machine:
- `$STICK`: where your PSP's Memory Stick is mounted (step 1, "Finding `$STICK`", shows how to find it).
- `~/Downloads`: where you saved the release's files.
- `$BAKE`: the bake bundle's folder, once unpacked (step 4): `~/Downloads/halo-psp-bake` (on Windows, the `halo-psp-bake` folder wherever you unpacked it).

## 1. What you need

1. **A PSP.** The port is built for the PSP-1000 and tested on it; the PSP-2000 and PSP-3000 work too.
2. **Custom firmware:** official firmware 6.61 with ARK-4 (step 2). Sierra-III is built and tested on that.
3. **A Memory Stick** (or an adapter) with about 3.5 GB free: the bake of all 24 maps is about 2.9 GB, and the rest is room for the game and its logs.
4. **Your Halo disc** for the original Xbox, **NTSC, version 01.10.12.2276**, as a disc image (step 3). Other versions and languages aren't supported yet: the bake refuses them rather than guess (step 3).
5. **A PC to bake on**, any one of:
   - Windows 10 or 11 (x86-64);
   - Linux (x86-64) with **glibc 2.39 or later**: Ubuntu 24.04, Debian 13, Fedora 40, or a current rolling release such as Arch. Older systems can't run b148's Linux bake tool; a build for older ones is planned.

   b148 has no macOS bake tool. Bake on a Windows or Linux PC (a virtual machine will do), then copy the result to the stick from your Mac (step 5).

   The bake uses the computer's own **Python** if it is 3.9 to 3.14; if there is none, it asks to download a portable one into its own folder. It needs the internet the first time.
6. **About 8 GB free on the PC** while the bake runs. The result is about 2.2 GB (the campaign and the menus); the rest is the bake's working files, which it deletes as it goes. The 8 GB is a safe estimate: the bake says how much it needs before it starts, and stops if the drive hasn't that much free.
7. **A USB cable** for the PSP, or a card reader for the Memory Stick.

**Finding `$STICK`.** On the PSP, open **Settings → USB Connection** and plug the cable in. Most Linux desktops mount the stick by themselves. Then:

```sh
lsblk -o NAME,SIZE,LABEL,MOUNTPOINT      # find the stick by its size; its MOUNTPOINT is the path
STICK=/run/media/$USER/<label>           # or whatever MOUNTPOINT showed; set it again in each new terminal
ls "$STICK"                              # a stick in use shows PSP/ at least
```

On **macOS** the stick shows in Finder and on the desktop; in Terminal it is `/Volumes/<label>`, so `STICK=/Volumes/<label>`. On **Windows** it shows in File Explorer as a drive of its own (`E:`, say).

**When you're done with the stick**, every time: on Linux, `sync`, then `umount "$STICK"` (or eject it from the desktop); on macOS, eject it in Finder; on Windows, "Safely Remove Hardware" or eject it in File Explorer. Then leave USB mode on the PSP.

**Check:** `ls "$STICK"` lists `PSP` (on Windows, the drive holds a `PSP` folder).

## 2. Custom firmware (ARK-4)

The PSP runs homebrew only on custom firmware. We don't write a CFW guide: follow ARK-4's own.

1. Install ARK-4 by **[its own instructions](https://github.com/PSP-Archive/ARK-4)**. They say which official firmware it needs and how to install it on your model; Sierra-III is tested on 6.61.
2. **Settings that matter for Sierra-III: none.** The game sets the PSP's standard clock (333 MHz) by itself; it never needs an overclock. Leave ARK-4's settings as its guide sets them.
3. From now on, the stick holds ARK-4's own files. **Never delete them, and never format the stick** (step 5 has the rules).

**Check:** the PSP boots to its home menu (the XMB) with ARK-4 running, as ARK-4's guide describes. The real test comes in step 6, when the game shows up in the game list.

## 3. Imaging your disc

The bake reads your disc's files out of an image you made. It takes any of these:
- **A bare game-partition image** ("XISO"): what extract-xiso and most Xbox dumping tools make.
- **A full Redump-style image** of the disc.
- **A folder** with the disc's files: the one that holds `default.xbe` and `maps/`, so the bake finds both (with only the maps, the loading screen is silent: step 4).

1. **Make the image from your own disc.** An ordinary PC DVD drive can't read an Xbox disc; Redump's guides describe ways to dump one. Don't download an image.
2. **The bake checks it for you** (step 4): before anything long starts, it reads every map it needs out of the image and checks each against the retail NTSC 01.10.12.2276 maps' SHA-256 (`hashes.json` in the bundle). Other disc versions, and other language releases in particular, aren't checked against anything yet, so the bake refuses them. Support for more versions is planned, and will come with a release that says so.

**Check:** in step 4, the bake's first stage ends with `[1/4] Your disc checks out`. If it says the disc is another version, or that some maps aren't the retail ones ("Hash mismatch", step 4), the image is another version or region, or a bad dump: make it again from your disc.

## 4. Baking the maps

The bake turns your disc's maps into the files the PSP reads: each map rebuilt for the PSP's memory, plus its texture, geometry and wall-decal packs. It reads the loading screen's chant out of the disc's `default.xbe`, adds a clip of your disc's music to the game as the sound the PSP's menu plays on its entry, and puts the game and the maps together in one folder, ready for the stick.

**Use the bake bundle from the same release as the build zip.** A release says when its bundle changed and the maps must be baked again (b147h did: its maps carry more than b143's, so the game says to bake again, and stops, if it finds maps from b143's tool; `bake_changes.md`). b148 plays a bake made with b147h's bundle as it is; bake again with b148's bundle to see Master Chief's armour as b148 draws it.

**A second bake is faster.** The bake keeps the work it can reuse (the textures' palettes and the converted sounds, about 420 MB) in your user cache folder: `%LOCALAPPDATA%\halo-psp-bake` on Windows, `~/.cache/halo-psp-bake` on Linux. A bake that finds them takes about a third of the time. The first time, the guided bake asks whether to keep them. Deleting the folder is always safe; the next bake just makes them again.

1. **Download** from the release page, **[Sierra-III-Release's releases](https://github.com/Sheepdog-01/Sierra-III-Release/releases)**, the build zip, `halo_psp_b148.zip`, and the bake bundle for your system:

   | System | Bundle |
   |---|---|
   | Windows 10 or 11 | `halo-psp-bake-windows-b148.zip` |
   | Linux, glibc 2.39 or later | `halo-psp-bake-linux-b148.tar.gz` |

   Each has a `.sha256` beside it, which the bundle's SHA-256 must equal (Windows PowerShell: `Get-FileHash <file>`; macOS: `shasum -a 256 <file>`; Linux: `sha256sum <file>`).
2. **Unpack the bundle.** It makes one folder, `halo-psp-bake`. On Windows (right-click, *Extract All*), keep its path short: `C:\halo-psp-bake` always works, and the bake says if the path is too long.
3. **Put in your two files:**
   - your disc image (step 3) in `halo-psp-bake/1-PUT-YOUR-DISC-IMAGE-HERE`;
   - the build zip, as downloaded, in `halo-psp-bake/2-PUT-THE-HALO-ZIP-HERE`.
4. **Double-click the bake** in `halo-psp-bake`:
   - **Windows:** `Bake Halo.cmd`. If *Windows protected your PC* appears: *More info*, *Run anyway*.
   - **Linux:** `bake-halo.sh` (if asked, *Run*; or right-click, *Run as a Program*). If it only opens in an editor: `cd "$BAKE" && ./bake-halo.sh` in a terminal.
5. **Let it run.** A window shows what it found, then four stages:

   | Stage | What happens |
   |---|---|
   | `[1/4] Reading your disc` | every map read and checked (step 3) |
   | `[2/4] Setting up` | the first time only: the bake's Python libraries, about 30 MB (and a portable Python first, if the computer has none: it asks) |
   | `[3/4] Baking the maps` | the long part, several maps at once, with the time left. Leave the window open |
   | `[4/4] Putting it together` | the game, its menu sound and the maps into `COPY-TO-MEMORY-STICK` |

   It bakes the ten campaign levels and the menus (`ui`). **How long:** about 10 minutes on an 8-core PC, longer on fewer cores. **Disk:** it says how much it needs before it starts, and stops if the drive hasn't that much free (step 1).

**Check:** the window ends with

```
Done in N min.

Now copy the two folders inside COPY-TO-MEMORY-STICK (PSP and halo) to the top of your PSP's Memory Stick, merging them with the folders already there.
```

and `halo-psp-bake/COPY-TO-MEMORY-STICK` holds `PSP/GAME/halo/EBOOT.PBP`, `halo/chant.adp` and `halo/maps/`. Press Enter to close the window.

**If it stops,** it says why in one sentence, and the details are in `halo-psp-bake/bake-log.txt`:
- **A folder is empty, or holds two things:** put exactly one disc image in the first and the build zip in the second.
- **Hash mismatch:** *This disc is another version of Halo*, or *N of the disc's maps (...) aren't the retail ones*: the image is another version or region, a modified map, or a damaged dump. Make it again from your disc (step 3). The bake stops before anything long starts, and bakes nothing.
- **The path is too long for Windows,** or **not enough free space:** move the `halo-psp-bake` folder (to `C:\halo-psp-bake`, or a drive with more room) and double-click again.
- **The bake of a map failed:** report it (step 8), with `bake-log.txt`.

**Two notes that don't stop it** come at the end, when they apply:
- *The menu sound couldn't be added*: the game plays, but its entry in the PSP's menu is silent.
- **The chant:** *The loading screen's chant couldn't be read from your disc*: the game plays, but its loading screen is silent. Bake from the whole image, or from the folder that holds `default.xbe`.

`bake/README.md` (also in the bundle) has the rest: the command line, its options, and what's in the bundle.

If the maps on the stick were baked with another bundle, the game says so as it starts (**YOUR MAPS NEED BAKING AGAIN**, or **THESE MAPS ARE NEWER THAN THIS BUILD**) and names the bundle to use, instead of drawing them wrong. Bake again with this release's bundle.

The output is game data made from your disc. Keep it on your own machines.

## 5. Installing on the Memory Stick

You copy one folder's contents: `COPY-TO-MEMORY-STICK` holds the **game** (the build zip's files, with the menu sound added to its EBOOT) and the **bake** (`halo/maps` and the chant), laid out as the stick wants them.

1. Mount the stick (step 1) and set `STICK`.
2. **When you bake again for a new release,** delete the old maps first, so no stale pack is left behind: `rm -rf "$STICK/halo/maps"`.
3. **Copy the bake's folder** to the stick's root, merging (it's large, so it takes a while):

   ```sh
   cp -R "$BAKE/COPY-TO-MEMORY-STICK/." "$STICK/"
   ```

4. Unmount the stick (step 1, "When you're done").

**On Windows,** in File Explorer: open `COPY-TO-MEMORY-STICK` and drag the two folders it holds (`PSP` and `halo`) onto the stick's drive. Explorer merges them into the folders already there; answer "Replace the files" if it asks. **On macOS,** use the Terminal command above, not Finder: Finder can replace the stick's whole `PSP` folder, ARK-4's files with it.

**The stick, once installed:**

| Path | What | From |
|---|---|---|
| `PSP/GAME/halo/` | the game: `EBOOT.PBP` (with your menu sound), `halo_crash.prx`, `halo_profile.prx`, `halo_sleep.prx`, `LICENSE-mecore.md` | the build zip, through the bake |
| `halo/presets.txt` | the launch menu's presets (when the build has one) | the build zip, through the bake |
| `halo/maps/` | each map's `.map` and its packs (`.ptex`, `_track.ptex`, `.pgeo`, `.pstn`) | your bake |
| `halo/chant.adp` | the loading screen's chant | your bake |
| `halo/log/`, `halo/shots/`, `halo/save/`, `halo/crash.txt`, `halo/stderr.txt` | what the game writes | the game |
| `halo/keep.txt`, `halo/init.txt` | the launch menu's choices (step 6) | the launch menu |

**The stick rules:**
- Delete only inside `halo/` and `PSP/GAME/halo/`.
- **Leave ARK-4's files alone**, wherever they are (`SEPLUGINS/` and the files ARK-4's guide put on the stick), and `overconfig.txt` if your stick has one.
- **Never format or wipe the stick.**

**Check:** before you unmount,

```sh
cmp "$BAKE/COPY-TO-MEMORY-STICK/PSP/GAME/halo/EBOOT.PBP" "$STICK/PSP/GAME/halo/EBOOT.PBP" && echo "the build is on the stick"
ls "$STICK/halo/chant.adp" "$STICK/halo/maps/ui.map" "$STICK/halo/maps/a10.ptex"
```

prints `the build is on the stick` and lists the three files. On Windows, check that the stick holds `PSP\GAME\halo\EBOOT.PBP`, `halo\chant.adp` and `halo\maps\ui.map`.

## 6. First launch

1. On the PSP, open **Game → Memory Stick** and start **Sierra-III: Halo CE port** (the game's name in the list, beside the Sierra-III tile).
2. **The start-up.** The game shows a loading screen with a thin progress bar while it starts, then its menus. For the first 3 seconds the screen also says `Hold L as the game starts for the development menu` (the launch menu, below). The first launch of a new build takes a few seconds longer (the saved-game check).
3. **The main menu** is Halo's own, with the ring behind it. It answers once its textures are in. In Halo's menus the buttons are the pad's: cross is A, circle B, square X, triangle Y; the d-pad and the nub move.
4. **Starting a level.** Pick **Campaign** and a level. While it loads you hear the **chant**, as on the Xbox: four voices of the monk tone, swelling with the load. The loading screen stays at least 6 seconds, so the chant can swell as it does on the Xbox. Between levels the last view zooms out and fades. A level's first view stands still for up to 3 s while its textures arrive; then play starts.
5. **In play**, the PSP is one Xbox pad:

   | PSP | Halo |
   |---|---|
   | nub | look |
   | triangle / cross | move forward / back |
   | square / circle | strafe left / right |
   | R / L | fire / grenade |
   | d-pad up / down | jump / melee |
   | d-pad left | tap: reload; hold: action (vehicles, switches, weapons) |
   | d-pad right | switch weapon |
   | select (tap) | zoom |
   | select + up | flashlight |
   | select + down | crouch (toggles) |
   | select + L | switch grenades |
   | select + right | screenshot (to `halo/shots/`) |
   | start | pause menu |

   Halo's own control settings (presets, look sensitivity, invert) apply on top. The performance readout (frame rate and timings) is off; the launch menu's `overlay` word, or the in-game debug menu, turns it on.
6. **How fast:** see "What to expect", below. The heaviest stretches run slowly; this is the port's open work.
7. **To quit:** HOME → **ARK-4 Kommander** → **Exit**.

## What to expect

**Speed.** The port's aim is 30 frames a second everywhere; it isn't there yet. Measured on a PSP-1000 at its standard clock, over the second half of the first two minutes of each level (the stretch includes fights, so it is not a best case), on the build just before b143's final profile (b148 measures the same or faster):

| Level | Frames a second |
|---|---|
| *The Maw* (d40) | about 30 |
| *Assault on the Control Room* (b40) | about 29 |
| *Two Betrayals* (c40) | about 26 |
| *The Pillar of Autumn* (a10) | about 20 |
| *343 Guilty Spark* (c10) | about 17 |
| *Truth and Reconciliation* (a50) | about 16 |
| *Keyes* (d20) | about 14 |
| *Halo* (a30) | about 13 |
| *The Library* (c20) | about 13 |
| *The Silent Cartographer* (b30) | about 6 |

A PSP-3000 measures within 3% of these. On the lighter levels the game's own logic keeps close to its full 30 ticks a second; on the heaviest it falls behind too, so those play in slow motion as well as at a low frame rate.

**What's different from the Xbox**, as far as we know (the build notes list more):
- A slight missing polygon at *The Silent Cartographer*'s far horizon.
- The Pelican's canopy glass is lighter than on the PC, and its side panes stay dim.
- Some cutscene audio drifts from its picture, a few animations are cut short, and some lights on world geometry are missing.
- Multiplayer isn't included.

**What it is:** a work in progress, tested by one person on his own PSPs. Reports of crashes and visual faults are welcome (step 8).

**The power switch doesn't put the PSP to sleep while the game runs.** A sleep during play could crash the PSP, so the game holds sleep off until it's made safe. Quit to the XMB (HOME, then exit) first; sleep works there as usual. Holding the switch up for about 8 seconds still turns the PSP off, but quit first when you can.

**The launch menu** (optional; the game calls it the development menu). **Hold L as the game starts**, or put an empty file `halo/menu.txt` on the stick, and the game opens a plain text menu before it starts:
- **MAPS:** every map on the stick, and whether its texture pack is beside it (`NO TEXTURE PACK` means the bake of that map is incomplete). The chosen map starts straight away.
- **OPTIONS:** the everyday keep words (switches) as toggles.
- **PRESETS:** named sets of keep words, from `halo/presets.txt`.
- **Controls:** up/down move; left/right or cross toggle; L/R switch pages; START saves and starts the game.

It writes your choices to `halo/keep.txt` (keep words, separated by spaces) and `halo/init.txt` (the map). The next plain launch still goes to the main menu, with the keep words you chose. To go back to the defaults, delete both files. None is needed to play.

**Check:** the main menu answers the buttons, a level loads with the chant, and `halo/log/log.txt` on the stick starts with `Halo PSP bring-up build N`, the release's build number. If the load is silent, the log says `loading: the chant is silent: chant.adp isn't in the bake` (step 4, "The chant").

## 7. Multiplayer

**Multiplayer isn't part of this release.** The build here is the campaign. A multiplayer build is in development (README, "Multiplayer"); a later release will say when it's ready.

## 8. When it goes wrong

**The log.** The game rewrites `halo/log/log.txt` at every launch and keeps the last 20 launches' as `halo/log/NNN_log.txt`, so a missed copy can still be recovered. After a crash it also writes `halo/crash.txt`.

**A crash** shows a text screen that ends: `Saved to ms0:/halo/crash.txt (if the Memory Stick still answers). Take a photo of this screen, then hold POWER.` Take the photo, then hold POWER until the PSP turns off.

1. **Copy the log off** right after the run that went wrong, before you launch again:

   ```sh
   cd "$STICK/halo" && tar czf ~/sierra-log.tgz log $(ls crash.txt stderr.txt 2>/dev/null); cd ~
   ```

   On Windows, copy the stick's `halo\log` folder (and `halo\crash.txt` and `halo\stderr.txt`, if they're there) into a new folder, then right-click it and choose "Compress to ZIP file".
2. **Open an issue** on **[Sierra-III-Release's issues page](https://github.com/Sheepdog-01/Sierra-III-Release/issues)** with:
   - the build number (the log's first line) and which zip you installed;
   - your PSP model (1000, 2000 or 3000) and your ARK-4 version;
   - your PC's system (Windows, macOS or Linux), whether you baked with that release's bundle, and the bake's log (`bake-log.txt`, in the bundle's folder);
   - what you did, what you saw, and what you expected;
   - the log archive from step 1, the crash screen's photo if there was one, and a screenshot from `halo/shots/` if it shows the problem.
3. **Never attach game files:** nothing from `halo/maps/`, no `chant.adp`, nothing from your disc image or `COPY-TO-MEMORY-STICK`. `bake-log.txt` holds none.

**Common problems:**

| What you see | Cause | Fix |
|---|---|---|
| The game isn't in Game → Memory Stick | The zip isn't at the stick's root, or ARK-4 isn't running | Step 5's check; step 2 |
| The launch menu's MAPS page says `no maps in ms0:/halo/maps` | The bake isn't on the stick, or it's one folder too deep (`halo/halo/maps`, or `COPY-TO-MEMORY-STICK/halo/maps`) | Step 5.3 |
| A map says `NO TEXTURE PACK`, or a level never loads | That map's files didn't all reach the stick | Step 5 again (delete `halo/maps` on the stick, then copy); if it persists, bake again (step 4) |
| The loading screen is silent | No `chant.adp` on the stick | Step 4, "The chant" |
| The game goes straight into a level, holding L shows no launch menu, or it starts in an odd state | Old `halo/init.txt`, `keep.txt`, `input.txt` or `nomenu.txt` left on the stick (from testing) | Delete those four files |
