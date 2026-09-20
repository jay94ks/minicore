target remote localhost:1234
symbol-file build/minicore.elf
set architecture i386:x86-64
set pagination off
set confirm off

source scripts/pn584_watch.py

break kPanic

continue
