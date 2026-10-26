#!/usr/bin/env python3
"""
inject_inspect.py - disassemble / lint a compiled INJECT.BIN against the
AT32UC3B1 firmware's opcode contract (src/main.c).

WHY THIS EXISTS
---------------
The firmware only ever sees INJECT.BIN - raw 16-bit big-endian words produced by
your DuckyScript compiler (PayloadStudio).  Almost every "it typed garbage / it
opened m365 / it stopped looping" bug is really a MISMATCH between the bytes the
compiler emits and the opcodes the firmware understands.  You cannot see that by
reading DuckyScript; you have to look at the bytes.

Run this on the exact INJECT.BIN you flash to the SD card:

    python tools/inject_inspect.py path/to/INJECT.BIN

It prints a word-by-word disassembly and a LINT report that flags the specific
conditions known to cause the three reported bugs:

  * odd file length              -> guaranteed word misalignment
  * a keystroke carrying a GUI    -> can land on Start menu / m365
    (Win) modifier bit, or the full 0x0F Ctrl+Shift+Alt+Win "Office key" combo
  * a KEY_DOWN (0xFFF8) that holds a bare modifier (intentional, but flagged)
  * a keycode outside 0x04..0x65  -> not a real key; the firmware now drops it,
    but its presence means the stream is misaligned or an opcode is unhandled
  * an unknown word that would fall through to the keystroke default case
  * DELAY chunks (each 0x00XX pair is one delay chunk; big delays are split
    into repeated [0x00,0xFF] 255 ms chunks + a remainder)

Opcode table below is transcribed directly from src/main.c so the disassembly
matches the device exactly.
"""
import sys

# ---- opcodes (verbatim from src/main.c) -----------------------------------
OPCODES = {
    0xe8e8: "REGISTERS (var block delimiter)",
    0xe801: "ASSIGN",
    0xefef: "IF",
    0x1ff4: "END_IF",
    0xf8f8: "GOTO",
    0xf7f7: "CALL",
    0xfdfd: "RETURN",
    0xfff8: "KEY_DOWN (bare modifier hold)",
    0xeee8: "KEY_UP",
    0xeaee: "BUTTON_DEF",
    0xebf4: "END_BUTTON",
    0xe9e9: "INJECT_VAR / RANDOM",
    0xe7e9: "DELAY_VAR",
    0xf6e9: "EXFIL_VAR (stub)",
    0xf0f0: "ATTACKMODE OFF",
    0xf1f1: "ATTACKMODE HID",
    0xf2f2: "ATTACKMODE STORAGE (forced->HID)",
    0xf3f3: "ATTACKMODE HID+STORAGE (forced->HID)",
    0x04ed: "RESET (no-op)",
    0xebee: "BUTTON_DISABLE",
    0xecee: "BUTTON_ENABLE",
    0xebf1: "STOP_PAYLOAD",
    0xeaed: "LED_OFF",
    0xebed: "LED_G",     # green  (verified: source LED_OFF/LED_R/LED_G -> eaed/eced/ebed)
    0xeced: "LED_R",     # red    (was mislabelled LED_GREEN)
    0xeeed: "LED_RED",   # unverified alias
    0xeffe: "LED_RED",   # unverified alias
    0xedee: "SYSLED_ON",
    0xeeee: "SYSLED_OFF",
    0xeaeb: "SAVE_HOST_LOCK_STATE",
    0xebeb: "RESTORE_HOST_LOCK_STATE",
    0xeae9: "SAVE_ATTACKMODE",
    0xebe9: "RESTORE_ATTACKMODE",
    0xeaea: "WAIT_FOR_BUTTON_PRESS",
    0x01ea: "WAIT_FOR_CAPS_ON",    0x02ea: "WAIT_FOR_CAPS_OFF",
    0x03ea: "WAIT_FOR_CAPS_CHANGE", 0x04ea: "WAIT_FOR_NUM_ON",
    0x05ea: "WAIT_FOR_NUM_OFF",    0x06ea: "WAIT_FOR_NUM_CHANGE",
    0x07ea: "WAIT_FOR_SCROLL_ON",  0x08ea: "WAIT_FOR_SCROLL_OFF",
    0x09ea: "WAIT_FOR_SCROLL_CHANGE",
    0xeaf1: "RESTART_PAYLOAD",
    0xf8e9: "HIDE_PAYLOAD",
    0xf9e9: "RESTORE_PAYLOAD",
}

# Internal-variable register addresses (token -> $_NAME), verified against real
# compiled samples.  Used only to annotate ASSIGN dest/src words for readability.
VARS = {
    0x8042: "$_BUTTON_ENABLED", 0x8142: "$_BUTTON_USER_DEFINED",
    0x8442: "$_BUTTON_PUSH_RECEIVED", 0x8642: "$_SYSTEM_LEDS_ENABLED",
    0x8742: "$_STORAGE_LEDS_ENABLED", 0x8842: "$_INJECTING_LEDS_ENABLED",
    0x8942: "$_EXFIL_LEDS_ENABLED", 0x9642: "$_RECEIVED_HOST_LOCK_LED_REPLY",
    0xf342: "$_RANDOM", 0xFC42: "$_RANDOM_NUMBER_KEYCODE",
    0xFE42: "$_RANDOM_CHAR_KEYCODE", 0xFF42: "$_RANDOM_LETTER_KEYCODE",
    0x9042: "$_CAPSLOCK_ON", 0x9142: "$_NUMLOCK_ON", 0x9242: "$_SCROLLLOCK_ON",
    0x9342: "$_SAVED_CAPSLOCK_ON", 0x9442: "$_SAVED_NUMLOCK_ON",
    0x9542: "$_SAVED_SCROLLLOCK_ON",
    0x9742: "$_EXFIL_MODE_ENABLED", 0x9842: "$_STORAGE_ACTIVITY_TIMEOUT",
    0x9942: "$_BUTTON_TIMEOUT",
    0x9B42: "$_CURRENT_VID", 0x9C42: "$_CURRENT_PID", 0x9D42: "$_OS",
    0x9F42: "$_HOST_CONFIGURATION_REQUEST_COUNT",
    0xA042: "$_CURRENT_ATTACKMODE", 0xA242: "$_JITTER_ENABLED",
    0xA342: "$_JITTER_MAX",
    0xf042: "$_RANDOM_MIN", 0xf142: "$_RANDOM_MAX",
    0xA842: "$_RANDOM_INT",    # verified tests/6.bin
    0xf242: "$_RANDOM_SEED",   # verified tests/4.bin (firmware historically mislabelled this $_RANDOM_INT)
    0x6742: "FALSE", 0x6842: "TRUE",
}
# opcodes that carry inline argument words (name -> how many extra words to skip)
ARG_WORDS = {0xe801: 1, 0xefef: 2, 0x1ff4: 1, 0xf8f8: 1, 0xf7f7: 1,
             0xfff8: 1, 0xeee8: 1, 0xeaee: 1, 0xebf4: 1, 0xe9e9: 1,
             0xe7e9: 1, 0xf6e9: 1}

# eval_op operator opcodes -> symbol (matches eval_op() in main.c)
EVAL_OPS = {
    0xe802: "+", 0xe803: "-", 0xe804: "*", 0xe805: "/", 0xe80e: "%",
    0x0fe8: "^", 0xe806: "==", 0xe807: "!=", 0xe808: "<", 0xe809: ">",
    0xe8a8: "<=", 0xe8a9: ">=", 0xe8aa: "&&", 0xe8bb: "||",
    0xe80a: "&", 0xe80b: "|", 0xe80c: ">>", 0xe80d: "<<",
}

MOD_BITS = [(0x01, "LCtrl"), (0x02, "LShift"), (0x04, "LAlt"), (0x08, "LGui"),
            (0x10, "RCtrl"), (0x20, "RShift"), (0x40, "RAlt"), (0x80, "RGui")]

# usage-id -> label for the common printable keys (US), just for readability
KEYNAME = {0x28: "ENTER", 0x2c: "SPACE", 0x2b: "TAB", 0x29: "ESC",
           0x4c: "DEL", 0x2a: "BKSP"}
for i in range(26):
    KEYNAME[0x04 + i] = chr(ord('a') + i)
for i, d in enumerate("1234567890"):
    KEYNAME[0x1e + i] = d


def mods(m):
    on = [name for bit, name in MOD_BITS if m & bit]
    return "+".join(on) if on else "-"


def disasm(data):
    warnings = []
    if len(data) % 2 != 0:
        warnings.append(f"FILE LENGTH IS ODD ({len(data)} bytes) -> the whole "
                        f"stream after the odd byte is MISALIGNED. A bare "
                        f"single-byte modifier (e.g. 'CTRL' on its own line) is "
                        f"the usual cause.")
    words = [(data[i] << 8) | data[i + 1] for i in range(0, len(data) - 1, 2)]

    print(f"{'idx':>4}  {'addr':>5}  {'word':>6}  decode")
    print("-" * 64)
    i = 0
    n = len(words)
    while i < n:
        w = words[i]
        addr = i * 2
        if i == 0 and w == 0xe8e8:         # leading REGISTERS/constant-pool block
            print(f"{i:>4}  {addr:>5}  0x{w:04x}  REGISTERS (constant pool begin)")
            j = i + 1
            while j < n and words[j] != 0xe8e8:
                print(f"{j:>4}  {j*2:>5}  0x{words[j]:04x}  const[{j}] = {words[j]}")
                j += 1
            if j < n:
                print(f"{j:>4}  {j*2:>5}  0x{words[j]:04x}  REGISTERS (constant pool end)")
                j += 1
            i = j
            continue
        if w in (0xf0f0, 0xf1f1, 0xf2f2, 0xf3f3):   # ATTACKMODE block
            amname = {0xf0f0: "OFF", 0xf1f1: "HID",
                      0xf2f2: "STORAGE", 0xf3f3: "HID+STORAGE"}[w]
            print(f"{i:>4}  {addr:>5}  0x{w:04x}  ATTACKMODE {amname}")
            j = i + 1
            # options run until the SAME opcode repeats (terminator) — matches
            # parse_attackmode() in main.c.
            while j < n and words[j] != w:
                p = words[j]
                if p == 0xf5f5 and j + 1 < n:
                    print(f"{j:>4}  {j*2:>5}  0x{p:04x}  VID_ = {VARS.get(words[j+1], f'0x{words[j+1]:04x}')}")
                    j += 2
                elif p == 0xf6f6 and j + 1 < n:
                    print(f"{j:>4}  {j*2:>5}  0x{p:04x}  PID_ = {VARS.get(words[j+1], f'0x{words[j+1]:04x}')}")
                    j += 2
                elif p in (0xf9f9, 0xfafa, 0xfbfb):   # MAN_/PROD_/SERIAL_ string, delimited by repeat
                    lbl = {0xf9f9: "MAN_", 0xfafa: "PROD_", 0xfbfb: "SERIAL_"}[p]
                    print(f"{j:>4}  {j*2:>5}  0x{p:04x}  {lbl}(string)")
                    j += 1
                    while j < n and words[j] != p:
                        j += 1
                    if j < n:
                        j += 1                        # consume closing delimiter
                else:
                    j += 1
            if j < n:
                print(f"{j:>4}  {j*2:>5}  0x{words[j]:04x}  (ATTACKMODE end)")
                j += 1
            i = j
            continue
        if w == 0xe6e9:                    # INJECT_MOD (bare-modifier prefix)
            nxt = words[i+1] if i + 1 < n else None
            if nxt is not None and (nxt >> 8) == 0x00 and (nxt & 0xFF):
                print(f"{i:>4}  {addr:>5}  0x{w:04x}  INJECT_MOD {mods(nxt & 0xFF)} "
                      f"(bare-modifier tap)")
                i += 2
            else:
                print(f"{i:>4}  {addr:>5}  0x{w:04x}  INJECT_MOD (prefix for HOLD)")
                i += 1
            continue
        if w == 0xe801:                    # ASSIGN: dest, s1, s2, OP  (OP is LAST)
            def vname(x):
                return VARS.get(x, f"var[0x{x:04x}]" if x < 0x0400 else f"0x{x:04x}")
            dest = words[i+1] if i+1 < n else 0
            s1   = words[i+2] if i+2 < n else 0
            s2   = words[i+3] if i+3 < n else 0
            if s2 == 0:                    # 4th word 0 -> plain copy
                print(f"{i:>4}  {addr:>5}  0x{w:04x}  ASSIGN {vname(dest)} = {vname(s1)}")
                i += 4
            else:
                op = words[i+4] if i+4 < n else 0
                print(f"{i:>4}  {addr:>5}  0x{w:04x}  ASSIGN {vname(dest)} = "
                      f"{vname(s1)} {EVAL_OPS.get(op, f'op(0x{op:04x})')} {vname(s2)}")
                i += 5
            continue
        if w in OPCODES:
            name = OPCODES[w]
            extra = ARG_WORDS.get(w, 0)
            arg = f"  arg=0x{words[i+1]:04x}" if extra and i + 1 < n else ""
            print(f"{i:>4}  {addr:>5}  0x{w:04x}  {name}{arg}")
            if w == 0xfff8 and i + 1 < n:
                k = words[i + 1]
                km = k & 0xFF
                warnings.append(f"[{i}] KEY_DOWN holds modifier {mods(km)} "
                                f"(0x{km:02x}) - intentional for combos, but if "
                                f"this KEY_DOWN is spurious it is a direct GUI/"
                                f"m365 path.")
            i += 1 + extra
            continue
        # short DELAY 0x00XX (XX != FF handled above)
        if (w >> 8) == 0x00:
            print(f"{i:>4}  {addr:>5}  0x{w:04x}  DELAY {w & 0xFF} ms (short)")
            i += 1
            continue
        # otherwise: raw keystroke keycode<<8 | modifier
        kc = (w >> 8) & 0xFF
        md = w & 0xFF
        label = KEYNAME.get(kc, f"key 0x{kc:02x}")
        note = ""
        if not (0x04 <= kc <= 0x65):
            note = "  <-- keycode OUT OF RANGE (dropped by firmware; misalign/unknown opcode?)"
            warnings.append(f"[{i}] word 0x{w:04x}: keycode 0x{kc:02x} not a real "
                            f"key - stream misaligned or unhandled opcode.")
        if md == 0x0f:
            # all four L-modifiers = the actual Office-key / m365 combo
            note = "  <-- 0x0F = Ctrl+Shift+Alt+Win (m365 'Office key' combo!)"
            warnings.append(f"[{i}] word 0x{w:04x}: modifier 0x0F = "
                            f"Ctrl+Shift+Alt+Win (the m365 'Office key' combo). "
                            f"This is almost always misalignment, not intent.")
        elif md & (0x08 | 0x80):
            # GUI + a keycode is a legitimate combo the user wrote (GUI r, GUI e).
            # The fixed firmware now sends it correctly; only note it for context.
            note = f"  (GUI combo: Win+{label} - intentional if you wrote 'GUI {label}')"
        print(f"{i:>4}  {addr:>5}  0x{w:04x}  PRESS {label} mod={mods(md)}{note}")
        i += 1

    print("\n" + "=" * 64)
    print(f"LINT: {len(warnings)} warning(s)")
    for wmsg in warnings:
        print("  * " + wmsg)
    if not warnings:
        print("  clean - no known bug signatures in this payload.")
    return len(warnings)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        print("usage: python inject_inspect.py INJECT.BIN")
        return 2
    with open(argv[1], "rb") as f:
        data = f.read()
    print(f"file: {argv[1]}   ({len(data)} bytes, {len(data)//2} words)\n")
    return 0 if disasm(data) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
