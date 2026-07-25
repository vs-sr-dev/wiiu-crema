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
  - a held note is the same note on contiguous steps — but *which* runs are held
    was, for one version of the format, a thing no reader could recover.

That second one used to be a guess. chiproll distinguishes a note dragged across
two cells from two single notes side by side, and its first JSON export did not:
sixteen separate G3s and one G3 held for sixteen cells serialised to sixteen
identical cells, differing only in their index.

**`format_version: 2` closed it**, with the smallest possible change — a
boolean per cell, `"tie": true` on a continuation. Additive, so a reader that
ignores it behaves exactly as before, and it removes the old convention's one
real cost: because a tie is now stated rather than inferred from contiguity, the
same note *can* be re-struck on the very next cell.

This importer wanted that flag before it existed and looked for it under several
plausible names, falling back to `--runs` and reporting every run it had to
guess at. On a v2 file it guesses at nothing and the report says so.

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


# Cell fields that would say, explicitly, whether a cell continues the note
# before it. None of them exist in the exports seen so far — which is the whole
# problem below — but reading them costs nothing and the day one appears this
# importer becomes exact instead of assuming.
TIE_KEYS = ("tie", "held", "continue", "sustain")
ATTACK_KEYS = ("attack", "trigger", "start")


def cell_continues(cell):
    """True / False if the file says, None if the file does not say."""
    for k in TIE_KEYS:
        if k in cell and cell[k] is not None:
            return bool(cell[k])
    for k in ATTACK_KEYS:
        if k in cell and cell[k] is not None:
            return not bool(cell[k])
    return None


def runs(steps, assume_held=True):
    """Walk a channel's grid and yield (first cell, how many cells it holds,
    ambiguous).

    A run is contiguous cells carrying the same note *and* the same register —
    the register is what the chip is actually given, and two cells that
    disagree about it are two different sounds however they are spelled.

    Whether such a run is ONE held note or several attacks is answered by the
    cell's `tie` flag from format_version 2 onward. Before that the export could
    not answer it at all — sixteen separate G3s and one G3 dragged across
    sixteen cells serialise to the same sixteen identical cells — so a run with
    no flag is still merged or not according to `assume_held`, and reported as
    ambiguous either way. A silent guess about note lengths is a silent guess
    about the music."""
    out = []
    for st in steps:
        if st.get("note") is None:
            continue
        key = (st["note"], st.get("register"))
        told = cell_continues(st)
        if out:
            prev, length, ambiguous = out[-1]
            contiguous = (st["step"] == prev["step"] + length and
                          (prev["note"], prev.get("register")) == key)
            if contiguous and (told is True or (told is None and assume_held)):
                out[-1] = (prev, length + 1, ambiguous or told is None)
                continue
        out.append((st, 1, False))
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


def convert(session, steps_per_beat=4, gate=0.95, keep_cents=True,
            assume_held=True):
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
    ambiguities = []
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
        guessed = 0
        for step, length, ambiguous in runs(chan["steps"], assume_held):
            if length > 1:
                held += 1
            if ambiguous:
                guessed += 1
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
        if guessed:
            ambiguities.append((chan["id"], guessed))

    events.sort(key=lambda e: (e[0], e[2]))
    end_ms = int(round(step_count * step_ms))
    return {
        "format_version": session.get("format_version", 1),
        "bpm": bpm, "chip": chip, "channels": used,
        "instruments": instruments, "events": events,
        "end_ms": end_ms, "report": report, "ambiguities": ambiguities,
        "assume_held": assume_held,
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
              "[--steps-per-beat=4] [--gate=0.95] [--equal-temperament] "
              "[--runs=held|attacks]")
        raise SystemExit(2)

    with open(args[0]) as fh:
        session = json.load(fh)
    song = convert(session,
                   steps_per_beat=int(flags.get("--steps-per-beat", 4)),
                   gate=float(flags.get("--gate", 0.95)),
                   keep_cents="--equal-temperament" not in flags,
                   assume_held=flags.get("--runs", "held") != "attacks")
    write_csong(args[1], song)

    print("importing %s session at %d BPM" % (song["chip"], song["bpm"]))
    for line in song["report"]:
        print(line)
    print("  %s: %d events, %d channels, %d instruments, %.2f s"
          % (args[1], len(song["events"]), song["channels"],
             len(song["instruments"]), song["end_ms"] / 1000.0))
    # Saying so out loud is the point: the difference between a file that states
    # its note lengths and one this importer had to read them out of is not
    # visible in the .csong, only in whether it is right.
    if not song["ambiguities"] and song["format_version"] >= 2:
        print("  the file states its own ties (format_version %d), so nothing "
              "above was guessed" % song["format_version"])
    if song["ambiguities"]:
        total = sum(n for _, n in song["ambiguities"])
        print("  NOTE: %d run%s of repeated cells (%s) carry no tie flag, so "
              "the file cannot say whether they are held notes or repeated "
              "attacks. Read as %s; --runs=%s for the other reading."
              % (total, "" if total == 1 else "s",
                 ", ".join("%s x%d" % (c, n) for c, n in song["ambiguities"]),
                 "HELD" if song["assume_held"] else "ATTACKS",
                 "attacks" if song["assume_held"] else "held"))


if __name__ == "__main__":
    main()
