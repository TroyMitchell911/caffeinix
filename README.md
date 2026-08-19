# Caffeinix

Caffeinix is a small Unix-like RISC-V operating system, written with an
Americano close at hand. The kernel keeps its own internal design while
exposing the Linux RISC-V userspace ABI needed by musl programs.

The current milestone boots an unmodified dynamically linked BusyBox and
upstream musl runtime linker from an ext4 root filesystem on QEMU `virt`.
The kernel mounts devfs at `/dev`, tmpfs at `/tmp`, can mount a second FAT16
or FAT32 disk at `/mnt/fat`, and can run IPv4 over a modern VirtIO network
device through lwIP.

## Supported target

- Architecture: RISC-V 64-bit little-endian
- ISA and ABI: RV64GC with LP64D
- Machine: QEMU `virt`, one, two, four, and eight harts tested
- Firmware: OpenSBI with SBI v0.2 or newer, TIME, HSM, and IPI for SMP
- Userspace: dynamically linked and static musl ELF executables
- Root filesystem: ext4 with 1 KiB filesystem blocks
- Optional data filesystem: FAT16 or FAT32
- Serial console: DT-discovered NS16550A at `/dev/ttyS0` (device 4:64)
- Network: optional DT-discovered modern VirtIO MMIO NIC, Ethernet and IPv4
- UAPI reference: Linux 6.10 RISC-V headers

## Prerequisites

Both supported hosts use the same `riscv64-linux-gnu-*` tools and the same
build commands. The Makefiles contain no host-distribution detection.

On Ubuntu 22.04 or later:

```bash
sudo apt update
sudo apt install \
  build-essential git curl \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
  qemu-system-misc e2fsprogs ripgrep
```

On Arch:

```bash
sudo pacman -S --needed \
  base-devel git curl riscv64-linux-gnu-gcc \
  qemu-system-riscv e2fsprogs ripgrep
```

Install `gdb-multiarch` on Ubuntu or `riscv64-elf-gdb` on Arch only when
using `make qemu-gdb`. The complete test suite additionally needs `expect`,
`dosfstools`, `mtools`, and Python 3.

On Ubuntu:

```bash
sudo apt install expect dosfstools mtools python3
```

On Arch:

```bash
sudo pacman -S --needed expect dosfstools mtools python
```

## Build the kernel

```bash
git clone \
  https://github.com/TroyMitchell911/caffeinix.git \
  /absolute/path/to/caffeinix

export CAFFEINIX_DIR=/absolute/path/to/caffeinix

make -C "$CAFFEINIX_DIR" -j"$(nproc)"
make -C "$CAFFEINIX_DIR" check-uapi
make -C "$CAFFEINIX_DIR" check-opensbi
```

This produces `output/kernel`. The kernel build needs neither musl nor
BusyBox. The optional `make build` target uses `bear` to generate a
compilation database; normal builds do not require it.

Free-page poisoning is disabled by default so boot does not write every RAM
page. Enable it when diagnosing use-after-free bugs:

```bash
make -C "$CAFFEINIX_DIR" clean
make -C "$CAFFEINIX_DIR" -j"$(nproc)" PAGE_POISONING=1
```

## Build a musl sysroot

Download and unpack upstream musl 1.2.6 anywhere outside the kernel tree,
then choose independent build and install directories:

```bash
export MUSL_SOURCE_DIR=/absolute/path/to/musl-1.2.6
export MUSL_BUILD_DIR=/absolute/path/to/musl-build
export MUSL_SYSROOT=/absolute/path/to/riscv64-linux-musl

mkdir -p "$MUSL_BUILD_DIR" "$MUSL_SYSROOT"
cd "$MUSL_BUILD_DIR"

"$MUSL_SOURCE_DIR/configure" \
  --target=riscv64-linux-musl \
  --prefix="$MUSL_SYSROOT" \
  --syslibdir="$MUSL_SYSROOT/lib" \
  --enable-wrapper=gcc \
  'CFLAGS=-march=rv64gc -mabi=lp64d' \
  CROSS_COMPILE=riscv64-linux-gnu-

make -j"$(nproc)"
make install

sed -i \
  "s|$MUSL_SYSROOT/lib/ld-musl-riscv64.so.1|/lib/ld-musl-riscv64.so.1|" \
  "$MUSL_SYSROOT/lib/musl-gcc.specs"
```

The resulting `$MUSL_SYSROOT/bin/musl-gcc` wrapper selects musl headers,
startup objects, and shared or static libraries. `--syslibdir` keeps the
installation out of the host's `/lib`; the `sed` command gives generated
programs the runtime-linker path used inside Caffeinix. This sysroot is a
userspace build input; do not pass it to the kernel Makefile.

## Build BusyBox

Download and unpack upstream BusyBox 1.38.0 anywhere. The repository ships a
tested Kconfig fragment at `configs/busybox.config`. Apply that fragment to a
fresh all-disabled configuration, then build with the external musl wrapper:

```bash
export BUSYBOX_DIR=/absolute/path/to/busybox-1.38.0

make -C "$BUSYBOX_DIR" distclean
make -C "$BUSYBOX_DIR" allnoconfig >/dev/null

while IFS= read -r setting; do
  symbol=${setting%%=*}
  sed -i \
    -e "s|^# $symbol is not set$|$setting|" \
    -e "s|^$symbol=.*|$setting|" \
    "$BUSYBOX_DIR/.config"
done < "$CAFFEINIX_DIR/configs/busybox.config"

yes '' | make -C "$BUSYBOX_DIR" oldconfig >/dev/null
make -C "$BUSYBOX_DIR" -j"$(nproc)" \
  ARCH=riscv \
  CROSS_COMPILE=riscv64-linux-gnu- \
  CC="$MUSL_SYSROOT/bin/musl-gcc"
```

The output must be an RV64 dynamically linked executable using musl:

```bash
file "$BUSYBOX_DIR/busybox"
riscv64-linux-gnu-readelf -l "$BUSYBOX_DIR/busybox"
riscv64-linux-gnu-readelf -d "$BUSYBOX_DIR/busybox"
```

`readelf` must show `/lib/ld-musl-riscv64.so.1` as the interpreter and
`libc.so` as a `DT_NEEDED` dependency.

## Create the ext4 root filesystem

Choose an empty staging directory and an output image path. BusyBox installs
its binary and applet symlinks into the staging tree. Copy the external musl
runtime into `/lib`; no Caffeinix-specific userspace source is needed.

```bash
export ROOTFS_STAGING=/absolute/path/to/rootfs-staging
export FS_IMG=/absolute/path/to/caffeinix.ext4

mkdir -p "$ROOTFS_STAGING"
make -C "$BUSYBOX_DIR" \
  ARCH=riscv \
  CROSS_COMPILE=riscv64-linux-gnu- \
  CC="$MUSL_SYSROOT/bin/musl-gcc" \
  CONFIG_PREFIX="$ROOTFS_STAGING" \
  install

install -d \
  "$ROOTFS_STAGING/dev" \
  "$ROOTFS_STAGING/lib" \
  "$ROOTFS_STAGING/tmp" \
  "$ROOTFS_STAGING/mnt/fat" \
  "$ROOTFS_STAGING/proc" \
  "$ROOTFS_STAGING/sys"

install -m 755 \
  "$MUSL_SYSROOT/lib/libc.so" \
  "$ROOTFS_STAGING/lib/libc.so"
ln -s libc.so "$ROOTFS_STAGING/lib/ld-musl-riscv64.so.1"

truncate -s 64M "$FS_IMG"
EXT4_FEATURES=none,has_journal,extent,filetype,dir_index
EXT4_FEATURES="$EXT4_FEATURES,ext_attr,sparse_super,large_file"
mke2fs -q -F \
  -t ext4 \
  -b 1024 \
  -I 256 \
  -L caffeinix \
  -O "$EXT4_FEATURES" \
  -E root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0 \
  -d "$ROOTFS_STAGING" \
  "$FS_IMG"

e2fsck -fn "$FS_IMG"
```

The explicit feature list avoids silently inheriting ext4 features from the
host's `/etc/mke2fs.conf`. Caffeinix currently requires 1 KiB filesystem
blocks; images made with the usual 4 KiB default are not supported yet.

The ext4 image contains ordinary empty `/dev` and `/tmp` directories. The
kernel covers them with devfs and tmpfs at boot, so creating host-side device
nodes is unnecessary.

To include the filesystem regression program in a newly created image, build
it with the same musl wrapper before running `mke2fs`:

```bash
"$MUSL_SYSROOT/bin/musl-gcc" \
  -static -march=rv64gc -mabi=lp64d \
  -O2 -Wall -Wextra -Werror \
  "$CAFFEINIX_DIR/tests/fs_runtime.c" \
  -o "$ROOTFS_STAGING/bin/fs-runtime"
```

Run `/bin/fs-runtime` after boot. It checks ext4, devfs, and tmpfs, and also
checks FAT semantics when `FAT_IMG` is present.

The same test can build its own external userspace and run non-interactively
under QEMU:

```bash
make -C "$CAFFEINIX_DIR/tests" qemu
```

This is the entry point used by continuous integration. See
[`tests/README.md`](tests/README.md) for its dependencies and coverage.

## Platform and serial drivers

The QEMU UART is discovered from the boot Device Tree and bound through the
platform bus. The NS16550 hardware driver feeds reusable UART and TTY cores;
devfs creates `/dev/ttyS0` from the live character-device registry.
`/dev/console` forwards to the serial console selected by
`/chosen/stdout-path`.

See [`Documentation/driver-model.md`](Documentation/driver-model.md) for the
layer ownership rules and the Device Tree needed to add another serial port.
The shared VirtIO, block, network-device, and protocol-stack boundaries are
described in the
[`network architecture`](Documentation/networking/architecture.md).
The SMP scheduling and virtual-runtime rules are described in
[`Documentation/scheduler.md`](Documentation/scheduler.md).

## Run

```bash
make -C "$CAFFEINIX_DIR" qemu FS_IMG="$FS_IMG"
```

QEMU's bundled OpenSBI starts Caffeinix in supervisor mode with eight harts and
256 MiB of RAM by default. Select other values with `CPUS` and `MEMORY`; the
kernel discovers both from the Device Tree and starts secondaries through SBI
HSM:

```bash
make -C "$CAFFEINIX_DIR" qemu \
  CPUS=4 \
  MEMORY=192M \
  FS_IMG="$FS_IMG"
```

OpenSBI remains external to the kernel build. To test a specific release,
build it in a separate directory and pass its dynamic firmware image:

```bash
export OPENSBI_DIR=/absolute/path/to/opensbi

make -C "$OPENSBI_DIR" -j"$(nproc)" \
  CROSS_COMPILE=riscv64-linux-gnu- \
  PLATFORM=generic

make -C "$CAFFEINIX_DIR" qemu \
  SBI_FIRMWARE="$OPENSBI_DIR/build/platform/generic/firmware/fw_dynamic.bin" \
  FS_IMG="$FS_IMG"
```

The kernel Makefile never downloads or builds OpenSBI. See
[`Documentation/opensbi.md`](Documentation/opensbi.md) for the boot register,
memory, SBI extension, and multi-hart contracts.

## Network device

QEMU attaches a VirtIO network device backed by user networking by default.
It needs no host privilege and supplies IPv4 configuration through DHCP:

```bash
make -C "$CAFFEINIX_DIR" qemu \
  FS_IMG="$FS_IMG"
```

Set `NET_BACKEND` to an empty value to boot without a network device:

```bash
make -C "$CAFFEINIX_DIR" qemu \
  FS_IMG="$FS_IMG" \
  NET_BACKEND=
```

Set it to another QEMU `-netdev` backend value to select a different host
backend.

Use `NET_OPTIONS` for backend-specific options such as port forwarding.
`NET_BUS` defaults to `virtio-mmio-bus.2`, after the root and optional FAT
disks, and `NET_MAC` defaults to `52:54:00:12:34:56`. Both are configurable:

```bash
make -C "$CAFFEINIX_DIR" qemu \
  FS_IMG="$FS_IMG" \
  NET_OPTIONS='hostfwd=tcp:127.0.0.1:18080-:18080' \
  NET_BUS=virtio-mmio-bus.2 \
  NET_MAC=52:54:00:12:34:56
```

Networking initializes asynchronously and never blocks the local shell when
the NIC, link, or DHCP server is absent. The initial stack supports Ethernet,
ARP, IPv4, ICMP, raw ICMP, DHCP, DNS, UDP, and TCP. musl programs use Linux
RISC-V IPv4 socket calls through ordinary VFS descriptors. See the
[`lwIP port`](Documentation/networking/lwip.md),
[`VirtIO contracts`](Documentation/networking/virtio.md), and
[`socket UAPI`](Documentation/networking/socket-uapi.md) documents.

At the shell prompt, try:

```sh
mkdir /demo
echo hello > /demo/message
ln /demo/message /demo/hard-link
ln -s message /demo/symbolic-link
cat /demo/symbolic-link
echo temporary > /tmp/volatile
ls -l /dev /tmp /demo
sync
```

Press `Ctrl-a`, then `x`, to leave QEMU. Replace `qemu` with `qemu-gdb` to
stop at reset and expose the QEMU GDB stub.

## Attach an optional FAT disk

Create either a FAT16 image:

```bash
export FAT_IMG=/absolute/path/to/data-fat16.img
truncate -s 32M "$FAT_IMG"
mkfs.fat -F 16 -n CAFFEINIX "$FAT_IMG"
```

or a FAT32 image:

```bash
export FAT_IMG=/absolute/path/to/data-fat32.img
truncate -s 128M "$FAT_IMG"
mkfs.fat -F 32 -n CAFFEINIX "$FAT_IMG"
```

Pass it as QEMU's second virtio block device:

```bash
make -C "$CAFFEINIX_DIR" qemu \
  FS_IMG="$FS_IMG" \
  FAT_IMG="$FAT_IMG"
```

The kernel mounts it at `/mnt/fat`. After a test, validate it on the host:

```bash
fsck.fat -n "$FAT_IMG"
mdir -i "$FAT_IMG" ::
```

FAT is usable as a mounted filesystem and could technically contain an init
binary. It is not the default root because FAT does not store Unix ownership,
permission bits, hard links, symbolic links, special files, or fully Unix-like
rename and unlink semantics. Devfs solves the device-node part, but it does not
restore the other metadata and namespace behavior expected by Linux software.
Ext4 is therefore the compatibility-oriented default; FAT remains useful for
removable and cross-platform data.

## Filesystem support

- ext4: default read-write root with journaling and recovery
- devfs: `/dev/console`, `/dev/ttyS0`, `/dev/tty`, `/dev/null`, and
  `/dev/zero`
- tmpfs: volatile read-write `/tmp` with files, directories, links, sparse
  pages, rename, truncate, and unlink-open-file behavior
- FAT16/32: optional persistent data filesystem with long UTF-8 names;
  Unix links and metadata are intentionally unsupported

The old xv6-derived CaffeFS format and its image builder are no longer part of
the kernel. No separate rootfs repository or private compiler is required.

## Current limitations

- Only a Linux RISC-V UAPI subset is implemented.
- Pipelines, job control, and real signal delivery are not ready.
- Dynamic musl executables and shared libraries use eager private mappings;
  demand paging, shared clean pages, copy-on-write, and ASLR are not ready.
- Userspace threads are not ready.
- Networking is IPv4-only and omits interface configuration, AF_UNIX,
  netlink, namespaces, firewalling, and the wider Linux socket option set.
- VirtIO currently uses modern MMIO split rings without packed rings,
  multiqueue, offloads, hot unplug, PCI, or a physical Ethernet MAC.
- The VFS has fixed-size object tables and no unmount syscall yet.
- FAT support excludes exFAT and returns `EBUSY` when unlinking an open file.
- Docker is not supported; it also needs namespaces, cgroups, mounts,
  networking, `/proc`, and a much wider syscall surface.
- Real boards still need a loader for the kernel and DTB plus compatible
  interrupt-controller, console, and storage drivers.

## Contributing

Bug reports and focused changes are welcome through
[GitHub issues](https://github.com/TroyMitchell911/Caffeinix/issues).

## License

Caffeinix is distributed under the GNU GPL version 3. See [LICENSE](LICENSE).
Vendored files retain their upstream licenses and provenance records.
