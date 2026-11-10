# Runs nxnative's shader build (dmsh_build.py, unchanged, so its chain version stays the one the iOS host is built with) on Windows:
# commands starting with xcrun go to the tools named in the JSON file AAI_XCRUN, and the translation-chain lock check (which compares the
# macOS binaries' fingerprints) is skipped, since the Windows builds of the translator, spirv-cross and Metal compiler necessarily differ.
# Loaded by sitecustomize.py when AAI_XCRUN is set. AAI_XCRUN JSON:
#   {"nxnative": "E:/aai/nxnative",
#    "sdks": {"iphoneos": {"metal": "...\\metal.exe", "metallib": "...\\metallib.exe", "metal_args": [...], "sdk_path": "...", "sdk_version": "18.0", "sdk_build": "..."},
#             "macosx": {...}}}
# metal_args are put right after the metal executable (e.g. the target the Windows driver needs for iOS).
import json
import os
import subprocess
import sys

CFG = json.load(open(os.environ['AAI_XCRUN'], encoding='utf-8'))
SDKS = CFG['sdks']
_Popen = subprocess.Popen


def _echo(text):
    return [sys.executable, '-I', '-c', f'print({text!r})']


def translate(args):
    """xcrun [-sdk X | --sdk X] (tool args... | -f tool | --show-sdk-path | --show-sdk-version | --show-sdk-build-version) → the Windows command"""
    a, sdk = list(args[1:]), 'macosx'
    while a and a[0] in ('-sdk', '--sdk'):
        sdk, a = a[1], a[2:]
    cfg = SDKS.get(sdk)
    if cfg is None:
        raise FileNotFoundError(f'aai_xcrun: no tools configured for SDK {sdk} ({" ".join(map(str, args))})')
    if a[:1] == ['-f']:
        return _echo(cfg[a[1]])
    if a[:1] == ['--show-sdk-path']:
        return _echo(cfg.get('sdk_path', ''))
    if a[:1] == ['--show-sdk-version']:
        return _echo(cfg.get('sdk_version', ''))
    if a[:1] == ['--show-sdk-build-version']:
        return _echo(cfg.get('sdk_build', ''))
    tool, rest = a[0], a[1:]
    if tool not in cfg:
        raise FileNotFoundError(f'aai_xcrun: no {tool} configured for SDK {sdk}')
    return [cfg[tool], *cfg.get(f'{tool}_args', []), *rest]


class Popen(_Popen):
    def __init__(self, args, *a, **k):
        if isinstance(args, (list, tuple)) and args and os.path.basename(str(args[0])) == 'xcrun':
            args = translate(args)
        super().__init__(args, *a, **k)


subprocess.Popen = Popen

# The lock check (tool_lock.check_tools) compares macOS binaries' fingerprints: skip it for the Windows builds
tools = os.path.join(CFG['nxnative'], 'dm', 'gfx', 'tools')
if tools not in sys.path:
    sys.path.insert(0, tools)
import tool_lock   # noqa: E402
tool_lock.check_tools = lambda: []
