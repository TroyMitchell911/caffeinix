# Caffeinix tests

The test tree contains host-side ABI checks and guest-side selftests. Normal
kernel builds do not download or build any userspace components.

Run the Linux RISC-V UAPI compile-time checks with:

```bash
make -C tests uapi
```

Run the complete QEMU test with:

```bash
make -C tests qemu
```

The QEMU test downloads checksum-pinned musl 1.2.6 and BusyBox 1.38.0 source
archives, builds both outside the kernel, and creates temporary ext4 and FAT32
images under `output/tests`. It then boots Caffeinix, waits for BusyBox ash,
runs `fs_runtime`, syncs storage, and exits QEMU through its serial monitor.

The guest selftest covers:

- devfs character devices and device numbers;
- ext4 and tmpfs links, sparse files, rename, directory iteration, truncate,
  fsync, and open-unlink lifetime rules;
- symlink metadata through `lstat`;
- FAT open-file restrictions, unsupported Unix links, overwrite rename, and
  UTF-8 long names.

After QEMU exits, the host harness recovers and checks ext4 with `e2fsck`,
checks FAT32 with `fsck.fat`, reads persistent values from both images, and
verifies that tmpfs data did not reach the ext4 image.

Required host commands are `riscv64-linux-gnu-*`, `qemu-system-riscv64`,
`curl`, `expect`, `mke2fs`, `e2fsck`, `debugfs`, `mkfs.fat`, and `mtools`.
The GitHub Actions workflow installs these dependencies automatically.
