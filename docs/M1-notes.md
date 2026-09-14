# M1 notes

Running log of surprises and resolved unknowns (spec §13).

## T01

- FBInk submodule pinned to `v1.25.0`. The device's own fbink binary reports `84bfe3b`,
  which isn't an upstream commit (probably a libkh build), so it can't be pinned exactly.
- Toolchain flags: `-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard` (U1, see DEVICE_FACTS).
- Device glibc is 2.20 (`ld-2.20.so`): the cross sysroot's glibc must not be newer,
  or binaries fail with `GLIBC_2.xx not found`.
