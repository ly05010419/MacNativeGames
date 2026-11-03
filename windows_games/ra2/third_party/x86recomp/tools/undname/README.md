# UnDecorateSymbolName reference check (dev-time, uses Wine)

A game that binds script calls through its EXE's exports may parse
`dbghelp!UnDecorateSymbolName` output (flags 0x800) for every exported
function. Our implementation (src/runtime/hle/dbghelp.cpp) must match
Microsoft's byte for byte, or such a game mis-registers functions and
its scripts fail silently.

Reference: Microsoft's dbghelp.dll, which ships with the game, run
under Wine (behaviour reference only; nothing from it ships):

    i686-w64-mingw32-gcc -O1 tools/undname/undref.c -o /tmp/undref.exe
    awk -F'\t' '$2=="func"{print $3}' symbols/exports.tsv | sort -u > /tmp/names.txt
    cp "<game install>/dbghelp.dll" /tmp/dbghelp_ref.dll
    wine /tmp/undref.exe 'Z:\tmp\dbghelp_ref.dll' /tmp/names.txt /tmp/ref.txt

Then undecorate the same names with recomp::hle::UndnameMsvc and diff (the
reference has CRLF line ends). Last result: 3230/3230 identical.
