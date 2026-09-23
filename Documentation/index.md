# Caffeinix internals

These documents describe the implemented kernel, not a promise of complete
Linux compatibility. Start with the [project README](../README.md) for host
dependencies, kernel builds, BusyBox, image creation, and interactive boot.
API contracts live beside declarations or definitions; component documents
explain how those interfaces fit together.

## Architecture and kernel services

- [OpenSBI and boot](opensbi.md): firmware boundary, DTB, hart startup, and
  guarded kernel stacks.
- [Memory management](memory-management.md): physical allocation, VMAs,
  faults, shared pages, copy-on-write, writeback, and reclaim.
- [Processes](process-lifecycle.md): first userspace entry, creation, exec,
  exit, waiting, credentials, signals, and terminal relationships.
- [Scheduling](scheduler.md): runnable-tree ordering, runtime accounting,
  wakeups, idle CPUs, and performance checks.
- [Synchronization](synchronization.md): locks, condition/wait protocols,
  timeouts, deferred work, futexes, and memory barriers.
- [User-program ABI](userspace.md): Linux RV64 syscall boundary, ELF loading,
  dynamic musl, TLS, and current limitations.
- [Logging, time, and diagnostics](logging-and-time.md): clock domains,
  timestamped records, console serialization, panic output, and snapshots.
- [Randomness](random.md): generator ownership, entropy sources, and the
  deliberately untrusted no-device fallback.
- [Common library helpers](core-library.md): intrusive containers,
  freestanding strings, ownership, and allocation boundaries.

## I/O subsystems

- [Device and serial framework](driver-model.md): discovery, binding,
  resources, interrupts, DMA, UART, TTY, and console handoff.
- [Block devices](block-devices.md): requests, completion callbacks,
  device lifetime, and synchronous filesystem-facing operations.
- [VFS and filesystems](filesystems.md): namespace objects, path resolution,
  descriptor I/O, pipes, ext4, tmpfs, procfs, devfs, and FAT adapters.
- [Network architecture](networking/architecture.md): interfaces, packet
  references, callbacks, queue state, and driver/stack separation.
- [VirtIO transport](networking/virtio.md): modern MMIO, split queues, DMA,
  descriptor ownership, and block/network bindings.
- [lwIP port](networking/lwip.md): OS callbacks, protocol-thread context,
  netif integration, and stack configuration.
- [Socket UAPI](networking/socket-uapi.md): Linux-facing descriptors,
  marshalling, poll, supported operations, and errors.

## Development

- [Comment and component-document rules](commenting.md).
- [Build and validation boundaries](build-and-test.md).
- [Test inventory and commands](../tests/README.md).

Imported libfdt, lwIP, lwext4, and FatFs retain their own licenses, comments,
and provenance records. Read the local adapter and its component document
before changing an imported library; do not restyle vendor code as part of
an unrelated kernel patch.
