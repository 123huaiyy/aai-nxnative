
from pathlib import Path
import sys
root=Path(sys.argv[1])/"usr/lib"
checks={
 "libc++.tbd": [
 "__ZNSt3__17collateIwE2idE",
 "__ZNSt3__15ctypeIwE2idE",
 "__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE9__grow_byEmmmmmm",
 "__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE21__grow_by_and_replaceEmmmmmmPKw",
 "__ZNKSt3__123__match_any_but_newlineIwE6__execERNS_7__stateIwEE",
 ],
 "system/libsystem_c.tbd":["_wmemcmp"],
}
for file, symbols in checks.items():
    path=root/file
    if not path.is_file() and file=="libc++.tbd": path=root/"libc++.1.tbd"
    content=path.read_text()
    for symbol in symbols:
        assert symbol in content, symbol
print("All six wide-regex symbols are present in the iPhoneOS SDK.")
