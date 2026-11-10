# Derives the game profile for a version nxnative's profile was not brought up on, with build_ipa_from_nsp.py's own derive_profile (its keys as they are,
# this version's build row, package and shader versions; the shader tables are made again from this version's files by shader_split.py).
# Usage: python -I -X utf8 aai_derive.py <nxnative> <profile id> <version, e.g. v65536> <extracted version folder> <output profile folder>
import sys
from pathlib import Path

repo, pid, ver, data, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5])
sys.path.append(str(Path(__file__).resolve().parent / 'winshim'))   # fcntl for Windows
sys.path.insert(0, str(repo / 'scripts'))
sys.path.insert(0, str(repo / 'dm/games'))
sys.path.insert(0, str(repo / 'dm/tools'))
import build_ipa_from_nsp as B
import game
import profile_guess
import nacp
B.pg, B.nacp = profile_guess, nacp   # what the script's main() imports into its globals
g = game.load(pid)
disp = B.derive_profile(g, out, ver, data, g.words('modules'))
print(f'derived profile for {ver} ({disp}): {out}')
