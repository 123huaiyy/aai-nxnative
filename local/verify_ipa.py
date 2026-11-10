# Checks the packed IPA: the embedded modules are the rewritten ones, the tables are there and name the shader library's stamp, the library is the host's chain.
import hashlib
import io
import plistlib
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, r'E:\aai\scripts')
sys.path.insert(0, r'E:\aai\nxnative\dm\tools')
import aai_pack as P
import romfs

ipa, patched = sys.argv[1], Path(sys.argv[2])
z = zipfile.ZipFile(ipa)
app = 'Payload/DmIOS.app/'
info = plistlib.loads(z.read(app + 'Info.plist'))
chain = info['DMChain']
exe = z.read(app + info['CFBundleExecutable'])
f = io.BytesIO(exe)
base, _ = P.arm64_slice(f)
secs, _ = P.macho(f, base)
ok = True
for n in ('main', 'subsdk0', 'sdk'):
    bin_, sz, parts = P.pieces(patched, n)
    data = bin_.read_bytes()
    for key, (lo, hi) in zip([('__TEXT', f'__dm{n}x0'), (f'__DM{n}W', '__img'), ('__TEXT', f'__dm{n}x1')], parts):
        off, size, _ = secs[key]
        want = data[lo:min(hi, sz)]
        ok &= exe[off:off + size] == want + bytes(size - len(want))
print('modules embedded:', 'OK' if ok else 'MISMATCH')
names = set(z.namelist())
need = ['dm/launcher.json', 'dm/shader_map.tsv', 'dm/pso_manifest.tsv', 'dm/c1_full.tsv', 'dm/cb_extent.tsv', 'dm/shader_pairs.tsv', 'dm/font/standard.ttf',
        'dm/font/nintendo_ext.ttf', f'msl/b-{chain}.img', 'romfs.id', 'AppIcon60x60@2x.png'] + [f'dm/{n}.{k}.tsv' for n in ('main', 'subsdk0', 'sdk') for k in ('fixups', 'gates', 'cuts', 'obs')]
missing = [n for n in need if app + n not in names]
print('files:', 'OK' if not missing else f'MISSING {missing}')
crlf = [n for n in names if n.startswith(app + 'dm/') and n.endswith(('.tsv', '.json')) and b'\r\n' in z.read(n)]
print('line endings:', 'LF' if not crlf else f'CRLF in {crlf}')
tmp = Path(r'E:\aai\work\pkg\verify-msl.img')
tmp.write_bytes(z.read(app + f'msl/b-{chain}.img'))
lib = romfs.Path(str(tmp))
stamp = dict(l.split('\t', 1) for l in (lib / 'inputs.tsv').read_text().splitlines() if '\t' in l and not l.startswith('#'))
want_head = f'# chain b-{chain} inputs {stamp["inputs"]} '
for t in ('c1_full.tsv', 'cb_extent.tsv'):
    head = z.read(app + 'dm/' + t).decode().partition('\n')[0]
    print(f'{t} names the library stamp:', 'OK' if head.startswith(want_head) else f'NO ({head[:80]})')
files = list(lib.iterdir())
print(f'shader library b-{chain}: game {stamp.get("game")}, {len(files)} files, metallibs {sum(1 for p in files if p.name.endswith(".metallib"))}')
bad = [p.name for p in files if p.name.endswith('.metallib') and p.read_bytes()[:4] != b'MTLB']
print('metallib headers:', 'OK' if not bad else f'BAD {bad[:5]}')
tmp.unlink()
print('display name:', info['CFBundleDisplayName'], '| version', info['CFBundleShortVersionString'], '| language', info['DMLanguage'], '| min iOS', info['MinimumOSVersion'])
print('romfs.id:', z.read(app + 'romfs.id').decode().strip())
