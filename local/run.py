# Runs an nxnative script as `python <script> args...` would, but under `python -I -X utf8 run.py <script> args...`:
# isolated mode keeps the current directory and PYTHON* variables out, and only the script's own folder goes first on sys.path.
import os
import runpy
import sys

script = os.path.abspath(sys.argv[1])
sys.argv = [script] + sys.argv[2:]
sys.path.insert(0, os.path.dirname(script))
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'winshim'))   # POSIX-only modules (fcntl) for Windows, after everything else
runpy.run_path(script, run_name='__main__')
