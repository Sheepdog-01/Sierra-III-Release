"""installer - the bake with a window: choose your game and your two files,
bake, and copy the result to the PSP's Memory Stick, checked file by file.

    "Install to PSP"          the window (launcher.py --installer, with the
                              Python the bake uses)
    python -m installer       the same, from the bundle's folder

It runs the guided bake (guided.py) unchanged, which also sets up its .venv
and its cache as it always does; the installer itself writes nothing but its
output folder (out) and, when asked, the Memory Stick."""
import sys


def main():
    try:
        from . import app
    except ImportError as e:
        # some Linux systems' Python comes without Tk (python3-tk)
        print('The installer window needs Tk, which this Python lacks (%s). "Bake Halo" bakes without a window; '
              'then copy COPY-TO-MEMORY-STICK to the stick as the README says.' % e)
        # double-clicked, the console would close before it could be read
        if sys.stdin and sys.stdin.isatty():
            try:
                input('\nPress Enter to close this window.')
            except (EOFError, KeyboardInterrupt):
                pass
        return 1
    return app.main()
