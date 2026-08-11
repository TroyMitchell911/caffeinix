# Caffeinix

Caffeinix is a small Unix-like RISC-V operating system, written with an
Americano close at hand. It keeps its own kernel design while exposing a
Linux RISC-V UAPI subset to userspace.

The current milestone boots an unmodified, statically linked musl BusyBox on
QEMU `virt`. Basic `ash`, file, directory, and process operations work without
a private GCC, libc, or userspace syscall layer.

## Supported target

- Architecture: RISC-V 64-bit little-endian
- ISA and ABI: RV64GC with LP64D
- Machine: QEMU `virt`, one hart tested
- Userspace: static non-PIE musl ELF executables
- UAPI reference: Linux 6.10 RISC-V headers

## Prerequisites

Both supported hosts use the same standard `riscv64-linux-gnu-*` tools. The
Makefiles contain no distribution detection or distribution-specific build
path.

### On Ubuntu

Ubuntu 22.04 or later:

```bash
sudo apt update
sudo apt install \
  build-essential git \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
  libc6-dev-riscv64-cross qemu-system-misc
```

Install `gdb-multiarch` as well if you plan to use `make qemu-gdb`.

### On Arch

```bash
sudo pacman -S --needed \
  base-devel git riscv64-linux-gnu-gcc qemu-system-riscv
```

The kernel does not use musl, a libc sysroot, or BusyBox. Those are userspace
build inputs and are documented by the rootfs project.

## Getting the kernel source

```bash
git clone \
  https://github.com/TroyMitchell911/caffeinix.git \
  /absolute/path/to/caffeinix-kernel
```

The destination is arbitrary. The kernel does not assume a workspace layout.

## Building

```bash
make -C /absolute/path/to/caffeinix-kernel \
  -j"$(nproc)"
make -C /absolute/path/to/caffeinix-kernel check-uapi
```

Useful targets:

- `make`: build `output/kernel`
- `make check-uapi`: compare the supported ABI with Linux headers
- `make clean`: remove kernel build output

The optional `make build` target uses `bear` to generate a compilation
database. Normal builds do not require it.

## Building the root filesystem

Build userspace separately with
[caffeinix-rootfs](https://github.com/TroyMitchell911/caffeinix-rootfs).
Its README explains how to create an external musl sysroot. The rootfs build
requires both external paths explicitly:

```bash
make -C /absolute/path/to/caffeinix-rootfs \
  -j"$(nproc)" \
  MUSL_SYSROOT=/absolute/path/to/riscv64-linux-musl \
  BUSYBOX_DIR=/absolute/path/to/busybox-1.38.0
```

Neither path is passed to the kernel build.

## Running

Pass the completed image path explicitly:

```bash
make -C /absolute/path/to/caffeinix-kernel qemu \
  FS_IMG=/absolute/path/to/caffeinix-rootfs/fs.img
```

There is no default `FS_IMG`, so the kernel and rootfs may live anywhere.
QEMU attaches the selected file as a raw virtio block device. At the shell
prompt, try:

```sh
mkdir /demo
echo hello > /demo/message
cat /demo/message
ls /
rm /demo/message
```

Press `Ctrl-a`, then `x`, to leave QEMU. Replace `qemu` with `qemu-gdb` to stop
at reset and expose the QEMU GDB stub.

## Root filesystem image

`fs.img` is a raw, little-endian Caffeinix filesystem image derived from the
xv6 layout. It has 1 KiB blocks containing a superblock, journal, inode table,
free-block bitmap, and data blocks. It is not an ext4 image and is not expected
to mount directly on the host.

The generated root directory contains:

- `/busybox`: static musl BusyBox and all enabled standalone applets
- `/LICENSE`: the rootfs repository license text
- `/console`: the character device used for standard descriptors

## Current features

- Linux RISC-V syscall numbers and boundary structures for supported calls
- Linux-compatible static ELF startup stack and auxiliary vector
- Basic VFS operations and Linux file metadata conversion
- Sparse user mappings, `brk`, `mmap`, and `munmap`
- Basic `clone`, `execve`, `wait4`, descriptor, and process-ID operations
- Interactive BusyBox `ash`, plus `cat`, `cp`, `echo`, `ls`, `mkdir`, `pwd`,
  `rm`, and `touch`

## Current limitations

- Only a Linux RISC-V UAPI subset is implemented.
- Pipelines, job control, polling, and real signal delivery are not ready.
- Dynamic ELF loading, shared libraries, threads, and networking are not ready.
- Docker is not supported; it also needs namespaces, cgroups, mounts,
  networking, `/proc`, and a much wider syscall surface.
- Real boards still need platform boot, interrupt, timer, and device drivers.

## Contributing

Bug reports and focused changes are welcome through
[GitHub issues](https://github.com/TroyMitchell911/Caffeinix/issues).

## License

Caffeinix is distributed under the GNU GPL version 3. See [LICENSE](LICENSE).
