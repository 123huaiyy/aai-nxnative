# Windows compatibility for running nxnative's (macOS-written) tools with this private Python, loaded at every interpreter start (also in spawned workers):
# - os.open opens binary (Windows' C runtime defaults to text mode, which would translate CR LF and stop at 0x1A)
# - os.pread / os.pwrite, which Windows' os module lacks (one lock around seek + read/write, so threads sharing a descriptor stay correct)
# - ctypes.CDLL(None) (macOS: the process's own symbols, e.g. clonefile) gives an object without symbols, so callers fall back to plain copies
# - with AAI_XCRUN set (a JSON file, see aai_xcrun.py), xcrun commands go to the Windows Metal tools
import os
import sys

if os.name == 'nt':
    import threading

    _open, _lock = os.open, threading.Lock()

    def _binary_open(path, flags, mode=0o777, *, dir_fd=None):
        return _open(path, flags | os.O_BINARY, mode, dir_fd=dir_fd)

    os.open = _binary_open

    if not hasattr(os, 'pread'):
        def pread(fd, n, offset):
            with _lock:
                was = os.lseek(fd, 0, os.SEEK_CUR)
                try:
                    os.lseek(fd, offset, os.SEEK_SET)
                    out = bytearray()
                    while len(out) < n:
                        chunk = os.read(fd, n - len(out))
                        if not chunk:
                            break
                        out += chunk
                    return bytes(out)
                finally:
                    os.lseek(fd, was, os.SEEK_SET)
        os.pread = pread

    if not hasattr(os, 'pwrite'):
        def pwrite(fd, data, offset):
            with _lock:
                was = os.lseek(fd, 0, os.SEEK_CUR)
                try:
                    os.lseek(fd, offset, os.SEEK_SET)
                    return os.write(fd, data)
                finally:
                    os.lseek(fd, was, os.SEEK_SET)
        os.pwrite = pwrite

    import ctypes

    class _NoSymbols:
        def __getattr__(self, name):
            raise AttributeError(name)

    class _CDLLCompat(ctypes.CDLL):
        def __new__(cls, name, *args, **kwargs):
            return _NoSymbols() if name is None else super().__new__(cls)

    ctypes.CDLL = _CDLLCompat

    if os.environ.get('AAI_XCRUN'):
        sys.path.insert(0, os.path.dirname(os.environ['AAI_XCRUN_PY'])) if os.environ.get('AAI_XCRUN_PY') else None
        try:
            import aai_xcrun   # noqa: F401  (installs the subprocess hook)
        except ImportError as e:
            sys.stderr.write(f'[aai] xcrun shim not loaded: {e}\n')
