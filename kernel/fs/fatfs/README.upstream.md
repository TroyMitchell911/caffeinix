# FatFs provenance

This directory vendors ChaN FatFs R0.16 from the official release archive.
Official patches 1 and 2 are applied to `ff.c`; patch 2 includes the current
R0.16 bounds-validation fixes. The imported text files use Unix line endings.

The Caffeinix-specific files are `ffconf.h`, `diskio.c`, `caffeinix.c`, the
compatibility headers, and the local build file. Imported text uses Unix line
endings and has trailing whitespace removed. `UPSTREAM` records source URLs
and checksums. See `LICENSE` for the upstream license.
