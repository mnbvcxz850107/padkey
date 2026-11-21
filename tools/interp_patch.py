"""Rewrite the ELF interpreter string in place (same length or shorter, NUL padded).
Unlike patchelf this never changes the file layout, so non-PIE binaries stay valid.
usage: interp_patch.py OLD NEW DIR... """
import os, struct, sys
old, new, dirs = sys.argv[1].encode(), sys.argv[2].encode(), sys.argv[3:]
assert len(new) <= len(old), "new interpreter path must not be longer"
seen, n = set(), 0
for d in dirs:
    for root, _, files in os.walk(d):
        for name in files:
            p = os.path.realpath(os.path.join(root, name))
            if p in seen or not os.path.isfile(p):
                continue
            seen.add(p)
            with open(p, "r+b") as f:
                h = f.read(64)
                if h[:4] != b"\x7fELF" or h[4] != 2:
                    continue
                phoff, = struct.unpack_from("<Q", h, 32)
                phentsize, phnum = struct.unpack_from("<HH", h, 54)
                for i in range(phnum):
                    f.seek(phoff + i * phentsize)
                    ptype, _, off, _, _, filesz = struct.unpack("<IIQQQQ", f.read(40))
                    if ptype == 3:  # PT_INTERP
                        f.seek(off)
                        cur = f.read(filesz).rstrip(b"\0")
                        if cur == old:
                            f.seek(off)
                            f.write(new + b"\0" * (filesz - len(new)))
                            n += 1
print("patched", n)
