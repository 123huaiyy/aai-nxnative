# Runs nxnative's iOS shader build (dm/gfx/tools/dmsh_build.py, unchanged) on Windows through aai_xcrun.py.
# Usage: python -I -X utf8 aai_shaders.py --metal-config <json> [--dry-run] [--pch 0|1] [-j N]
#   --metal-config: the Windows Metal tools for aai_xcrun.py (sdks.iphoneos / sdks.macosx: metal, metallib, metal_args, sdk_version ...)
#   --dry-run: fake_metal.py instead of a compiler, own output and cache folders (to check the translation chain on Windows)
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(r'E:\aai')
NX = ROOT / 'nxnative'
W = ROOT / 'work'

ap = argparse.ArgumentParser()
ap.add_argument('--metal-config', type=Path)
ap.add_argument('--dry-run', action='store_true')
ap.add_argument('--pch', default='1')
ap.add_argument('-j', default=str(max(1, (os.cpu_count() or 4) - 2)))
ap.add_argument('--spirv-cross', type=Path, default=ROOT / 'build/win-tools/spirv-cross/spirv-cross.exe')
ap.add_argument('keys', nargs='*')
a = ap.parse_args()

out_root = W / ('dryrun' if a.dry_run else 'shader')
cfg_path = out_root / 'xcrun.json'
out_root.mkdir(parents=True, exist_ok=True)
if a.dry_run:
    fake = [sys.executable, '-I', str(ROOT / 'scripts/fake_metal.py')]
    sdk = {'metal': fake[0], 'metal_args': fake[1:] + ['metal'], 'metallib': fake[0], 'metallib_args': fake[1:] + ['metallib'],
           'sdk_path': 'C:/fake-sdk', 'sdk_version': '18.0', 'sdk_build': 'fake'}
    cfg = {'nxnative': str(NX), 'sdks': {'iphoneos': sdk, 'macosx': sdk}}
else:
    cfg = json.loads(a.metal_config.read_text(encoding='utf-8'))
    cfg['nxnative'] = str(NX)
cfg_path.write_text(json.dumps(cfg, indent=1), encoding='utf-8')

env = dict(os.environ, DM_GAME='010005501E68C000', DMSH_TARGETS='ios', IOSOUT=str(out_root / 'ios'),
           XLATE=str(ROOT / 'build/win-tools/dmsh_xlate/dmsh_xlate.exe'), SPIRV_CROSS=str(a.spirv_cross), DOTNET_ROOT=str(ROOT / 'dotnet'),
           AAI_XCRUN=str(cfg_path), AAI_XCRUN_PY=str(ROOT / 'scripts/aai_xcrun.py'), J=a.j, DMSH_PCH=a.pch,
           DMSH_CAS=str(out_root / 'cas.db'))
cmd = [sys.executable, '-I', '-X', 'utf8', str(NX / 'dm/gfx/tools/dmsh_build.py'), str(W / 'shader/split/pkg'), str(out_root / 'cache'), *a.keys]
print(' '.join(cmd), flush=True)
sys.exit(subprocess.run(cmd, env=env, cwd=str(out_root)).returncode)
