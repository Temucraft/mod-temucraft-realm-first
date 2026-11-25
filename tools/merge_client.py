#!/usr/bin/env python3
"""Writes a copy of a release patch with this module's client files added (it never changes the patch itself).

  python merge_client.py "F:\\TemuCraft Server\\5) Release\\patches\\patch-T.MPQ" patch-T.new.MPQ

Every file of the patch is kept as it is; DBFilesClient\\Achievement.dbc is added (or replaced) with
client/patch-T.MPQ/DBFilesClient/Achievement.dbc. Needs "F:\\TemuCraft Server\\_tools" (mpqtool3.py, hd\\mpqwrite.py):
found from this file's place in "1) Source\\modules", or give --tools DIR.
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
CLIENT = os.path.join(MOD, 'client', 'patch-T.MPQ')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('patch')
    ap.add_argument('out')
    ap.add_argument('--tools', default=os.path.normpath(os.path.join(MOD, '..', '..', '..', '_tools')))
    a = ap.parse_args()
    if os.path.abspath(a.out) == os.path.abspath(a.patch):
        raise SystemExit('write to a new file, then swap it in yourself (keep a backup of the old patch)')
    sys.path.insert(0, a.tools)
    sys.path.insert(0, os.path.join(a.tools, 'hd'))
    import mpqtool3
    from mpqwrite import write_mpq

    files = {}
    for name in mpqtool3.read_file(a.patch):
        if not name or name == '(listfile)':
            continue
        data = mpqtool3.read_file(a.patch, name)
        if data is None:
            raise SystemExit(f'{name} is in the listfile but could not be read')
        files[name] = data
    added = []
    for root, _, names in os.walk(CLIENT):
        for n in names:
            rel = os.path.relpath(os.path.join(root, n), CLIENT).replace('/', '\\')
            files[rel] = open(os.path.join(root, n), 'rb').read()
            added.append(rel)
    write_mpq(a.out, files)
    print(f'{a.out}: {len(files)} files ({", ".join(added)} added or replaced)')


if __name__ == '__main__':
    main()
