#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
"""Write a .csong — a piano roll, not a tracker pattern.

A song here is a flat list of events sorted in time: this note starts on this
channel at this millisecond, this note stops. That is deliberately the same
model a piano roll editor has, so importing one later is a translation and not
a redesign — no patterns, no order table, no rows, nothing that exists because
tracker software of 1989 had 64-row memory pages.

Times are in milliseconds rather than ticks or rows: the player adds the length
of one AX frame per callback and compares. That keeps the file independent of
what the audio hardware happens to run at, which is the same reason the mesh
format does not know what a vertex shader is.

Instruments are referenced by name against whatever bank is loaded, so a song
and a bank can be rebuilt separately.

    python tools/gen_song.py examples/poc11-flight/assets/audio/theme.csong
"""

import os
import struct
import sys

VERSION = 1
HEADER_SIZE = 32
EVENT_SIZE = 12

NOTE_OFF, NOTE_ON = 0, 1

SEMITONES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def n(name):
    """'D3' -> MIDI number. 'D#3' and 'Db3' both work; 69 is A4 = 440 Hz."""
    step = SEMITONES[name[0].upper()]
    i = 1
    while i < len(name) and name[i] in "#b":
        step += 1 if name[i] == "#" else -1
        i += 1
    return step + (int(name[i:]) + 1) * 12


class Song:
    def __init__(self, bpm, channels):
        self.bpm = bpm
        self.channels = channels
        self.events = []          # (timeMs, channel, type, note, inst, vol)
        self.instruments = []

    def beat_ms(self, beats):
        return int(round(beats * 60000.0 / self.bpm))

    def inst(self, name):
        if name not in self.instruments:
            self.instruments.append(name)
        return self.instruments.index(name)

    def play(self, channel, instrument, note, start_beats, length_beats,
             volume=48, detune=0):
        """One note on a piano roll: where it starts, how long it is held.

        `detune` is in cents and is how a real chip's out-of-tuneness survives
        the trip: its pitch comes from an integer divider, so its E6 is a few
        cents flat and always the same few cents flat."""
        idx = self.inst(instrument)
        midi = n(note) if isinstance(note, str) else note
        t0 = self.beat_ms(start_beats)
        t1 = self.beat_ms(start_beats + length_beats)
        cents = max(-127, min(127, int(round(detune))))
        self.events.append((t0, channel, NOTE_ON, midi, idx, volume, cents))
        self.events.append((t1, channel, NOTE_OFF, midi, idx, 0, 0))

    def line(self, channel, instrument, notes, start_beats, step_beats,
             length=None, volume=48):
        """A run of evenly spaced notes; None leaves a gap."""
        length = step_beats * 0.9 if length is None else length
        for i, note in enumerate(notes):
            if note is not None:
                self.play(channel, instrument, note,
                          start_beats + i * step_beats, length, volume)

    def write(self, path, loop_beats=0.0, end_beats=None):
        self.events.sort(key=lambda e: (e[0], e[2]))   # note-offs before ons
        end = self.beat_ms(end_beats) if end_beats is not None \
            else max(e[0] for e in self.events)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(struct.pack(">4s6I", b"CSNG", VERSION, len(self.events),
                                 len(self.instruments), self.beat_ms(loop_beats),
                                 end, self.channels))
            fh.write(b"\0" * (HEADER_SIZE - fh.tell()))
            for name in self.instruments:
                fh.write(name.encode("ascii")[:23].ljust(24, b"\0"))
            for (t, ch, typ, note, inst, vol, cents) in self.events:
                fh.write(struct.pack(">IBBBBBbBB", t, ch, typ, note, inst, vol,
                                     cents, 0, 0))
        print("  %s: %d events, %d channels, %d instruments, %.1f s, loops at %.1f s"
              % (path, len(self.events), self.channels, len(self.instruments),
                 end / 1000.0, self.beat_ms(loop_beats) / 1000.0))


def build():
    """Four bars in D minor at 132 BPM — the tempo of something being chased.

    Four voices, which is what the chip this imitates would have had, except
    that here the limit is 96 and the restraint is a choice."""
    s = Song(bpm=132, channels=4)

    for bar in range(4):
        b = bar * 4.0

        # bass: the root on every eighth, walking on the last bar so the loop
        # arrives somewhere instead of just stopping
        roots = ["D2", "D2", "D2", "D2", "A2", "A2", "F2", "F2"]
        if bar == 3:
            roots = ["D2", "D2", "C2", "C2", "A#1", "A#1", "A1", "C2"]
        s.line(0, "pulse50", roots, b, 0.5, length=0.42, volume=44)

        # arpeggio: sixteenths through the chord of the bar. A chip arpeggio is
        # a chord you cannot afford, played fast enough to be mistaken for one.
        chords = {0: ["D3", "F3", "A3", "F3"], 1: ["D3", "F3", "A3", "F3"],
                  2: ["C3", "F3", "A3", "F3"], 3: ["A#2", "D3", "F3", "D3"]}
        arp = chords[bar] * 4
        s.line(1, "pulse25", arp, b, 0.25, length=0.22, volume=26)

        # lead: enters on the second bar, so the first is the engine's
        melody = {
            1: [("A4", 1.0), ("F4", 0.5), ("G4", 0.5), ("A4", 2.0)],
            2: [("D5", 1.5), ("C5", 0.5), ("A4", 1.0), ("F4", 1.0)],
            3: [("G4", 0.5), ("A4", 0.5), ("D5", 2.0), (None, 1.0)],
        }.get(bar, [])
        t = b
        for note, length in melody:
            if note:
                s.play(2, "pulse12", note, t, length * 0.92, volume=38)
            t += length

        # percussion: the noise instrument is not pitched, so the "note" only
        # picks how fast its LFSR loop is read — low is a kick, high is a hat
        s.line(3, "noise", ["C2", None, "C4", None, "C2", "C2", "C4", None],
               b, 0.5, length=0.12, volume=30)

    return s


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "theme.csong"
    build().write(out, loop_beats=0.0, end_beats=16.0)


if __name__ == "__main__":
    main()
