target remote localhost:1234
symbol-file build/minicore.elf
set architecture i386:x86-64
set pagination off
set confirm off

source scripts/pn584_alloc_watch.py

break kPanic

continue

echo \n=== STOPPED (either kPanic or a DOUBLE ALLOC/FREE hit) ===\n
bt
info registers
