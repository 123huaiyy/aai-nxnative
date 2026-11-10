#!/usr/bin/env python3
"""Assembles the game's IPA on Windows from nxnative's iOS app built in CI with placeholder modules (no game data, ci/build_shell.py) and the
parts made on this machine from the user's own game files: the rewritten modules (written into the executable's placeholder sections, as
gen_embed.py lays them out), the rewriter and shader tables, the launcher settings, the iOS shader library, the RomFS identity (or the RomFS
itself), the game's name and icon.

Usage: python -I -X utf8 aai_pack.py --nxnative <nxnative> --shell <DmIOS-shell.zip | DmIOS.app> --patched <patched dir> --profile <profile dir>
         --msl-lib <b-<chain>.img> --romfs <RomFS.img> --nacp <control.nacp> --launcher <launcher.json> --icons <dir> --out <out.ipa>
         [--romfs-mode separate|bundle] [--opmode 0|1] [--version 1.0.0] [--package v65536]
"""
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import stat
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

PG = 0x4000
LC_SEGMENT_64, LC_CODE_SIGNATURE = 0x19, 0x1d
CPU_ARM64 = 0x0100000c


def r16(x):
    return (x + PG - 1) & ~(PG - 1)


def pieces(patched, n):
    """The module's pieces (x0, W, x1[, x2]) as (file start, end) of <n>.patched.bin: gen_embed.py's pieces()"""
    hdr = dict(l[2:].split() for l in open(patched / f'{n}.fixups.tsv', encoding='utf-8'))
    te, po = int(hdr['TEXT_END'], 16), int(hdr['PATCH_OFF'], 16)
    ko, ks = int(hdr['KEY_PAGE_OFF'], 16), int(hdr['KEY_PAGE_SIZE'], 16)
    bin_ = patched / f'{n}.patched.bin'
    sz = bin_.stat().st_size
    a, end = r16(te), r16(sz)
    if po < 0:
        fl, bl = int(hdr['FILE_IMAGE_LEN'], 16), int(hdr.get('BACK_LEN', '0'), 16)
        assert fl % PG == 0 and a <= fl and ko % PG == 0 and ks == PG and ko >= fl and bl % PG == 0 and (not bl or sz % PG == 0)
        return bin_, sz, ((0, a), (a, fl), (fl, end - bl)) + (((end - bl, end),) if bl else ())
    assert po % PG == 0 and a <= ko and ko + ks <= po and ko % PG == 0 and ks == PG
    return bin_, sz, ((0, a), (a, po), (po, end))


def layout(patched, names):
    """What CI's build_shell.py needs for the placeholders: per module the patched.bin size and the fixups header's layout keys (numbers only)"""
    out = []
    for n in names:
        hdr = dict(l[2:].split() for l in open(patched / f'{n}.fixups.tsv', encoding='utf-8'))
        keep = {k: v for k, v in hdr.items() if k in ('PATCH_OFF', 'KEY_PAGE_OFF', 'KEY_PAGE_SIZE', 'TEXT_END', 'IMAGE_SIZE', 'FILE_IMAGE_LEN', 'BACK_OFF', 'BACK_LEN')}
        out.append({'name': n, 'size': (patched / f'{n}.patched.bin').stat().st_size, 'header': keep})
    return out


def arm64_slice(f):
    """(offset, size) of the arm64 Mach-O in the file (a thin file is its own slice)"""
    f.seek(0)
    head = f.read(8)
    if struct.unpack('<I', head[:4])[0] == 0xfeedfacf:
        f.seek(0, 2)
        return 0, f.tell()
    magic, n = struct.unpack('>II', head)
    if magic not in (0xcafebabe, 0xcafebabf):
        raise ValueError('not a Mach-O executable')
    for i in range(n):
        if magic == 0xcafebabe:
            f.seek(8 + 20 * i)
            cpu, _, off, size, _ = struct.unpack('>iiIII', f.read(20))
        else:
            f.seek(8 + 32 * i)
            cpu, _, off, size, _, _ = struct.unpack('>iiQQII', f.read(32))
        if cpu == CPU_ARM64:
            return off, size
    raise ValueError('no arm64 slice')


def macho(f, base):
    """{(segment, section): (file offset in the slice, size, flags)} and the code signature's (offset, size) or None"""
    f.seek(base)
    hdr = f.read(32)
    magic, _, _, _, ncmds, sizeofcmds, _, _ = struct.unpack('<IiiIIIII', hdr)
    if magic != 0xfeedfacf:
        raise ValueError('not a 64-bit Mach-O')
    cmds = f.read(sizeofcmds)
    off, secs, cs = 0, {}, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', cmds, off)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', cmds, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                sect = cmds[so:so + 16].rstrip(b'\0').decode()
                seg = cmds[so + 16:so + 32].rstrip(b'\0').decode()
                _, ssize, soff, _, _, _, sflags = struct.unpack_from('<QQIIIII', cmds, so + 32)
                secs[(seg, sect)] = (soff, ssize, sflags)
                so += 80
        elif cmd == LC_CODE_SIGNATURE:
            cs = struct.unpack_from('<II', cmds, off + 8)
        off += size
    return secs, cs


def write_modules(exe, patched, names):
    with open(exe, 'r+b') as f:
        base, _ = arm64_slice(f)
        secs, cs = macho(f, base)
        for n in names:
            bin_, sz, parts = pieces(patched, n)
            want = [('__TEXT', f'__dm{n}x0'), (f'__DM{n}W', '__img'), ('__TEXT', f'__dm{n}x1'), ('__TEXT', f'__dm{n}x2')]
            with open(bin_, 'rb') as src:
                for key, (lo, hi) in zip(want, parts):
                    if key not in secs:
                        sys.exit(f'[RED] the shell app has no section {key[0]},{key[1]}: it was built for another layout')
                    soff, ssize, sflags = secs[key]
                    if ssize != hi - lo:
                        sys.exit(f'[RED] section {key[0]},{key[1]} is {ssize:#x} bytes, the module piece {hi - lo:#x}: build the shell again with this layout')
                    if (sflags & 0xff) == 1 or soff == 0:
                        sys.exit(f'[RED] section {key[0]},{key[1]} has no file content (zerofill)')
                    src.seek(lo)
                    data = src.read(max(0, min(hi, sz) - lo))
                    f.seek(base + soff)
                    f.write(data)
                    f.write(bytes(ssize - len(data)))
                    print(f'  {n}: {key[0]},{key[1]} {ssize:#x} bytes at {soff:#x} ({len(data):#x} from the module)')
        return base, cs


def rehash_signature(exe, base, cs, info_plist=None):
    """Recompute the page hashes of every code directory of the executable's (ad-hoc / linker) signature after its pages changed"""
    if not cs:
        print('  executable has no code signature (the install tool signs it)')
        return
    with open(exe, 'r+b') as f:
        f.seek(base + cs[0])
        blob = bytearray(f.read(cs[1]))
        magic, _, count = struct.unpack_from('>III', blob, 0)
        if magic != 0xfade0cc0:
            print('  unknown signature format: left as it is (the install tool re-signs it)')
            return
        for i in range(count):
            _, boff = struct.unpack_from('>II', blob, 12 + 8 * i)
            if struct.unpack_from('>I', blob, boff)[0] != 0xfade0c02:
                continue
            (_, _, version, _, hash_off, _, n_special, n_code, code_limit, hash_size, hash_type, _, page_log2, _) = struct.unpack_from('>IIIIIIIIIBBBBI', blob, boff)
            algo = {1: 'sha1', 2: 'sha256', 3: 'sha256', 4: 'sha384'}[hash_type]
            if version >= 0x20300:
                limit64 = struct.unpack_from('>Q', blob, boff + 56)[0]
                code_limit = limit64 or code_limit
            page = 1 << page_log2 if page_log2 else code_limit
            for k in range(n_code):
                f.seek(base + k * page)
                data = f.read(min(page, code_limit - k * page))
                h = hashlib.new(algo, data).digest()[:hash_size]
                at = boff + hash_off + k * hash_size
                blob[at:at + hash_size] = h
            if info_plist is not None and n_special >= 1:
                at = boff + hash_off - hash_size
                if any(blob[at:at + hash_size]):
                    blob[at:at + hash_size] = hashlib.new(algo, info_plist).digest()[:hash_size]
            print(f'  code directory ({algo}, {n_code} pages of {page:#x}) rehashed')
        f.seek(base + cs[0])
        f.write(blob)


def nacp_title(path, index):
    b = Path(path).read_bytes()
    return b[index * 0x300:index * 0x300 + 0x200].split(b'\0', 1)[0].decode('utf-8', 'replace').strip()


def unpack_shell(shell, stage):
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'Payload').mkdir(parents=True)
    if shell.is_dir():
        shutil.copytree(shell, stage / 'Payload/DmIOS.app')
    else:
        with zipfile.ZipFile(shell) as z:
            for i in z.infolist():
                name = i.filename
                if name.startswith('__MACOSX/') or '/._' in name or name.startswith('._'):
                    continue
                if not name.startswith('DmIOS.app/'):
                    sys.exit(f'[RED] unexpected entry in the shell archive: {name}')
                if (i.external_attr >> 16) & 0o170000 == stat.S_IFLNK:
                    sys.exit(f'[RED] symlink in the shell app: {name}')
                z.extract(i, stage / 'Payload')
    return stage / 'Payload/DmIOS.app'


def zip_tree(out, stage, exe_arc, store_suffixes):
    tmp = out.with_name(out.name + '.partial')
    files = []
    for root, dirs, names in os.walk(stage):
        dirs.sort()
        for n in sorted(names):
            files.append(Path(root) / n)
    total = sum(p.stat().st_size for p in files)
    print(f'pack: {len(files)} files, {total / 1e6:.1f} MB')
    with zipfile.ZipFile(tmp, 'w', allowZip64=True) as z:
        seen = set()
        for p in files:
            rel = p.relative_to(stage).as_posix()
            parts = rel.split('/')
            for i in range(1, len(parts)):
                d = '/'.join(parts[:i]) + '/'
                if d not in seen:
                    seen.add(d)
                    zi = zipfile.ZipInfo(d, date_time=(1980, 1, 1, 0, 0, 0))
                    zi.create_system, zi.external_attr = 3, (stat.S_IFDIR | 0o755) << 16
                    z.writestr(zi, b'')
            st = p.stat()
            zi = zipfile.ZipInfo(rel, date_time=time.localtime(max(st.st_mtime, 315532800))[:6])
            zi.create_system = 3
            zi.external_attr = (stat.S_IFREG | (0o755 if rel == exe_arc else 0o644)) << 16
            zi.compress_type = zipfile.ZIP_STORED if p.name.lower().endswith(store_suffixes) else zipfile.ZIP_DEFLATED
            with open(p, 'rb') as src, z.open(zi, 'w', force_zip64=st.st_size >= 0xFFFFFFFF) as dst:
                shutil.copyfileobj(src, dst, 16 << 20)
    os.replace(tmp, out)
    print(f'IPA: {out} ({out.stat().st_size / 1e6:.1f} MB)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nxnative', type=Path, required=True)
    ap.add_argument('--shell', type=Path)
    ap.add_argument('--patched', type=Path, required=True)
    ap.add_argument('--profile', type=Path, required=True)
    ap.add_argument('--msl-lib', type=Path)
    ap.add_argument('--romfs', type=Path)
    ap.add_argument('--nacp', type=Path)
    ap.add_argument('--launcher', type=Path)
    ap.add_argument('--icons', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--stage', type=Path)
    ap.add_argument('--romfs-mode', choices=('separate', 'bundle'), default='separate')
    ap.add_argument('--opmode', choices=('0', '1'))
    ap.add_argument('--version', default='')
    ap.add_argument('--package', default='')
    ap.add_argument('--print-layout', action='store_true', help='print the layout JSON CI needs (numbers only) and stop')
    a = ap.parse_args()

    sys.path.insert(0, str(a.nxnative / 'dm/games'))
    import game
    g = game.Game(a.profile)
    names = g.words('modules')
    if a.print_layout:
        print(json.dumps({'game': g.id, 'modules': layout(a.patched, names)}, indent=1))
        return

    stage = a.stage or a.out.with_suffix('.stage')
    app = unpack_shell(a.shell, stage)
    info_path = app / 'Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    exe = app / info['CFBundleExecutable']
    shell = json.loads(Path(a.shell).with_name('shell.json').read_text()) if Path(a.shell).with_name('shell.json').exists() else {}
    chain = info.get('DMChain') or shell.get('chain')
    print(f'shell app: {info.get("CFBundleIdentifier")}, nxnative {info.get("DMCommit")}, chain {chain}')

    # 1. the rewritten modules into the executable's placeholder sections
    print('modules:')
    base, cs = write_modules(exe, a.patched, names)

    # 2. tables: the rewriter's, the shader tables and the launcher settings (build_ipa.sh program_tables)
    td = app / 'dm'
    for p in list(td.iterdir()):
        if p.is_file() and (p.name == 'PLACEHOLDER.txt' or p.name.endswith(('.fixups.tsv', '.gates.tsv', '.cuts.tsv', '.obs.tsv'))):
            p.unlink()
    for n in names:
        for kind in ('fixups', 'gates', 'cuts', 'obs'):
            shutil.copyfile(a.patched / f'{n}.{kind}.tsv', td / f'{n}.{kind}.tsv')
    if (a.patched / 'nro.tsv').exists():
        shutil.copyfile(a.patched / 'nro.tsv', td / 'nro.tsv')
    for t in ('shader_map.tsv', 'pso_manifest.tsv', 'c1_full.tsv'):
        shutil.copyfile(a.profile / t, td / t)
    for t in ('cb_extent.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv'):
        if (a.profile / t).exists():
            shutil.copyfile(a.profile / t, td / t)
    launcher = json.loads(a.launcher.read_text(encoding='utf-8'))
    if a.opmode is not None:
        for s in launcher['settings']:
            if s['env'] == 'DM_OPMODE':
                s['default'] = a.opmode
    (td / 'launcher.json').write_text(json.dumps(launcher, ensure_ascii=False, indent=1), encoding='utf-8')
    lang = next((s['default'] for s in launcher['settings'] if s['env'] == 'DM_LANG'), 'en-US')

    # 3. the iOS shader library, as one file (msl/b-<chain>.img)
    if a.msl_lib:
        if chain and a.msl_lib.name != f'b-{chain}.img':
            sys.exit(f'[RED] shader library {a.msl_lib.name} is not the shell host\'s chain b-{chain}')
        (app / 'msl').mkdir(exist_ok=True)
        shutil.copyfile(a.msl_lib, app / 'msl' / a.msl_lib.name)

    # 4. the game files: their identity only (Documents/romfs.img on the phone), or the image itself
    if a.romfs_mode == 'separate':
        ident = subprocess.run([sys.executable, '-I', '-X', 'utf8', str(a.nxnative / 'dm/tools/romfs_image.py'), '--identity', str(a.romfs), g.id,
                                a.version or info.get('CFBundleShortVersionString', ''), a.package], capture_output=True, text=True, check=True).stdout
        (app / 'romfs.id').write_text(ident, encoding='ascii')
        print(f'game files identity: {ident.strip()}')
    else:
        shutil.copyfile(a.romfs, app / 'romfs.img')

    # 5. name, version, language and icon
    name_zh = nacp_title(a.nacp, 14) if a.nacp else ''
    name = name_zh or (nacp_title(a.nacp, 0) if a.nacp else '') or info.get('CFBundleDisplayName')
    info['CFBundleDisplayName'] = name
    if a.version:
        info['CFBundleShortVersionString'] = info['CFBundleVersion'] = a.version
    info['DMLanguage'] = lang
    if a.icons and a.icons.is_dir():
        for p in a.icons.glob('*.png'):
            shutil.copyfile(p, app / p.name)
        info['CFBundleIcons'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60'], 'CFBundleIconName': 'AppIcon'}}
        info['CFBundleIcons~ipad'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60', 'AppIcon76x76', 'AppIcon83.5x83.5'], 'CFBundleIconName': 'AppIcon'}}
    data = plistlib.dumps(info, fmt=plistlib.FMT_BINARY)
    info_path.write_bytes(data)
    print(f'app: {name} {info.get("CFBundleShortVersionString")} ({info.get("CFBundleIdentifier")}), language {lang}')

    # 6. the executable's signature over its new pages, then the archive
    rehash_signature(exe, base, cs, data)
    store = tuple(s.lower() for s in g.get('zip_store', '').split(':') if s) + ('.png',)
    zip_tree(a.out, stage, f'Payload/DmIOS.app/{exe.name}', store)


if __name__ == '__main__':
    main()
