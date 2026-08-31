#!/usr/bin/env python3
"""make_chara_tree.py — a stand-in TPP character tree for a container session.

    python3 tools/make_chara_tree.py _probe/pullhash.tsv <out-dir> [donor.fmdl]

The container's archive set holds no Phantom Pain character model at all — no
Snake, no Quiet, no soldier — so the character catalogue (PlayerCatalog's
buildTpp / buildTppCharacters) had nothing to build from and nothing to test
against. The install's own hash dump does carry the PATHS, and the catalogue
reads paths: it groups by the family code and buckets by the part token in the
stem, and never opens the file.

So: write every /Assets/tpp/chara/**/Scenes/*.fmdl path from the dump as a copy
of one small real model. 578 of them come to about 21 MB with a 33 KB donor.
Mount the result as the LOOSE folder beside the normal game folder:

    FOXAssetBrowser --game <uitest> --loose <out-dir> --playerdump out.tsv

What that verifies: the family split, the slot vocabulary, the labels, the
defaults, the ordering, the panel and every list in it. What it CANNOT show is
how any of it looks — every model is the same donor mesh. Say which of the two
you did when reporting a result.

Pass --patched to include the _patched twins as well; the catalogue folds them
and 8h fixed the place that did not, so a tree without them cannot catch that
class of bug again.
"""
import os, shutil, sys

def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    want_patched = "--patched" in sys.argv
    if len(args) < 2:
        print(__doc__)
        return 2
    dump, out = args[0], args[1]
    donor = args[2] if len(args) > 2 else None
    if not donor:
        print("give a donor .fmdl as the third argument "
              "(any small one — a weapon part does)")
        return 2
    n = 0
    for line in open(dump, encoding="utf-8", errors="replace"):
        path = line.rstrip("\n").split("\t")[-1]
        if "/Assets/tpp/chara/" not in path or "/Scenes/" not in path:
            continue
        stem = path.rsplit("/", 1)[1]
        if stem.endswith(".fmdl"):
            stem = stem[:-5]
        if stem.endswith("_patched") and not want_patched:
            continue
        # The avatar families are the one part of /chara/ the container already
        # has for real; overwriting them with a donor mesh would make the MGO
        # pages worse, not testable.
        if stem[:3] in ("avm", "avf"):
            continue
        d = os.path.join(out, path.rsplit("/", 1)[0].lstrip("/"))
        os.makedirs(d, exist_ok=True)
        shutil.copyfile(donor, os.path.join(d, stem + ".fmdl"))
        n += 1
    print(f"wrote {n} stand-in model(s) into {out}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
