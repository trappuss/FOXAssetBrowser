#!/usr/bin/env python3
"""qarpull.py — standalone SQAR reader, for pulling a handful of named files out
of a Fox Engine archive on a machine that cannot run the browser itself.

This is a development probe, not part of the shipped tool. It is a direct port
of src/fox/QarFile.cpp + the FPK reader, with one deliberate simplification: it
does not compute Fox path hashes. Instead it is handed a table of
"<51-bit path hash in hex>\t<name>" produced by

    FOXAssetBrowser --hashdump "<substring>=<out.tsv>"

so the hashing stays in one implementation rather than two.

  qarpull.py <hashes.tsv> <archive.dat> <outDir> <name-regex> [maxFiles] [ext.tsv]

Writes every matching entry to <outDir>/<assetPath>.<ext>, keeping the asset
directory structure so the result can be handed straight back to the browser as
a loose asset folder (--loose). FPK containers are expanded in place, each
member written at its own asset path. `ext.tsv` is the extension-code table from

    FOXAssetBrowser --hashdump "EXT=<out.tsv>"

without which entries fall back to a `.ext<code>` suffix.
"""
import os
import re
import struct
import sys
import zlib

XOR1, XOR2, XOR3, XOR4 = 0x41441043, 0x11C22050, 0xD05608C3, 0x532C7319
XORS = (XOR1, XOR2, XOR3, XOR4)
PATH_MASK = 0x3FFFFFFFFFFFF
MAGIC1, MAGIC2 = 0xA0F8EFE6, 0xE3F8EFE6

D1 = (0xBB8ADEDB, 0x65229958, 0x08453206, 0x88121302,
      0x4C344955, 0x2C02F10C, 0x4887F823, 0xF3818583)


def decrypt_sections(count, blob, version):
    out = []
    if version != 2:
        for i in range(count):
            o1, o2 = i * 8, i * 8 + 4
            i1 = struct.unpack_from('<I', blob, o1)[0] ^ XORS[(i + o1 // 5) % 4]
            i2 = struct.unpack_from('<I', blob, o2)[0] ^ XORS[(i + o2 // 5) % 4]
            out.append((i2 << 32) | i1)
    else:
        key = 0xA2C18EC3
        for i in range(count):
            o1, o2 = i * 8, i * 8 + 4
            s1 = struct.unpack_from('<I', blob, o1)[0]
            s2 = struct.unpack_from('<I', blob, o2)[0]
            i1 = s1 ^ XORS[(key + o1 // 5) % 4]
            i2 = s2 ^ XORS[(key + o2 // 5) % 4]
            out.append((i2 << 32) | i1)
            rot = (i2 // 256) % 19
            rotated = i1 if rot == 0 else (((i1 >> rot) | (i1 << (32 - rot))) & 0xFFFFFFFF)
            key ^= rotated
    return out


def decrypt1(data, version, hash_low, md5):
    seed = struct.unpack_from('<Q', md5, (hash_low % 2) * 8)[0]
    seed_lo, seed_hi = seed & 0xFFFFFFFF, (seed >> 32) & 0xFFFFFFFF
    buf = bytearray(data)
    blocks = len(buf) // 8
    for i in range(blocks):
        o = i * 8
        if version != 2:
            idx = 2 * ((hash_low + o // 11) % 4)
            m1, m2 = D1[idx], D1[idx + 1]
        else:
            idx = 2 * ((hash_low + seed + o // 11) % 4)
            m1, m2 = D1[idx] ^ seed_lo, D1[idx + 1] ^ seed_hi
        u1, u2 = struct.unpack_from('<II', buf, o)
        struct.pack_into('<II', buf, o, u1 ^ m1, u2 ^ m2)
    for i in range(len(buf) % 8):
        o = blocks * 8 + i
        ob = o - (o % 8)
        if version != 2:
            idx = 2 * ((hash_low + ob // 11) % 4)
            di = o % 8
            mask = D1[idx] if di < 4 else D1[idx + 1]
            buf[o] ^= (mask >> ((8 * di) & 31)) & 0xFF
        else:
            idx = 2 * ((hash_low + seed + ob // 11) % 4)
            di = o % 8
            mask = D1[idx] if di < 4 else D1[idx + 1]
            sm = seed_lo if di < 4 else seed_hi
            sh = 8 * (di % 4)
            buf[o] ^= ((mask >> sh) & 0xFF) ^ ((sm >> sh) & 0xFF)
    return bytes(buf)


def decrypt2(data, key):
    mul = (278 * key) & 0xFFFFFFFF
    blk = (key | ((key ^ 25974) << 16)) & 0xFFFFFFFF
    buf = bytearray(data)
    for o in range(0, (len(buf) // 4) * 4, 4):
        v = struct.unpack_from('<I', buf, o)[0] ^ blk
        struct.pack_into('<I', buf, o, v)
        blk = (mul + 48828125 * blk) & 0xFFFFFFFF
    return bytes(buf)


def read_fpk(blob):
    """Return [(memberPath, bytes)] for an FPK/FPKD blob, or [] if it is not one."""
    if len(blob) < 64 or blob[0:6] != b'foxfpk':
        return []
    off = 10 + 4 + 18
    (_two, count, refcount, _pad) = struct.unpack_from('<IIII', blob, off)
    off += 16
    out = []
    for _ in range(count):
        data_off, _a, data_size, _b = struct.unpack_from('<IIiI', blob, off)
        off += 16
        s_off, _c, s_len, _d = struct.unpack_from('<IIII', blob, off)
        off += 16
        off += 16   # md5
        if s_off + s_len > len(blob) or data_off + data_size > len(blob):
            break
        name = blob[s_off:s_off + s_len].decode('latin-1')
        out.append((name, blob[data_off:data_off + data_size]))
    return out


def read_pftxs(blob):
    """Return [(hash, bytes)] for the FTEX groups in a PFTXS pack, else [].

    Port of PftxsFile.cpp. UI textures are bundled into these packs rather than
    stored as loose QAR entries, so a puller that only knows QAR and FPK cannot
    see a single icon. Entries are keyed by the same 64-bit path hash the QAR
    uses, so they match the hash table directly.
    """
    if len(blob) < 32 or blob[0:4] != b'PFTX' or blob[16:20] != b'TEXL':
        return []
    out = []
    pos = 32
    while pos + 32 <= len(blob):
        base = pos
        if blob[pos:pos + 4] != b'FTEX':
            break
        # Three zero words are what tells a group header from a payload that
        # happens to start with the same magic.
        if struct.unpack_from('<III', blob, pos + 20) != (0, 0, 0):
            break
        ghash, count = struct.unpack_from('<Q', blob, pos + 8)[0], \
            struct.unpack_from('<I', blob, pos + 16)[0]
        if count == 0 or count > 64:
            break
        pos += 32
        entries = []
        for _ in range(count):
            if pos + 16 > len(blob):
                entries = []
                break
            h, off, size = struct.unpack_from('<QII', blob, pos)
            pos += 16
            entries.append((h, base + off, size))
        if not entries:
            break
        for h, off, size in entries:
            if off + size <= len(blob):
                out.append((h, blob[off:off + size]))
        # Group payloads follow the entry table; the next header starts after
        # the last entry's data.
        end = max(o + s for _, o, s in entries)
        pos = max(pos, end)
    return out


def write_asset(out_dir, name, ext, data):
    """Write one asset at its own path under out_dir, creating directories."""
    rel = name.lstrip('/').replace('\\', '/')
    if ext:
        rel += '.' + ext
    path = os.path.join(out_dir, *rel.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as o:
        o.write(data)


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    table_path, archive, out_dir, pattern = sys.argv[1:5]
    limit = int(sys.argv[5]) if len(sys.argv) > 5 else 400
    exts = {}
    if len(sys.argv) > 6:
        for line in open(sys.argv[6], encoding='utf-8', errors='replace'):
            c, _, e = line.rstrip('\n').partition('\t')
            if e:
                exts[int(c, 16)] = e
    scan = pattern in ('!SCAN', '!PACKS')
    scan_packs = pattern == '!PACKS'
    rx = re.compile('.' if scan else pattern, re.I)

    # `names` are the entries to OPEN; `all_names` is everything the table
    # knows, used to name the members of a container that itself matched. A
    # PFTXS pack is named for the folder, not for the textures inside it.
    names, all_names = {}, {}
    with open(table_path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            h, _, n = line.rstrip('\n').partition('\t')
            if not n:
                continue
            k = int(h, 16)
            all_names[k] = n
            if rx.search(n):
                names[k] = n
    print('targets: %d name(s) of %d known' % (len(names), len(all_names)),
          flush=True)
    if not names:
        return 0

    os.makedirs(out_dir, exist_ok=True)
    fh = open(archive, 'rb')
    head = fh.read(32)
    if struct.unpack_from('<I', head, 0)[0] != 0x52415153:
        print('not an SQAR:', archive)
        return 1
    flags = struct.unpack_from('<I', head, 4)[0] ^ XOR1
    count = struct.unpack_from('<I', head, 8)[0] ^ XOR2
    unknown = struct.unpack_from('<I', head, 12)[0] ^ XOR3
    version = struct.unpack_from('<I', head, 24)[0] ^ XOR1
    shift = 12 if (flags & 0x800) else 10
    print('%s: v%d, %d entries' % (os.path.basename(archive), version, count),
          flush=True)

    fh.seek(32)
    sections = decrypt_sections(count, fh.read(8 * count), version)

    # Chunked scanning: a full scan of a multi-gigabyte archive does not fit in
    # one run, so QARPULL_SKIP starts N sections in and QARPULL_MAX stops after
    # M of them. Progress is reported so the next chunk knows where to begin.
    skip = int(os.environ.get('QARPULL_SKIP', '0'))
    stop = skip + int(os.environ.get('QARPULL_MAX', '0') or len(sections))
    found = 0
    for si, sec in enumerate(sections):
        if si < skip:
            continue
        if si >= stop:
            print('stopped at section %d of %d' % (si, len(sections)), flush=True)
            break
        if found >= limit:
            break
        fh.seek((sec >> 40) << shift)
        hdr = fh.read(32)
        if len(hdr) != 32 or hdr == b'\0' * 32:
            continue
        hl = struct.unpack_from('<I', hdr, 0)[0] ^ XOR1
        hh = struct.unpack_from('<I', hdr, 4)[0] ^ XOR1
        h = (hh << 32) | hl
        name = names.get(h & PATH_MASK)
        if name is None:
            # Scan mode opens every entry and keeps only the container members
            # the table can name. A .pftxs bundle is named for its folder and
            # can sit inside an .fpk, so nothing about the OUTER entry's name
            # says the icons are in there — the only way to find them is to
            # look inside everything once.
            if not scan:
                continue
            # Only containers are worth opening blind. Decompressing every
            # texture and model in an 8 GB archive to look for one icon pack is
            # the difference between seconds and hours.
            want = ('pftxs',) if scan_packs else ('fpk', 'fpkd', 'pftxs')
            if exts.get(h >> 51) not in want:
                continue
            name = '__scan__/%013x' % (h & PATH_MASK)
        s1 = struct.unpack_from('<I', hdr, 8)[0] ^ XOR2
        s2 = struct.unpack_from('<I', hdr, 12)[0] ^ XOR3
        md5 = struct.pack('<IIII',
                          struct.unpack_from('<I', hdr, 16)[0] ^ XOR4,
                          struct.unpack_from('<I', hdr, 20)[0] ^ XOR1,
                          struct.unpack_from('<I', hdr, 24)[0] ^ XOR1,
                          struct.unpack_from('<I', hdr, 28)[0] ^ XOR2)
        usize, csize = (s1, s2) if version != 2 else (s2, s1)
        data = fh.read(csize)
        if len(data) != csize:
            continue
        data = decrypt1(data, version, hl, md5)
        if len(data) >= 8:
            m = struct.unpack_from('<I', data, 0)[0]
            if m in (MAGIC1, MAGIC2):
                key = struct.unpack_from('<I', data, 4)[0]
                data = decrypt2(data[8 if m == MAGIC1 else 16:], key)
        if usize != csize:
            try:
                data = zlib.decompress(data)
            except zlib.error:
                pass
        ext = exts.get(h >> 51, 'ext%d' % (h >> 51))
        members = read_fpk(data)
        packed = read_pftxs(data) if not members else []
        if members:
            inner = 0
            for mname, mdata in members:
                sub = read_pftxs(mdata)
                if sub:
                    for ph, pdata in sub:
                        pname = all_names.get(ph & PATH_MASK)
                        if pname:
                            write_asset(out_dir, pname,
                                        exts.get(ph >> 51, 'ftex'), pdata)
                            inner += 1
                    continue
                write_asset(out_dir, mname, None, mdata)
            print('  FPK %s -> %d member(s), %d packed texture(s)'
                  % (name, len(members), inner), flush=True)
        elif packed:
            wrote = 0
            for ph, pdata in packed:
                pname = names.get(ph & PATH_MASK) or all_names.get(ph & PATH_MASK)
                if not pname:
                    continue
                write_asset(out_dir, pname,
                            exts.get(ph >> 51, 'ftex'), pdata)
                wrote += 1
            if wrote:
                print('  PFTXS %s -> %d of %d entr(ies) named'
                      % (name, wrote, len(packed)), flush=True)
                # The community dictionary does not know every name a pack
                # contains — Survive's own eye and skincolor icon sets are in
                # it and not in the dictionary. Printing the hashes it could
                # not name lets a caller brute-force candidate names offline
                # instead of guessing one round-trip at a time.
                if os.environ.get('QARPULL_UNNAMED'):
                    for ph, _pd in packed:
                        if not (names.get(ph & PATH_MASK)
                                or all_names.get(ph & PATH_MASK)):
                            print('    UNNAMED %013x ext %d'
                                  % (ph & PATH_MASK, ph >> 51), flush=True)
            else:
                continue
        elif not name.startswith('__scan__/'):
            write_asset(out_dir, name, ext, data)
            print('  %s.%s (%d bytes)' % (name, ext, len(data)), flush=True)
        else:
            continue          # scan mode: an unnamed leaf is not the target
        found += 1
    print('extracted %d' % found, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
