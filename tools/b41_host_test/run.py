"""Build and run the B41 host test against this Sierra-III tree (or another).

    python tools/b41_host_test/run.py [--src <Sierra-III checkout>] [--cc gcc] [-v]

It compiles psp/src/psp_sleepguard.c and psp/sleep/main.c unchanged against
the stub headers in stubs/ and a model of the firmware, the GE, the ME and the
caches (b41_host_test.c), and runs each scenario in its own process. gcc
(MinGW on Windows) is enough: no PSP toolchain.

The test links at 0x08800000 so the module's address checks see a PSP-like
address (Windows: PE image base; Linux: -Ttext-segment). See b41_host_test.c.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENARIOS = [
    'module-rejects', 'no-module', 'register-fails', 'sleephold', 'sleepallowed',
    'play', 'menu', 'ge-slow', 'clock-wrap', 'load', 'ge-hung', 'ge-busy-after-me',
    'request-refused', 'no-sleep-comes', 'late-sleep', 'wake-echo', 'twice',
    'press-while-asking', 'forced', 'forced-after-press', 'five',
]
# psp_platform.c's power_callback, as b41_host_test.c mirrors it
SUSPEND_CALLS = ['psp_me_sleep_stop', 'psp_ge_inflight_room_suspend', 'psp_geo_suspend', 'psp_tex_suspend']
RESUME_CALLS = ['psp_tex_resume', 'psp_geo_resume', 'psp_ge_inflight_room_resume', 'psp_me_wake_request',
                'psp_sleepguard_woken']


def callback_calls(platform):
    text = platform.read_text(encoding='utf-8', errors='replace')
    match = re.search(r'static int power_callback\(.*?\n}\n', text, re.S)
    if not match:
        sys.exit('power_callback not found in %s' % platform)
    body = re.sub(r'/\*.*?\*/', '', match.group(0), flags=re.S)
    suspend, _, resume = body.partition('PSP_POWER_CB_RESUME_COMPLETE')
    names = lambda part: re.findall(r'\b(psp_[a-z_]+)\s*\(\s*\)\s*;', part)
    return names(suspend), names(resume), 'psp_sleepguard_quiet()' in suspend


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--src', default=HERE.parents[1], type=Path)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('-v', action='store_true')
    args = parser.parse_args()
    guard = args.src / 'psp' / 'src' / 'psp_sleepguard.c'
    module = args.src / 'psp' / 'sleep' / 'main.c'
    platform = args.src / 'psp' / 'src' / 'psp_platform.c'
    suspend, resume, gated = callback_calls(platform)
    waking = 'void psp_sleepguard_waking(void)' in guard.read_text(encoding='utf-8')
    expect_resume = (['psp_sleepguard_waking'] if waking else []) + RESUME_CALLS
    if suspend != SUSPEND_CALLS or resume != expect_resume or not gated:
        sys.exit('psp_platform.c power_callback no longer matches the test\'s mirror:\n  suspend %s\n  resume  %s'
                 % (suspend, resume))
    print('power_callback matches the mirror (%d + %d calls)' % (len(suspend), len(resume)))
    out = Path(tempfile.mkdtemp(prefix='b41-host-'))
    exe = out / ('b41_host_test.exe' if os.name == 'nt' else 'b41_host_test')
    link = (['-Wl,--image-base,0x08800000', '-Wl,--disable-dynamicbase', '-Wl,--disable-high-entropy-va']
            if os.name == 'nt' else ['-no-pie', '-Wl,-Ttext-segment=0x08800000'])
    command = [args.cc, '-std=gnu99', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
               '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast', '-I', str(HERE / 'stubs')]
    if waking:
        command.append('-DB41_WAKING')
    command += [str(HERE / 'b41_host_test.c'), str(guard), str(module), '-o', str(exe)] + link
    subprocess.run(command, check=True)
    failed = []
    for name in SCENARIOS:
        result = subprocess.run([str(exe), name] + (['-v'] if args.v else []), capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            failed.append(name)
    print('%d of %d scenarios passed' % (len(SCENARIOS) - len(failed), len(SCENARIOS)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
