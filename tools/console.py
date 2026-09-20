"""Drive the board's serial console non-interactively.

    python3 tools/console.py wifi_show
    python tools/console.py --port COM4 ota_status
    python3 tools/console.py "wifi_set TestNetwork hunter2" wifi_show
    python3 tools/console.py --timeout 120 "ota http://192.168.1.125:8000/pandadeath.bin"

Each argument is one command line, sent in order; everything the board prints
back is echoed here.

`--timeout` raises the per-command ceiling from the default 5 s. Needed for
`ota`, which streams a megabyte and a half and reports progress as it goes;
without it the reply is cut off mid-download and a working update looks like a
hung one. Exists because `idf.py monitor` needs a real terminal, so
an agent cannot use it — but the console is just line-oriented text over the
same UART that capture.py already reads, and that needs no terminal at all.
Before this, every console check cost a human typing at a prompt.

Uses pyserial, which lives only in the IDF virtualenv. Either source export.sh
first or call that interpreter directly:

    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/console.py wifi_show

Does not replace capture.py: this assumes the board is already up and the port
already exists, whereas capture.py exists to survive a replug. Use capture.py
for boot logs, this for asking the running firmware questions.

**Never pass a real credential.** Arguments land in shell history, in the
process list while running, and in whatever transcript is watching. Test with
throwaway values; real ones get typed by a human at `idf.py monitor`.
"""

import argparse
import sys
import time

import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200

# How long the board must be quiet before the *initial* flush is considered
# done. Replies are no longer timed at all — see PROMPT — so this governs only
# the boot log and prompt noise cleared before the first command is sent.
QUIET_TIME = 0.4

# The board reprints this when a command finishes, so a reply is over when it
# appears — not when the line goes quiet.
#
# Silence was the original test and it is wrong for anything slow. `ota` writes
# a megabyte and a half to flash between progress lines, and a wrong URL sits on
# a ten-second connect timeout saying nothing at all; both look identical to a
# finished command. Two working updates were reported as truncated before this
# was fixed by asking the board rather than timing it.
#
# Owned by console.c's repl_config.prompt; kept as one constant here so the two
# places this file needs it cannot disagree with each other.
PROMPT = "panda>"

# Default ceiling on any one command, raised per-invocation with --timeout.
# A backstop for a command that never returns a prompt at all; anything that
# legitimately takes longer than this should say so on the command line.
REPLY_TIMEOUT = 5.0


def drain(port: serial.Serial, settle: float) -> bytes:
    """Read until the board has been quiet for `settle` seconds, or the ceiling."""
    out = bytearray()
    started = time.time()
    last = started
    while time.time() - last < settle:
        chunk = port.read(256)
        if chunk:
            out += chunk
            last = time.time()
        if time.time() - started > REPLY_TIMEOUT:
            # Deliberately returns what it has rather than raising: a truncated
            # reply still tells the caller what the board said, and a chatty
            # board is not an error.
            break
    return bytes(out)


def read_reply(port: serial.Serial, ceiling: float) -> bytes:
    """Read until the prompt comes back, or the ceiling is hit."""
    out = bytearray()
    started = time.time()
    while time.time() - started < ceiling:
        chunk = port.read(256)
        if chunk:
            out += chunk
            # Only the tail is checked, so a command that merely mentions the
            # prompt in its own output cannot end its own reply early.
            if out.rstrip().endswith(PROMPT.encode()):
                break
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run commands on the board's serial console.")
    parser.add_argument("--port", default=PORT)
    parser.add_argument("--timeout", type=float, default=REPLY_TIMEOUT)
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    with serial.Serial(args.port, BAUD, timeout=0.1) as port:
        # Not asserted: this board wires neither line to EN/GPIO0, and driving
        # them would do nothing useful. Left explicit so nobody adds a reset
        # here expecting it to work — see CLAUDE.md on the two USB gestures.
        port.dtr = False
        port.rts = False

        # A newline first, to flush any half-typed line and force a fresh
        # prompt. Whatever comes back is boot log or prompt noise, not a reply
        # to anything asked, so it is discarded.
        port.write(b"\n")
        drain(port, QUIET_TIME)

        for command in args.commands:
            print(f"$ {command}")
            port.write(command.encode() + b"\r\n")
            reply = read_reply(port, args.timeout).decode(errors="replace")
            # The console echoes what was typed and re-prints the prompt around
            # the answer. Both are noise once the command is known, so only the
            # lines between them are shown.
            for line in reply.splitlines():
                stripped = line.strip()
                if not stripped or stripped == command:
                    continue
                print(f"  {stripped.removeprefix(PROMPT).strip()}")
            if not reply.rstrip().endswith(PROMPT):
                print("Timed out before the console prompt returned.", file=sys.stderr)
                return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
