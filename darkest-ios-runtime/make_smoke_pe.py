"""Create a minimal PE fixture to exercise the production loader without game files."""
from pathlib import Path
from struct import pack_into
import sys

pe = bytearray(0x400)
pe[:2] = b"MZ"
pack_into("<I", pe, 0x3C, 0x80)
pe[0x80:0x84] = b"PE" + bytes(2)
pack_into("<HHIIIHH", pe, 0x84, 0x8664, 1, 0, 0, 0, 0xF0, 0x22)
optional = 0x98
pack_into("<H", pe, optional, 0x20B)
pack_into("<II", pe, optional + 16, 0x1000, 0x1000)
pack_into("<QII", pe, optional + 24, 0x14000000, 0x1000, 0x200)
pack_into("<II", pe, optional + 56, 0x2000, 0x200)
pack_into("<H", pe, optional + 68, 2)
pack_into("<Q", pe, optional + 72, 0x100000)
section = optional + 0xF0
pe[section:section + 8] = b".text" + bytes(3)
pack_into("<IIIIIIHHI", pe, section + 8, 0x40, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x60000020)
pe[0x200] = 0xC3
pe[0x220] = 0xC3
Path(sys.argv[1]).write_bytes(pe)
