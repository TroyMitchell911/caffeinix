# Randomness

## Interface

The kernel owns a single locked ChaCha20 generator.  Each request advances
and replaces its key before returning bytes, so callers on different CPUs
cannot reuse one output block.  The generator supplies:

- the 16 bytes referenced by the Linux `AT_RANDOM` auxiliary-vector entry;
- Linux RISC-V `getrandom(2)` with `GRND_NONBLOCK`, `GRND_RANDOM`, and
  `GRND_INSECURE` flag validation; and
- kernel callers through `get_random_bytes()` and `get_random_u64()`.

This interface is a kernel service.  libc continues to provide the public
function wrappers and does not require Caffeinix-specific source changes.

## Boot seeding

Early boot always mixes timer, memory-layout, and kernel-address state so the
generator can operate when a platform has no entropy device.  These values
are not credited as entropy.  Output from this fallback keeps development
systems usable but must not be used for cryptographic keys or secrets; boot
reports `random: using untrusted boot-time seed` when it is active.

The VirtIO entropy driver contributes 256 credited bits before userspace
starts.  A successful seed changes the report to
`random: crng initialized`.  QEMU supplies this device from `/dev/urandom` by
default.  `RNG_BACKEND=` deliberately removes it, and `RNG_BUS` chooses its
VirtIO MMIO slot.

A real platform should register a hardware entropy driver and call
`random_add_hardware()` before `random_finalize_boot()`.  The driver must only
credit data whose unpredictability is guaranteed by its hardware and trust
boundary.

## Current policy

Boot does not block indefinitely when no trusted source exists.  All
supported `getrandom(2)` modes return from the weak fallback after the warning;
`GRND_NONBLOCK` therefore does not return `EAGAIN` merely because hardware
entropy is absent.  This is an explicit availability-over-confidentiality
policy for the current hobby-system scope, not a claim that the fallback is
cryptographically secure.
