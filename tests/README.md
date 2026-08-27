# Caffeinix tests

The test tree contains host-side ABI checks and guest-side selftests. Normal
kernel builds do not download or build any userspace components.

Run the Linux RISC-V UAPI compile-time checks with:

```bash
make -C tests uapi
```

Run the host ELF, memory-management, VMA, red-black-tree, and VirtIO split-ring
tests with:

```bash
make -C tests elf memrange buddy sv39 rbtree vma virtqueue
```

The memory tests cover range normalization, buddy splitting and coalescing,
allocation orders, invalid and duplicate frees, randomized reuse, and Sv39
leaf selection. The VMA test covers ordered insertion, merging, address hints,
top-down gap selection, protection splits, partial removal, file offsets, and
clone ownership. The ELF test covers PIE load bias, program-header and entry
relocation, segment alignment, overlap and permissions, executable-stack
requests, integer overflow, and interpreter path validation. The tree test
performs 20,000 deterministic randomized
insert and erase operations while checking ordering and red-black invariants.
The virtqueue test covers scatter/gather chains, descriptor exhaustion,
completion lengths, notification, and 16-bit ring wraparound.

Run the complete QEMU test with:

```bash
make -C tests qemu
```

The QEMU test downloads checksum-pinned musl 1.2.6 and BusyBox 1.38.0 source
archives, builds both outside the kernel, and creates temporary ext4 and FAT32
images under `output/tests`. It runs boot checks with one hart and 64 MiB, two
harts and 192 MiB, three harts and 96 MiB, four harts and 128 MiB, and eight
harts and 256 MiB, plus a nine-hart boot that exceeds the old static CPU
limit. A one-hart, 4 GiB boot requires early memory setup and Sv39 activation
to finish within ten seconds; the guest stays below one quarter of the
standard public GitHub runner's RAM. A second eight-hart, 256 MiB run waits
for BusyBox ash, runs the full guest suite, syncs storage, and exits QEMU
through its serial monitor. Finally, a test-only kernel deliberately crosses
an unmapped kernel-stack guard and must report the overflow from its per-CPU
emergency stack.

The low-level dynamic ELF handoff tests retain minimal relocation-free ET_DYN
and ET_EXEC interpreters. One main image starts at a high virtual address and
requires 2 MiB PT_LOAD alignment. Permission fixtures exercise an executable
stack and a permissionless guard segment. In addition, the image contains the
unmodified musl 1.2.6 runtime linker and shared libc, dynamically linked test
programs, shared-object fixtures, a dynamic BusyBox, and a static recovery
BusyBox. Both BusyBox binaries use the checked broad configuration and must
report exactly the 207 applets in `configs/busybox.applets`.

Each boot requires one SBI BASE report and exactly one online and timer marker
per logical CPU. A static check rejects machine-mode CSR operations, direct
CLINT access, `mret`, `-bios none`, and a kernel entry other than
`0x80200000`.

The full eight-hart run attaches the network device before two VirtIO block
devices.
This verifies that transport enumeration cannot change root or data-disk
selection. A host fixture behind QEMU user networking provides deterministic
UDP, TCP, reverse-TCP, bulk-transfer, and HTTP replies.

The Expect harnesses launch guests through the top-level `qemu` target, so
interactive boots and selftests share one QEMU machine configuration. The
boot-smoke matrix passes an empty `NET_BACKEND` to omit the NIC. A separate
offline boot keeps the NIC but replaces user networking with a socket backend
that has no DHCP server.

The guest selftest covers:

- network device registration, packet ownership, and queue state;
- repeated fork, exec, exit, and wait cycles;
- execution of an ELF image whose text and data PT_LOAD segments share a
  page;
- dynamic ELF interpreter entry, relocated auxiliary-vector values, high-base
  and large-alignment placement, fixed-address interpreter loading,
  executable stacks, permissionless load segments, transfer to the
  main-program entry, and repeated atomic rejection of a missing interpreter;
- upstream musl relocation, `DT_NEEDED` lookup, constructors, destructors,
  initial-exec TLS, RELRO, `dlopen`, `dlsym`, `dlclose`, `pread64`,
  close-on-exec, and concurrent dynamic fork/exec;
- musl pthread creation, TLS, join, detach, mutexes, condition variables,
  barriers, robust-owner recovery, cancellation, futex operations, and
  private expedited memory barriers;
- process- and thread-directed Linux signals, RV64 integer and floating-point
  signal context, alternate and nested stacks, masks, pending signals,
  synchronous faults, stop/continue and child state, exec dispositions, and
  automatic child reaping;
- `SA_RESTART` and `EINTR` behavior for futex, wait, TTY read, and `ppoll`,
  including atomic temporary masks and multithreaded `exit_group` pressure;
- anonymous and private file mappings, address hints, fixed replacement,
  partial protection and removal, child fault isolation, fork isolation, and
  mapping lifetime, including kernel copies honoring mapping permissions;
- demand faults for anonymous, ELF, interpreter, and private file mappings,
  sharing of clean executable pages, page-cache accounting, and `SIGBUS` for
  pages wholly beyond the mapped file's end;
- copy-on-write isolation for anonymous and cached private mappings, kernel
  copies into child memory, fork under resident-memory pressure, and stale
  writable-TLB rejection while sibling threads run on other harts;
- shared file mappings, immediate alias and fork visibility, coherent
  positional and ordinary I/O, synchronous writeback, persistence, partial
  final pages, and cross-process invalidation after truncation;
- shared anonymous mappings faulted before and after `fork`, including VMA
  protection splits and partial unmapping;
- per-exec `AT_RANDOM`, Linux `getrandom` flags and errors, non-repeating
  output across calls and `fork`, a default VirtIO entropy source, and the
  explicitly warned no-device fallback;
- independently randomized PIE, interpreter, shared-library, anonymous mmap,
  heap, and stack addresses across repeated executions;
- W^X enforcement for ELF and runtime mappings, explicit executable-stack
  handling, safe write-to-execute transitions, `MAP_FIXED_NOREPLACE`, and
  supported stack, reservation, and prefault mapping flags;
- clean file-cache and private anonymous page eviction under a 64 MiB memory
  limit, including file reload, zero-page recreation, and dirty-page
  preservation;
- remote TLB invalidation after a sibling thread removes a user mapping;
- prompt release of exited processes' user pages while zombies await their
  parent's `wait4`, under memory pressure exceeding available guest RAM;
- read and positional-read copy faults through ext4, tmpfs, FAT, and
  character devices, plus concurrent reads through one ext4 file handle;
- CFS runqueue progress with 24 runnable processes, timer preemption,
  weighted nice values, and more runnable work than CPUs;
- concurrent CPU, allocator, ext4, VirtIO completion, sleeplock, and process
  wait activity;
- concurrent CFS nice classes, ext4, tmpfs, FAT, and four TCP bulk clients;
- multiple TTY sleepers and repeated wake-all/requeue behavior;
- devfs character devices and device numbers;
- `/dev/ttyS0` metadata, controlling `/dev/tty`, foreground-group and session
  ioctls, and background terminal access;
- termios set/get state, canonical echo and erase, raw input, CR/NL handling,
  `ISIG` control characters, `NOFLSH`, blocking wakeups, and UART output
  larger than the transmit queue;
- dynamic BusyBox ash startup, core applets, repeated process startup,
  command and UTF-8 pathname completion, completion listings, saved and
  reverse-searchable history, Home/End/Delete and control-key editing, long
  input, empty-line Ctrl-D, and cancellation of a partial command with Ctrl-C;
- broad BusyBox ash language, text, archive, file, account, process, and
  system-tool semantics, with the dynamic and static applet lists checked
  against the committed manifest;
- BusyBox vi insertion, persistence, exit, and reopen, foreground pipeline
  interruption, stopped and resumed jobs, background terminal reads, plus
  execution of the static recovery binary;
- ext4 and tmpfs links, sparse files, rename, directory iteration, truncate,
  fsync, and open-unlink lifetime rules;
- ext4 and tmpfs inode timestamps, nanosecond persistence, automatic access
  and modification updates, `utimensat`, `futimens`, `UTIME_OMIT`, and
  no-follow symlink updates;
- procfs process snapshots, command-line lifetime, process-exit races, mount,
  memory, uptime, scheduler, and IPv4 statistics, together with BusyBox
  `ps`, `top`, `free`, `uptime`, `pidof`, and `netstat` consumers;
- real, effective, saved, and filesystem credentials, supplementary groups,
  umask inheritance, file ownership, discretionary access, sticky-directory
  deletion, signal authorization, and credential auxv values across exec;
- filesystem capacity reporting, chmod/chown ownership updates, FIFO nodes,
  extent allocation, and raw virtio block-device discovery and I/O;
- symlink metadata through `lstat`;
- FAT open-file restrictions, unsupported Unix links, overwrite rename, and
  UTF-8 long names;
- DHCP, raw ICMP, UDP, TCP clients and servers, blocking and nonblocking
  sockets, polling, metadata, options, shutdown, and close; and
- BusyBox IPv4 status and routes, `ping`, deterministic musl DNS resolution,
  `nslookup`, DNS-backed `wget`, a loopback `httpd`, `nc`, and direct `wget`,
  including a 32 KiB transfer across packet, socket, pbuf, and virtqueue
  buffer boundaries.

Every smoke boot runs a dynamically linked hello program and concurrent
dynamic fork/exec pressure. The one-, two-, four-, and eight-hart cases cover
the supported SMP configurations; three- and nine-hart cases cover dynamic
CPU discovery. These boots omit the NIC and exercise UDP loopback. A separate
two-hart boot places an unsupported VirtIO device before the root disk and
uses a socket-backed NIC without DHCP; it must still reach the shell and pass
loopback.

After QEMU exits, the host harness recovers and checks ext4 with `e2fsck`,
checks FAT32 with `fsck.fat`, reads persistent values from both images, and
verifies that tmpfs data did not reach the ext4 image.

Required host commands are `riscv64-linux-gnu-*`, `qemu-system-riscv64`,
`curl`, `expect`, `mke2fs`, `e2fsck`, `debugfs`, `mkfs.fat`, `mtools`, and
`python3`.
The firmware audit also requires `rg`. The GitHub Actions workflow installs
these dependencies automatically.

Measure one-CPU and eight-CPU command latency and verify that an idle
eight-CPU guest consumes less than one host CPU with:

```bash
make -C tests scheduler-perf
```

This target builds a fresh test root image before measuring it. The
benchmark reports 13-sample medians for a shell builtin, `pwd`, and
`ls` on tmpfs, devfs, and ext4. Absolute command latency and the SMP ratio are
reported rather than used as CI gates because shared-runner timing is noisy.
Idle QEMU CPU usage is gated because the pre-fix scheduler consistently
consumed one host CPU per guest CPU while doing no work.

Measure static and dynamic musl startup, resident memory, physical file-page
sharing, and sequential `fork()` cost with:

```bash
make -C tests memory-perf
```

The benchmark runs identical statically and dynamically linked programs in a
one-CPU, 256 MiB guest. It uses the kernel emergency state dump to compare
allocator occupancy and page-cache references while processes are alive and
after they exit. CI applies broad timing limits to catch deadlocks and severe
regressions while tolerating shared-runner noise. Physical sharing and
resident-page limits are deterministic gates; twelve concurrent dynamic
processes must share their musl and executable file pages.

Run the same matrix against an externally built OpenSBI image with:

```bash
SBI_FIRMWARE=/absolute/path/to/fw_dynamic.bin make -C tests qemu
```

The test accepts the firmware as an input; it does not download or build
OpenSBI.
