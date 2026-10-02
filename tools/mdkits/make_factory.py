#!/usr/bin/env python3
"""Makes Machinemodule's factory data from the user's own MD OS: the factory kits (FACTORY.syx) and the ROM
machines' sample memory (ROM_SAMPLES.bin), both installed to <data dir>/factory.

Both come from booting the emulated MD (tools/mdtrace/mdProbe) from the user's flash image. The factory kits are made
by the OS itself the first time it initialises an empty flash: mdProbe's kitdump action asks for each of the 64 kits
over sysex (the MD's own kit request $53), and the named ones are kept (empty slots have a $7F name). The ROM samples
are what the MD copies from its sample flash into the voice DSP at boot: mdProbe's dspdump action dumps that DSP's
memory and tools/mdkits/mdsamples keeps the sample directory and data. Both are Elektron's data from the user's own
files: per-user build output, never committed or distributed.

    make_factory.py <mdProbe> <mdsamples> <MD flash image .bin> <MD OS .syx> <out dir>
"""
import os
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2
    probe, mdsamples, rom, os_syx, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        dump = os.path.join(tmp, "dsp2_p.bin")
        subprocess.run([probe, rom, os.path.join(tmp, "flash.bin"), "kitdump:" + tmp, "dspdump:1:0:800000:" + dump],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kits = []
        for slot in range(64):
            path = os.path.join(tmp, "kit_%02d.syx" % slot)
            if os.path.exists(path):
                data = open(path, "rb").read()
                if len(data) >= 0x4d1 and data[0x0a] not in (0x00, 0x7f):   # a named kit, not an empty slot
                    kits.append(data)
        if not kits:
            print("no kits dumped", file=sys.stderr)
            return 1
        open(os.path.join(out, "FACTORY.syx"), "wb").write(b"".join(kits))
        print("%s: %d factory kits" % (os.path.join(out, "FACTORY.syx"), len(kits)), file=sys.stderr)
        subprocess.run([mdsamples, os_syx, dump, os.path.join(out, "ROM_SAMPLES.bin")], check=True,
                       stdout=subprocess.DEVNULL)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
