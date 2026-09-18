# Change Log

## PicoVerse 2350 WiFi BIOS v1.3

- Fixed EXTBIO hook initialization to fill 11 bytes starting at `EXTBIO+4`, including the fifth hook byte and both DISINT and ENAINT hooks, with `RET`. This prevents stale hook data from executing when Nextor copies and chains the hook. Ported from [cristianoag/msx-picoverse-public#71](https://github.com/cristianoag/msx-picoverse-public/pull/71/changes/67d02b278678c169409bf95a4c20a05acf1a7bc8) and included in Explorer v2.50.
