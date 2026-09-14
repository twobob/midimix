# midimix

midimix mixes two MIDI files into one.

```
midimix a.mid b.mid
midimix -o out.mid a.mid b.mid
```

The output defaults to `mix.mid`. It is a format 1 file holding every track of both inputs.

- Ticks are rescaled to a common division.
- The first file's tempo and time signatures are kept; the second file's are dropped.
- A channel the second file shares with the first is moved to a free channel. Channel 10 (drums) is never moved.
- SMPTE divisions are not supported.

## Build

Pure C11. With Visual Studio 2022, run `build_midimix.bat`. Elsewhere:

```
cc -std=c11 -O2 -o midimix midimix.c
```
