#!/usr/bin/env python3
"""Builds nxnative's iOS app (DmIOS.app) for one game profile with placeholder module sections, on a Mac with Xcode.

No game data is used: the embedded modules are zero-filled files with the layout the user's rewrite produced (sizes and
offsets only), and the tables are empty stand-ins. The real module bytes, rewriter and shader tables, shader libraries and
game files are put into the app afterwards, on the user's own machine (aai_pack.py).

Usage: build_shell.py --repo <nxnative checkout> --layout <layout.json> --output <new directory>
layout.json: {"game": "<profile id>", "game_tsv": "<the derived game.tsv text>", "display_name": "...", "version": "...",
              "language": "en-US", "bundle_id": "...", "modules": [{"name": "main", "size": <patched.bin bytes>,
              "header": {"PATCH_OFF": "0x...", "KEY_PAGE_OFF": "0x...", "KEY_PAGE_SIZE": "0x4000", "TEXT_END": "0x...", ...}}, ...]}
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ARIAL_UNICODE = '/System/Library/Fonts/Supplemental/Arial Unicode.ttf'   # build_ipa.sh's shared-font stand-in


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--repo', required=True, type=Path)
    ap.add_argument('--layout', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    lay = json.loads(a.layout.read_text(encoding='utf-8'))
    repo = a.repo.resolve()
    game_id = lay['game']
    gdir = repo / 'dm/games' / game_id
    if not (gdir / 'game.tsv').is_file():
        sys.exit(f'no game profile {gdir}')
    if lay.get('game_tsv'):   # the profile derived for the user's version (its build row, package, mem_used): what the host is compiled with
        (gdir / 'game.tsv').write_text(lay['game_tsv'], encoding='utf-8')

    root = a.output.resolve()
    root.mkdir(parents=True)
    ios = repo / 'dm/ios'
    project = root / 'project'
    gen = project / 'Gen'
    tables = gen / 'dm'
    tables.mkdir(parents=True)
    for name in ('Sources', 'tools'):
        (project / name).symlink_to(ios / name, target_is_directory=True)
    shutil.copyfile(ios / 'project.yml', project / 'project.yml')
    assets = gen / 'Assets.xcassets'
    assets.mkdir()
    (assets / 'Contents.json').write_text(json.dumps({'info': {'author': 'xcode', 'version': 1}}))

    # Placeholder modules: zero-filled files of the real sizes, and fixups headers with the real layout, so gen_embed.py lays the sections out exactly as for the real modules
    patched = root / 'patched'
    patched.mkdir()
    names = []
    for m in lay['modules']:
        n = m['name']
        names.append(n)
        with open(patched / f'{n}.patched.bin', 'wb') as f:
            f.truncate(int(m['size']))
        (patched / f'{n}.fixups.tsv').write_text(''.join(f'# {k}\t{v}\n' for k, v in m['header'].items()))
        shutil.copyfile(patched / f'{n}.fixups.tsv', tables / f'{n}.fixups.tsv')
    (tables / 'PLACEHOLDER.txt').write_text('Stand-in tables of a CI build; replaced on the user\'s machine.\n')

    env = dict(os.environ, GAME=game_id, DM_GAME=game_id,
               DM_BUNDLE_ID=lay.get('bundle_id') or f'org.nxnative.{game_id.lower()}',
               DM_DISPLAY_NAME=lay.get('display_name') or game_id, DM_APP_VERSION=lay.get('version') or '1.0',
               DM_APP_CATEGORY=lay.get('app_category') or 'public.app-category.games', DM_TEAM_ID='0000000000',
               DM_SCHEME_NAME='DmIOS', DM_APPICON='', DM_SHOW_FPS='NO', DM_LANG=lay.get('language') or 'en-US')

    def run(*command, capture=False):
        print('+', ' '.join(str(c) for c in command), flush=True)
        return subprocess.run([str(c) for c in command], cwd=project, env=env, check=True, text=True,
                              stdout=subprocess.PIPE if capture else None)

    config = run(sys.executable, ios / 'tools/gen_build.py', repo / 'dm/host', gen, capture=True).stdout
    flags = run(sys.executable, ios / 'tools/gen_embed.py', patched, gen / 'embed.S', *names, capture=True).stdout.strip()
    (gen / 'dm.xcconfig').write_text(config + f'OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS) {flags}\n')
    print(gen.joinpath('dm.xcconfig').read_text(), flush=True)

    # Shared-font stand-ins, as build_ipa.sh makes them (no game data)
    (tables / 'font').mkdir()
    shutil.copyfile(ARIAL_UNICODE, tables / 'font/standard.ttf')
    run(sys.executable, repo / 'dm/tools/make_ext_font.py', tables / 'font/nintendo_ext.ttf')

    run('xcodegen', 'generate')
    chain = next(line.split('"')[1] for line in (repo / 'dm/gfx/nvn/dmsh_chain.h').read_text().splitlines()
                 if line.startswith('#define DMSH_CHAIN "'))
    commit = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip()
    for key, value in (('DMCommit', commit or 'unknown'), ('DMChain', chain)):
        run('/usr/libexec/PlistBuddy', '-c', f'Add :{key} string {value}', gen / 'Info.plist')

    derived = root / 'derived'
    run('xcodebuild', '-project', project / 'DmIOS.xcodeproj', '-scheme', 'DmIOS',
        '-configuration', 'Release', '-destination', 'generic/platform=iOS',
        '-derivedDataPath', derived, '-jobs', '3',
        'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY=', 'DM_COPY_GAME_RESOURCES=NO',
        f'DM_XCODE_OUTPUT_ROOT={derived}', f'SYMROOT={derived}/Build/Products',
        f'OBJROOT={derived}/Build/Intermediates.noindex', 'build')

    app = derived / 'Build/Products/Release-iphoneos/DmIOS.app'
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    exe = app / info['CFBundleExecutable']
    if not exe.is_file():
        sys.exit(f'no linked executable {exe}')
    out = root / 'out'
    out.mkdir()
    shutil.copytree(app, out / 'DmIOS.app', symlinks=True)
    (out / 'shell.json').write_text(json.dumps({'chain': chain, 'commit': commit, 'game': game_id,
                                                'modules': lay['modules'], 'flags': flags}, indent=1))
    print(f'shell app: {out / "DmIOS.app"} (chain {chain}, nxnative {commit})', flush=True)


if __name__ == '__main__':
    main()
