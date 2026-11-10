# aai-nxnative

Build helpers for playing the user's own copy of Ace Attorney Investigations Collection (Nintendo Switch) on an iPhone with
[seedds/nxnative](https://github.com/seedds/nxnative), without a Mac.

**This repository and its workflow runs hold no game files, keys or anything made from them.** The workflows build only from public sources:

- `win-tools`: Windows builds of nxnative's `nx_extract` and `dmsh_xlate`, and of KhronosGroup's SPIRV-Cross at the tag nxnative pins.
- `ios-shell`: nxnative's iOS app for one game profile, with zero-filled placeholder sections of a given layout. The layout input is numbers only
  (module sizes and offsets) plus the derived `game.tsv` (build IDs, display version, memory settings).

Everything that touches the game (extraction with the user's own keys, nxnative's code rewrite, shader extraction and translation, writing the
rewritten modules into the placeholder sections, packing the IPA) runs on the user's own Windows PC with the scripts in `local/`.

## Local scripts (Windows)

- `local/sitecustomize.py`: Windows compatibility for nxnative's tools in a private Python (binary `os.open`, `os.pread`, `ctypes.CDLL(None)`,
  and `xcrun` forwarded to the Windows Metal tools when `AAI_XCRUN` is set).
- `local/fcntl.py`: no-op stand-in for POSIX `fcntl`.
- `local/run.py`: runs an nxnative script under `python -I` with its own folder on `sys.path`.
- `local/aai_derive.py`: nxnative's `derive_profile` for a game version its profile was not brought up on.
- `local/aai_xcrun.py`: runs `dmsh_build.py` unchanged (its chain version must stay the host's) with the Windows Metal tools.
- `local/aai_pack.py`: assembles the IPA from the `ios-shell` app and the locally made parts.

nxnative is MIT licensed; see its repository for its third-party licenses.
