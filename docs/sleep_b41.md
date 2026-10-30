# B41: the power switch sleeps safely

b148 holds the power switch off while the game runs (`scePowerLock`), because
sleeping during play could crash while the firmware was writing its own
settings, and the PSP then reset its system settings (flash1: date and time,
nickname, network connections, theme) at the next start. This change lets the
switch sleep the PSP again, safely.

## How it works

- **`psp/sleep/main.c` (`halo_sleep.prx`)**, a small kernel module the game
  loads at start-up. Before anything sleeps, the firmware asks every driver
  whether it may (sysevent 0x100). The module answers "not yet" while the game
  isn't ready, and notes in the game's block that a sleep was asked for. It
  reads and writes only that block (user memory, its address given and checked
  at start) and lets every other event pass. It fails closed: a start without
  a valid block, or a handler the firmware won't take, answers non-zero, and
  the game keeps b148's switch held off.
- **`psp/src/psp_sleepguard.c`**, in the game. At the end of a frame that
  isn't in flight it sees the request, stops the Media Engine, drains the GE,
  gives back the volatile partition, marks itself ready and asks for the sleep
  itself (`scePowerRequestSuspend`). That request's query is answered yes.
- **On the wake**, the game stops being ready before anything is taken back
  (`psp_sleepguard_waking`), so a second query in that gap can't sleep the PSP
  with the partition in use again.
- **Every wait is bounded.** If the GE isn't idle at a frame end within 60
  frames, or a requested sleep doesn't come within 10 s, the game goes back to
  b148's stopgap for the rest of the session. The log says which.

## Tests

### On a PC: `tools/b41_host_test`

```bash
python tools/b41_host_test/run.py
```

It compiles `psp_sleepguard.c` and `psp/sleep/main.c` **unchanged** against stub
PSP headers and a model of the firmware, the GE, the ME and the caches, and
runs 21 scenarios, each in its own process: the module refusing or missing,
menus and play, a slow or hung GE, a clock wrap, a level load, a refused
request, a sleep that never comes, a late sleep, the wake's echo, two presses,
a press while the game is already asking, firmware-forced sleeps, and five
sleeps in a row. gcc (MinGW on Windows) is enough. It found one real hole
(the wake's gap above), now fixed; all 21 pass.

### On a PSP

**First, back up the PSP's system settings.** B41 is a settings corruption,
and a tester's custom theme may exist nowhere else: the PSP keeps its own copy
in flash1, often the only one left once the original `.ptf` file is gone.
Either way works, and the first is the one to rely on:

- **The custom firmware's USB flash1 mount** (ARK-4: the VSH menu's USB
  device, set to Flash 1): connect, copy every file to the PC, read only, and
  set the USB device back to Memory Stick afterwards.
- **RemotePilot PSP** (see `tools/remotepilot/README.md`): start it and press
  △ Back up. It copies flash1 into a new folder on the Memory Stick,
  `BACKUP/flash1-<date>-<time>`, reads every copy back, and writes a manifest
  (each file's size and SHA-256). Copy that folder to the PC by card reader or
  over Wi-Fi; RemotePilot's viewer checks each file against the manifest. Its
  □ Restore puts a backup back if a reset ever happens (it checks the backup
  and the firmware first, needs ✕ held for two seconds on the console, backs
  up the current settings, writes only what differs and restarts the PSP).
  The restore hasn't been run on hardware yet, so keep the PC copy too.

Then install the build in its own folder, keep the stock one, and copy
`halo/log/log.txt` off the stick after each step:

1. **The module loads (no sleeping).** Reach the first level and quit. The log
   has `power: ...halo_sleep.prx: started (..., its handler in)` and `power:
   the sleep switch works; each sleep waits for a quiet frame end`. If it says
   `not loaded` or `refused`, the game kept b148's stopgap: as safe as b148.
2. **One sleep in the main menu.** Slide the power switch **up** briefly (down
   is HOLD; held up for about three seconds it powers off). Expected: under a
   second's pause, then sleep. Wake it with the switch; the menu carries on.
   The log has `sleep: the switch asked`, `sleep: quiet in N us`, `sleep:
   awake (queries Q, refused R, allowed A)`. Then check the system settings
   (or compare flash1 with the backup).
3. **One sleep in play**, in *The Pillar of Autumn*, standing still: sleep,
   wake after ten seconds, play a minute, quit; settings again.
4. **Heavier spots:** five sleeps across *The Silent Cartographer* and a
   cutscene; settings after each.

### Results so far (PSP-1000, firmware 6.61, ARK-4)

- **Step 1:** passed. `halo_sleep.prx: started (0x0404326f, its handler in)`
  and `the sleep switch works`; played into *The Pillar of Autumn*, exited
  cleanly, no crash file.
- **Step 2:** passed. The switch's query was refused once (`query 00000040
  ...`), the game was quiet in 16.9 ms and asked for the sleep itself, then
  woke: `6 files reopened, 0 failed`, `queries 2, refused 1, allowed 1`. So the
  firmware asks again after a refusal and honours the game's own request.
  Exited cleanly, no crash file.
- **Steps 3 and 4:** to be filled in before this is merged.

## Known limits

- A sleep the firmware forces itself (a battery about to run flat) may not ask
  first. That case still takes the old path; afterwards no stale press is left.
- A sleep asked for during a level load waits until the load finishes.
- Holding the switch to power off may be read as a sleep request first; the
  log records each query's first words (`query ...`) so the two can be told
  apart. A long hold still powers off.
- After a press, the next frame runs without the Media Engine's frame overlap
  (as the menus do), for one frame, before the sleep.
- If the game itself hangs, the switch can't sleep the PSP (the module keeps
  answering "not yet" while the game isn't ready). A long hold of the switch
  still powers it off, as with b148. A heartbeat that lets the sleep through
  once the game has stopped answering would be a follow-up.

## Review

Two independent reviews of the design before any hardware run (the first found
a GE wait that could never finish: fixed by sleeping only at the end of a
frame that isn't in flight, with bounded waits), then the host test, then the
hardware steps above.
