"""PSP: Halo CE (Sierra-III) over RP2, with the PSP section's read-only folders.

The device side is psp/device (rp2_device.c, rp2_actions.c); on the PSP it
runs in the RemotePilot PSP companion or a Sierra-III build with the
integration. Pairing writes ms0:/remotepilot/remotepilot.cfg, which the device never
lists or sends."""
from reference_adapter import ReferenceAdapter


def create_adapter(local_dir, manifest):
    return ReferenceAdapter(local_dir, manifest)
