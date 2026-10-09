AHX container
=============

The reader in `ahx_read.c` is based on Stuart Caie's AHX format specification (15 March 2000)
and observed module files. The module data is big-endian and is read in place without allocating
or copying the container.


## Layout

```text
0                         header (14 bytes)
14                        subsong list (SS big-endian words)
14 + SS*2                 position list (LEN entries, 8 bytes each)
...                       tracks (stored_tracks * TRL * 3 bytes)
...                       instruments (SMP variable-length records)
...                       SMP+1 NUL-terminated names, through EOF
```

Each instrument record is 22 bytes followed by `PLEN` playlist entries of 4 bytes each; `PLEN`
is byte 21 of the record. `stored_tracks` is `TRK` when byte 6 bit 7 is set, otherwise
`TRK+1`.


## Specification differences

- Track 0:  
  The specification describes byte 6 bit 7 the other way around. In the observed
  files, a set bit means track 0 is absent from the file; a clear bit means all `TRK+1` tracks
  are stored.
- Speed:  
  The speed field is bits 6-5 of byte 6. Reading bits 6-4 produces values outside
  the documented four-value range.
- Title offset:  
  The 16-bit offset at bytes 4-5 matched the parsed name-block offset in all
  1,335 modules measured on 2026-10-08.

The filter modulation speed is read from the five bits used by the reference replayer. The
specification describes an additional bit in byte 19, but the reference does not use it.
Commands 6 and 7 are retained in cells even though they are not defined for AHX1.


## Names and malformed data

Names may not consume the file exactly. Some modules have fewer than `SMP+1` names or leave
trailing bytes. `ahx_name()` returns a pointer and a readable length, so unterminated names do
not require an out-of-bounds string operation. Track references past `TRK` are returned as
empty cells.

The parser rejects structures that do not fit in the input, including instrument playlists that
extend past the end. It accepts a shorter name block when the rest of the module is readable.
It is a reader, not a complete validator.

