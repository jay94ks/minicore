import gdb

# PN-584DB994 gdb watchpoint hunt (2026-09-20 tick) - watches the exact
# stack slot that stores kSyncDebugRegs()'s local `dr7` value between its
# last legitimate write and the final `mov %rax,%dr7`. Any write to that
# address whose RIP falls OUTSIDE kSyncDebugRegs's own instruction range
# is, by construction, an illegitimate/external write - the smoking gun
# this plan's history (see plan body update 9/11) has been hunting for.
#
# Addresses below are read fresh from THIS build (nm/objdump), not
# hardcoded from the historical investigation - code layout shifts.

FUNC_START = 0xffffffff8011af96
FUNC_END = 0xffffffff8011b1c8  # exclusive (next symbol start)
ENTRY_BP_ADDR = 0xffffffff8011af9a  # right after `mov %rsp,%rbp`
EXIT_BP_ADDR = 0xffffffff8011b1c7   # the `ret`

state = {"wp": None, "hits": 0, "calls": 0}


def log(msg):
    gdb.write(f"[pn584-watch] {msg}\n")
    gdb.flush()


class EntryBreak(gdb.Breakpoint):
    def stop(self):
        frame = gdb.selected_frame()
        rbp = int(frame.read_register("rbp"))
        addr = rbp - 8
        state["calls"] += 1
        if state["wp"] is not None:
            try:
                state["wp"].delete()
            except Exception:
                pass
            state["wp"] = None
        wp = DrSlotWatch(f"*(unsigned long*)0x{addr:x}", addr)
        state["wp"] = wp
        return False  # don't actually stop the program, just continue


class ExitBreak(gdb.Breakpoint):
    def stop(self):
        if state["wp"] is not None:
            try:
                state["wp"].delete()
            except Exception:
                pass
            state["wp"] = None
        return False


class DrSlotWatch(gdb.Breakpoint):
    def __init__(self, expr, addr):
        super().__init__(expr, gdb.BP_WATCHPOINT, gdb.WP_WRITE, internal=False)
        self.addr = addr

    def stop(self):
        rip = int(gdb.selected_frame().read_register("rip"))
        if FUNC_START <= rip < FUNC_END:
            # legitimate write by kSyncDebugRegs itself (zero-init or the
            # accumulation loop's `or %rax,-0x8(%rbp)`) - not interesting.
            return False
        state["hits"] += 1
        log(f"*** EXTERNAL WRITE to dr7 slot 0x{self.addr:x} from rip=0x{rip:x} (outside kSyncDebugRegs [0x{FUNC_START:x},0x{FUNC_END:x})) ***")
        gdb.execute("info registers")
        gdb.execute("bt")
        gdb.execute(f"x/i 0x{rip:x}")
        return True  # actually stop here - this is the money shot


EntryBreak(f"*0x{ENTRY_BP_ADDR:x}", internal=False)
ExitBreak(f"*0x{EXIT_BP_ADDR:x}", internal=False)
log(f"armed - entry=0x{ENTRY_BP_ADDR:x} exit=0x{EXIT_BP_ADDR:x} func=[0x{FUNC_START:x},0x{FUNC_END:x})")
