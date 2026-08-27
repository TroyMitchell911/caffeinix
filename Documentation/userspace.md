# BusyBox userspace

Caffeinix uses unmodified BusyBox 1.38.0 and musl 1.2.6.  The normal
kernel build does not fetch or build either project.  The checked
`configs/busybox.config` fragment selects the broad userspace supported by
the current kernel, and `configs/busybox.applets` records its 207 applet
names.

The test image contains two builds of the same configuration:

- `/bin/busybox` is dynamically linked against the upstream musl runtime;
- `/bin/busybox-static` is a statically linked recovery binary.

The image builder compares installed applet links with the checked manifest.
The guest test also compares both binaries' `--list` output with that
manifest.  This catches accidental Kconfig drift as well as incomplete
installation.

## Supported families

The QEMU suite checks observable behavior from each enabled family:

- ash language features, scripts, pipelines, substitution, foreground and
  background jobs, terminal signals, history, completion, cursor editing,
  long and UTF-8 input, and `vi` persistence;
- core pathname, file, checksum, encoding, text-search, stream, and archive
  tools, including gzip, bzip2, xz, cpio, and tar round trips;
- file metadata, ownership, permissions, sparse allocation, ext4, tmpfs,
  FAT, devfs, mount, and unmount consumers;
- account lookup and credential changes through `id`, `groups`, `whoami`,
  `su`, and password hashing;
- clocks, sleeps, timeouts, process priority, process lookup, procfs-backed
  observation, and system identity;
- IPv4 status, route reporting, ICMP echo, DNS lookup, TCP and UDP sockets,
  `nc`, `wget`, and a loopback BusyBox HTTP server.

Some enabled programs are inherently interactive or require an external
service configuration.  `less`, `more`, `ed`, `hexedit`, `man`, interactive
`bc`, `watch`, and interactive `top` are covered by their shared TTY,
process, file, signal, and procfs primitives rather than scripted keystrokes.
The FTP, TFTP, telnet, inetd, cron, DNS-daemon, and superserver applets are
covered by representative socket, HTTP-server, timer, process, and file
tests; protocol interoperability with every external peer is not claimed.

## Deliberately excluded families

The profile leaves these applets or optional features disabled until their
kernel subsystem exists:

- module tools: loadable modules and Linux module metadata;
- MTD, NAND, UBI, and flash tools: raw flash and MTD UAPI;
- I2C, framebuffer, console, input, PCI, SCSI, and USB tools: the
  corresponding device classes and UAPI;
- swap, loop, RAID, and NBD tools: their backing block and memory-management
  facilities;
- namespace and container tools: namespaces, cgroups, capabilities, and
  SELinux;
- IPv6, bridge, VLAN, firewall, and traffic-control tools: IPv6 and mutable
  rtnetlink and netfilter control planes;
- syslog and kernel-log tools: a stable userspace printk ring interface;
- thread display in process tools: `/proc/<pid>/task/<tid>` snapshots;
- `mesg` and multi-user login policy: mutable per-TTY ownership and mode;
- `seedrng`: Linux random-device administration ioctls.

This policy intentionally measures compatibility by working behavior rather
than by approaching BusyBox `defconfig`'s applet count.  See the main README
for the external musl, BusyBox, and ext4 image workflow, and `tests/README.md`
for automated coverage.
