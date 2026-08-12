# libfdt

This directory contains the read-only subset of libfdt from dtc v1.7.2,
commit `2d10aa2afe35527728db30b35ec491ecb6959e5c`.

Only the files required to validate and inspect a flattened Device Tree are
built. The imported code remains dual-licensed under GPL-2.0-or-later or
BSD-2-Clause. `libfdt_env.h` is adapted to the Caffeinix freestanding build;
the parser sources are otherwise unchanged.

See `UPSTREAM` for provenance and the precise imported file list.
