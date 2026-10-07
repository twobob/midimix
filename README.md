# midimix

midimix mixes MIDI files into one.

```
midimix a.mid b.mid
midimix -o out.mid a.mid b.mid c.mid
midimix songs
midimix --no-suno-fudge songs
midimix -R library
```

With files, the output defaults to `mix.mid`. Given a single folder, midimix mixes every `.mid` and `.midi` file
directly inside it (not in subfolders), in name order, and writes the folder's name plus `.mid` inside the folder:
`midimix songs` writes `songs/songs.mid`, wherever it is run from. `-o` overrides either default. The folder's own
`<folder>.mid` is always skipped, even with `-o`, and so is the `-o` file when it lies in the folder; the output is
overwritten at the end, so a folder can be re-merged as often as needed. The output is a format 1 file holding every
track of every input.

`-R` (or `--recurse`) with a folder walks it and every subfolder, and mixes each folder that holds MIDI files into
its own `<name>.mid`, from that folder's files only. Hidden folders (names starting with `.`) and links are not
followed, and `-o` cannot be used with it. A bad MIDI file stops the walk; folders mixed before it keep their output.

- Ticks are rescaled to a common division.
- Global tempo merge, on by default: when the output's first track is not a tempo track (one with tempo or time
  signature events and no notes), and every track that carries tempo or time signature events carries the same ones,
  those events go into a new first track and are dropped from every other track. It is meant for stems, such as Suno's,
  that repeat the tempo map in every clip. `--no-global-tempo-merge`, or its alias `--no-suno-fudge`, turns it off.
- Otherwise the first file's tempo and time signatures are kept; every later file's are dropped. When the first file
  already opens with a tempo track, midimix says so and warns about every track whose tempo map differs from it.
- A channel a later file shares with an earlier one is moved to a free channel. Channel 10 (drums) is never moved.
- When a file's channels no longer fit in the 16, the whole file goes onto the next MIDI port, and every track opens
  with a port event (`FF 21`). DAWs such as Reaper and Cubase honour it; most General MIDI players ignore it and play
  every port on one set of 16 channels. When everything fits, no port events are written.
- An input's own port events are dropped, so an input that already spans several ports is treated as one port.
- SMPTE divisions are not supported.

## Build

Pure C11. With Visual Studio 2022, run `build_midimix.bat`. Elsewhere:

```
cc -std=c11 -O2 -o midimix midimix.c
```

## Binaries

Each release carries both binaries, built from the same `midimix.c`:

- `midimix.exe`: Windows x64, MSVC 2022, `build_midimix.bat`.
- `midimix-linux-x86_64`: Linux x86-64, gcc 11.4 on Ubuntu 22.04:

```
gcc -std=c11 -O2 -Wall -Wextra -o midimix midimix.c
```

The two write byte-identical MIDI from the same inputs: midimix uses integer arithmetic only, so no compiler or flag
changes its output.
