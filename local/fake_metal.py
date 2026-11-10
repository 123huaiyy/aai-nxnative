# A stand-in for the Metal compiler and metallib, only for a dry run of dmsh_build.py on Windows before the real Windows Metal tools are installed:
# it writes placeholder .air / .metallib files (never used in an app: the dry run keeps its own output and cache folders). metal --version prints a fake identity.
# Usage (through aai_xcrun.py's tool mapping): python -I fake_metal.py metal|metallib <args...>
import sys

tool, args = sys.argv[1], sys.argv[2:]
if '--version' in args:
    print(f'fake {tool} for a dry run (aai)')
    sys.exit(0)
out = args[args.index('-o') + 1] if '-o' in args else None
src = sys.stdin.buffer.read() if '-' in args or not any(a.endswith(('.metal', '.air')) for a in args) else b''
if out:
    with open(out, 'wb') as f:
        f.write(b'AAI-FAKE-' + tool.encode() + b'\0' + src[:64])
