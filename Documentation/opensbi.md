# OpenSBI boot contract

Caffeinix is an S-mode kernel. OpenSBI is the external M-mode runtime that
owns machine traps, PMP, interrupt delegation, machine timers, and hart
lifecycle. The kernel contains only an SBI client; it does not contain or
build firmware.

The QEMU boot chain is:

```text
QEMU reset ROM -> OpenSBI in M-mode -> Caffeinix in S-mode
                                      -> BusyBox in U-mode
```

## Entry and memory

OpenSBI enters the boot hart with paging disabled and the standard next-stage
registers:

```text
a0  physical hart ID
a1  physical address of the flattened Device Tree
```

The kernel image is linked at `0x80200000`, leaving the beginning of QEMU RAM
for resident firmware. RAM banks come from enabled `device_type = "memory"`
nodes. The allocator excludes the kernel image, the DT reservation map, and
children of `/reserved-memory`; it has no fixed 128 MiB limit.

Physical hart IDs are firmware identifiers and may be sparse or selected in
any boot order. Caffeinix assigns the boot hart logical CPU 0, assigns dense
logical IDs to the remaining enabled CPU nodes, and stores that logical ID in
`tp`. A physical hart ID is never used as an array or stack index.

## Required SBI extensions

The kernel requires SBI v0.2 or newer and probes extensions through BASE.
There is no legacy SBI fallback.

- TIME is always required. The kernel reads `/cpus/timebase-frequency`, calls
  `set_timer` with an absolute deadline, handles supervisor timer cause 5,
  and rearms the next event on every online hart.
- HSM is required when the Device Tree contains more than one enabled CPU.
  The boot hart checks each secondary state and calls `hart_start` with the
  S-mode secondary entry and its logical ID as the opaque value.

Every secondary starts with paging disabled, selects a stack by logical ID,
validates the physical-to-logical mapping, installs the shared kernel page
table, resolves its PLIC context from `interrupts-extended`, arms SBI TIME,
and enters the scheduler. Startup and first-timer waits are bounded so a
firmware or DT mismatch fails with a diagnostic instead of hanging.

The kernel does not use SBI console calls for normal output. The early UART
and normal NS16550 driver remain DT-driven kernel facilities.

## QEMU firmware selection

The normal command uses `-bios default`, which selects the OpenSBI firmware
distributed with QEMU:

```bash
make qemu FS_IMG=/absolute/path/to/root.ext4
```

An external dynamic OpenSBI image can be supplied without rebuilding the
kernel:

```bash
make qemu \
  SBI_FIRMWARE=/absolute/path/to/fw_dynamic.bin \
  FS_IMG=/absolute/path/to/root.ext4
```

For example, build a pinned upstream release outside this repository:

```bash
git clone --depth 1 --branch v1.7 \
  https://github.com/riscv-software-src/opensbi.git \
  /absolute/path/to/opensbi-v1.7

make -C /absolute/path/to/opensbi-v1.7 -j"$(nproc)" \
  CROSS_COMPILE=riscv64-linux-gnu- \
  PLATFORM=generic
```

The resulting image is
`build/platform/generic/firmware/fw_dynamic.bin`. Real hardware may load the
same S-mode kernel through a ROM, SPL, or bootloader, provided the firmware
supplies the entry registers, DT description, accessible RAM, SBI TIME, and
SBI HSM contract described above.

## Validation

`make check-opensbi` checks the ELF entry and disassembly and rejects direct
CLINT or machine-mode operations. `make -C tests qemu` adds 1-, 2-, 4-, and
8-hart boots at several RAM sizes, verifies every hart's timer and online
transition, and runs the existing BusyBox, TTY, ext4, devfs, tmpfs, and FAT
regression suite. CI runs the Linux UAPI layout check separately.
