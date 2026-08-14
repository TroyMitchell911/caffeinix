# Caffeinix tests

The test tree contains host-side ABI checks and guest-side selftests. Normal
kernel builds do not download or build any userspace components.

Run the Linux RISC-V UAPI compile-time checks with:

```bash
make -C tests uapi
```

Run the host red-black-tree and VirtIO split-ring tests with:

```bash
make -C tests rbtree virtqueue
```

The tree test performs 20,000 deterministic randomized insert and erase
operations while checking ordering and red-black invariants. The virtqueue
test covers scatter/gather chains, descriptor exhaustion, completion
lengths, notification, and 16-bit ring wraparound.

Run the complete QEMU test with:

```bash
make -C tests qemu
```

The QEMU test downloads checksum-pinned musl 1.2.6 and BusyBox 1.38.0 source
archives, builds both outside the kernel, and creates temporary ext4 and FAT32
images under `output/tests`. It runs boot checks with one hart and 64 MiB, two
harts and 192 MiB, three harts and 96 MiB, four harts and 128 MiB, and eight
harts and 256 MiB, plus a nine-hart boot that exceeds the old static CPU
limit. A second eight-hart, 256 MiB run waits for BusyBox ash, runs the full
guest suite, syncs storage, and exits QEMU through its serial monitor.
Finally, a test-only kernel deliberately crosses an unmapped kernel-stack
guard and must report the overflow from its per-CPU emergency stack.

Each boot requires one SBI BASE report and exactly one online and timer marker
per logical CPU. A static check rejects machine-mode CSR operations, direct
CLINT access, `mret`, `-bios none`, and a kernel entry other than
`0x80200000`.

The full eight-hart run attaches the network device before two VirtIO block
devices.
This verifies that transport enumeration cannot change root or data-disk
selection. A host fixture behind QEMU user networking provides deterministic
UDP, TCP, reverse-TCP, bulk-transfer, and HTTP replies.

The guest selftest covers:

- network device registration, packet ownership, and queue state;
- repeated fork, exec, exit, and wait cycles;
- CFS runqueue progress with 24 runnable processes, timer preemption,
  weighted nice values, and more runnable work than CPUs;
- concurrent CPU, allocator, ext4, VirtIO completion, sleeplock, and process
  wait activity;
- concurrent CFS nice classes, ext4, tmpfs, FAT, and four TCP bulk clients;
- multiple TTY sleepers and repeated wake-all/requeue behavior;
- devfs character devices and device numbers;
- `/dev/ttyS0` metadata, `/dev/tty` error semantics, and terminal ioctls;
- termios set/get state, canonical echo and erase, raw input, CR/NL handling,
  blocking wakeups, and UART output larger than the transmit queue;
- ext4 and tmpfs links, sparse files, rename, directory iteration, truncate,
  fsync, and open-unlink lifetime rules;
- symlink metadata through `lstat`;
- FAT open-file restrictions, unsupported Unix links, overwrite rename, and
  UTF-8 long names;
- DHCP, raw ICMP, UDP, TCP clients and servers, blocking and nonblocking
  sockets, polling, metadata, options, shutdown, and close; and
- BusyBox `nc` and `wget`, including a 32 KiB transfer across packet,
  socket, pbuf, and virtqueue buffer boundaries.

The one-, two-, four-, and eight-hart smoke boots omit the NIC and exercise
UDP loopback. A separate two-hart boot places an unsupported VirtIO device
before the root disk and uses a socket-backed NIC without DHCP; it must still
reach the shell and pass loopback.

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

Run the same matrix against an externally built OpenSBI image with:

```bash
SBI_FIRMWARE=/absolute/path/to/fw_dynamic.bin make -C tests qemu
```

The test accepts the firmware as an input; it does not download or build
OpenSBI.
