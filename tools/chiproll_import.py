#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
"""Turn a chiproll session into a .csong.

chiproll (https://github.com/vs-sr-dev/chiproll) is a piano roll for chip music
that knows what the chip actually does. Its export carries, per step, both the
note you meant and the register the chip would be given — and, because those
two things do not agree, how many cents apart they are.

That disagreement is the point. A NES makes its pitch by dividing a clock by an
integer, so it cannot play in tune: E6 comes out 3.3 cents flat, every time,
and every NES ever built is flat by exactly the same amount. Import the notes
and you get the tune; import the notes *and the cents* and you get the machine.
Which is why this importer applies `cents_offset` by default and needs a flag to
stop.

    python tools/chiproll_import.py session.json out.csong

Two rules that are not written in the file but are part of the format,
confirmed by its author:

  - **an empty step is a rest, not a held note.** This is chiproll's convention
    on purpose, taken from DAW piano rolls and against the tracker habit:
    FamiTracker makes you write the rest, and leaving the cell blank sustains
    what came before. Here blank means silence.
  - **a held note is the same note repeated over contiguous steps.** Two cells
    of C#4 are one note two cells long, not two attacks — the grid is
    run-length encoded, the way a piano roll's rectangle looks when you drag
    its edge. So this importer coalesces runs rather than counting them, and
    the price of that grammar is that a note cannot be immediately retriggered
    at the same pitch: to strike it twice you leave a cell between.

The two conventions cooperate exactly because the second needs *contiguous*
cells: notes of the same pitch separated by a rest stay separate attacks.

A step is a sixteenth note by default (--steps-per-beat), which is the one
thing chiproll leaves to the reader. --gate sets how much of a note's last
step is held before the note-off.
"""

import json
import math
import os
import struct
import sys

VERSION = 1
HEADER_SIZE = 32
NAME_SIZE = 24
NOTE_OFF, NOTE_ON = 0, 1

SEMITONES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

# Which of our instruments stands in for each chip channel, and how loud it
# sits in the mix. A bass line and a hi-hat at the same volume is not a mix.
CHANNEL_MAP = {
    "NES": {
        "pulse1":   ("pulse12", 40),
        "pulse2":   ("pulse25", 34),
        "triangle": ("triangle", 46),
        "noise":    ("noise", 26),
    },
    "TIA": {
        "ch0": ("pulse50", 40),
        "ch1": ("pulse25", 36),
    },
    "POKEY": {
        "ch0": ("pulse50", 38), "ch1": ("pulse25", 34),
        "ch2": ("triangle", 40), "ch3": ("noise", 26),
    },
}

# NTSC NES noise periods by register. The channel has no pitch, only sixteen
# speeds, so the register becomes a transposition of our noise sample: register
# 8 is "as recorded" and everything else is faster or slower than it.
NES_NOISE_PERIODS = [4, 8, 16, 32, 64, 96, 128, 160,
                     202, 254, 380, 508, 762, 1016, 2034, 4068]
NES_NOISE_REFERENCE = 8


def midi_from_name(name):
    """'G#5' -> 80. Returns None for anything that is not a note name."""
    if not name or name[0].upper() not in SEMITONES:
        return None
    step = SEMITONES[name[0].upper()]
    i = 1
    while i < len(name) and name[i] in "#b":
        step += 1 if name[i] == "#" else -1
        i += 1
    try:
        return step + (int(name[i:]) + 1) * 12
    except ValueError:
        return None


def runs(steps):
    """Walk a channel's grid and yield (first step, how many cells it holds).

    The same note on contiguous cells is one held note. Identity is the note
    *and* its register, because the register is what the chip is actually given
    and two cells that disagree about it are two different sounds however they
    are spelled."""
    out = []
    for st in steps:
        if st.get("note") is None:
            continue
        key = (st["note"], st.get("register"))
        if out:
            prev, length = out[-1]
            if (st["step"] == prev["step"] + length and
                    (prev["note"], prev.get("register")) == key):
                out[-1] = (prev, length + 1)
                continue
        out.append((st, 1))
    return out


def noise_note(register):
    """A noise register as a note our sampler can transpose to."""
    if register is None or not (0 <= register < len(NES_NOISE_PERIODS)):
        return 60, 0
    ratio = (NES_NOISE_PERIODS[NES_NOISE_REFERENCE] /
             NES_NOISE_PERIODS[register])
    semitones = 12.0 * math.log2(ratio)
    note = 60 + semitones
    return int(round(note)), int(round((note - round(note)) * 100))


def convert(session, steps_per_beat=4, gate=0.95, keep_cents=True):
    bpm = session.get("bpm", 120)
    chip = session.get("active_chip", "NES")
    mapping = CHANNEL_MAP.get(chip)
    if not mapping:
        raise SystemExit("no instrument mapping for chip %r" % chip)

    step_ms = 60000.0 / bpm / steps_per_beat
    step_count = session.get("step_count", 16)

    soloed = [c for c in session["channels"] if c.get("solo")]
    channels = soloed if soloed else session["channels"]

    instruments, events = [], []
    used = 0
    report = []
    for chan in channels:
        if chan.get("muted"):
            continue
        entry = mapping.get(chan["id"])
        if not entry:
            report.append("  %s: no instrument for this channel, skipped"
                          % chan["id"])
            continue
        inst_name, volume = entry
        if inst_name not in instruments:
            instruments.append(inst_name)
        inst_index = instruments.index(inst_name)
        out_channel = used
        used += 1

        notes = 0
        detuned = 0
        held = 0
        for step, length in runs(chan["steps"]):
            if length > 1:
                held += 1
            if chan.get("kind") == "noise":
                note, cents = noise_note(step.get("register"))
            else:
                note = midi_from_name(step["note"])
                if note is None:
                    continue
                cents = step.get("cents_offset") or 0.0
                if not keep_cents:
                    cents = 0.0
                if abs(cents) > 0.5:
                    detuned += 1
            cents = max(-127, min(127, int(round(cents))))
            t0 = int(round(step["step"] * step_ms))
            # the gate eats into the LAST step of the run, so a note two cells
            # long is two cells long and not two notes of one
            t1 = int(round((step["step"] + length - (1.0 - gate)) * step_ms))
            events.append((t0, out_channel, NOTE_ON, note, inst_index,
                           volume, cents))
            events.append((t1, out_channel, NOTE_OFF, note, inst_index, 0, 0))
            notes += 1
        report.append("  %-9s -> %-9s %3d notes%s%s"
                      % (chan["id"], inst_name, notes,
                         ", %d held" % held if held else "",
                         ", %d off the ideal pitch" % detuned if detuned else ""))

    events.sort(key=lambda e: (e[0], e[2]))
    end_ms = int(round(step_count * step_ms))
    return {
        "bpm": bpm, "chip": chip, "channels": used,
        "instruments": instruments, "events": events,
        "end_ms": end_ms, "report": report,
    }


def write_csong(path, song):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(struct.pack(">4s6I", b"CSNG", VERSION, len(song["events"]),
                             len(song["instruments"]), 0, song["end_ms"],
                             song["channels"]))
        fh.write(b"\0" * (HEADER_SIZE - fh.tell()))
        for name in song["instruments"]:
            fh.write(name.encode("ascii")[:NAME_SIZE - 1].ljust(NAME_SIZE, b"\0"))
        for (t, ch, typ, note, inst, vol, cents) in song["events"]:
            fh.write(struct.pack(">IBBBBBbBB", t, ch, typ, note, inst, vol,
                                 cents, 0, 0))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a.split("=")[0]: a.split("=")[-1]
             for a in sys.argv[1:] if a.startswith("--")}
    if len(args) != 2:
        print(__doc__ or "")
        print("usage: chiproll_import.py <session.json> <out.csong> "
              "[--steps-per-beat=4] [--gate=0.95] [--equal-temperament]")
        raise SystemExit(2)

    with open(args[0]) as fh:
        session = json.load(fh)
    song = convert(session,
                   steps_per_beat=int(flags.get("--steps-per-beat", 4)),
                   gate=float(flags.get("--gate", 0.95)),
                   keep_cents="--equal-temperament" not in flags)
    write_csong(args[1], song)

    print("importing %s session at %d BPM" % (song["chip"], song["bpm"]))
    for line in song["report"]:
        print(line)
    print("  %s: %d events, %d channels, %d instruments, %.2f s"
          % (args[1], len(song["events"]), song["channels"],
             len(song["instruments"]), song["end_ms"] / 1000.0))


if __name__ == "__main__":
    main()
