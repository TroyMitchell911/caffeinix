# Filesystems and VFS

## Scope

The Caffeinix VFS presents one process-visible pathname namespace over
filesystem implementations, pseudo filesystems, character devices, block
devices, pipes, and sockets.  It owns path resolution, mount attachment,
descriptor tables, and the common permission checks.  A filesystem owns its
private inode and on-media state and supplies the operation vectors declared
in `kernel/include/vfs.h`.

The VFS uses its own `VFS_ERR_*` domain internally.  `kernel/sysfile.c` is the
boundary that converts those results to Linux RISC-V UAPI errno values.  New
filesystem code must not return Linux errno directly.

## Objects and lifetime

`vfs_super_block` represents one mounted filesystem.  A filesystem creates
one through `vfs_super_alloc()` and stores its private state in `private`.
The VFS drops it only after the mount and every inode reference are gone.

`vfs_inode` is the VFS description of an object.  Its `private` member is
filesystem-owned.  `vfs_inode_get()` and `vfs_inode_put()` control the VFS
reference count; the superblock `put_inode` method releases the private
object on the final put.

`vfs_dentry` connects an inode to a parent and component name.  A `vfs_path`
holds one mount and dentry reference.  An open `vfs_file` owns such a path,
so an unlinked object can remain usable until its final file reference is
released.  Filesystem implementations must not free private inode state just
because its directory entry was removed.

`vfs_get_root()` fills a kernel `vfs_path` output with owned mount and dentry
references. It does not take a pathname; release its result with
`vfs_path_put()` when the caller no longer needs it.

The old fixed `file_t` table is a compatibility facility for early kernel
users.  New kernel code should prefer `vfs_file` references or process file
descriptors.

## Path walking and mounts

Path walking begins at the process current directory for relative names and
at the root mount for absolute names.  It resolves `.` and `..`, follows
mounts, and follows at most the configured number of symbolic links.  Creation
operations resolve the parent separately and pass a validated leaf name to
the filesystem operation.

The namespace lock serializes mount attachment, detachment, dentry-cache
invalidation, and path transitions across mountpoints.  A filesystem must use
its own lock for metadata and on-media transactions; it must not assume that
the namespace lock protects its private fields.  Mount and unmount may sleep,
and unmount rejects a busy mount rather than invalidating open references.

`vfs_mount_root()` installs the first root.  `vfs_mount()` attaches a further
filesystem to a directory.  `vfs_mount_path()` resolves a block-device source
name before following the same mount path. Filesystem names and source/target
pathnames are kernel-resident strings; syscall wrappers copy user strings
before entering VFS. The caller supplies only
filesystem-private mount data that remains valid for the mount operation; a
filesystem that needs persistent data must copy it.

## I/O, offsets, and access

Read and write operation vectors receive explicit user-address indicators.
Backends must safely copy user buffers and may return a successful short
count.  Positioned operations never change the shared open-file position.
Current-position reads take `position_lock`. Writes to regular files take the
superblock write lock, not `position_lock`; they do not provide common
read/write offset serialization. In particular, `vfs_file_write_current()`
accepts one byte buffer and a byte count, not an iovec array. Callers needing
serialization against other offset mutations must arrange it explicitly.
Callers must preserve a nonzero partial result when a later copy, device, or
signal error occurs; Linux syscall wrappers rely on that rule.

VFS tracks read, write, and executable mappings to enforce access conflicts.
It also centralizes metadata and atime policy before asking a backend to
update persistent state.  Backends report their natural errors with
`VFS_ERR_*`; VFS does not reinterpret a backend-specific protocol error.

## Filesystem implementations

- **lwext4** is used through `kernel/fs/lwext4/caffeinix.c`.  That adapter
  serializes the imported library, translates paths, and maintains open inode
  state and the adapter orphan directory.  Imported lwext4 sources are kept
  untouched.
- **FatFs** is used through `kernel/fs/fatfs/caffeinix.c` and `diskio.c`.
  The configured library is non-reentrant, so the adapter lock encloses every
  FatFs operation.  It supports the one configured 512-byte-sector volume;
  POSIX links and symlinks are not provided by FAT.
- **tmpfs** stores entries and sparsely allocated pages in kernel memory.  A
  per-superblock lock protects all tmpfs private objects.  It has no backing
  store and therefore loses content at unmount or reboot.
- **devfs** exposes registered character and block devices.  It does not own
  drivers; a driver must unregister its nodes before releasing device storage.
- **procfs** produces read-only snapshots of selected process, memory, mount,
  scheduler, and network state.  It is diagnostic text, not a stable complete
  Linux procfs implementation.

Pipes use the VFS file interface but are not mounted filesystems.  Each pipe
owns one page-sized ring buffer, endpoint counters, and reader/writer wait
queues.  Its lock protects all three and waiters must recheck the condition
after wakeup.

## Errors and limitations

The VFS integrates the kernel page cache for regular-file reads, writes,
writeback, truncation, executable mappings, and mount teardown.  It does
not provide journaling of its own, mount namespaces, file leases, Linux
extended attributes, or a complete Linux procfs surface.  Crash consistency
is consequently defined by the backing filesystem and device driver.  Callers
must tolerate
`VFS_ERR_NOTSUPP` for optional operations and must not infer a full Linux
feature from a successful mount.

## Reproducible checks

Build the kernel and run the filesystem-facing guest tests as described in
the top-level README.  The test build creates the required guest rootfs and
its musl/BusyBox inputs; it does not require a separately built rootfs.

```bash
make -C /path/to/caffeinix -j"$(nproc)"
make -C /path/to/caffeinix/tests qemu
```

The QEMU suite exercises ext4 root mounting, tmpfs and FAT mounts, devfs,
procfs, pathname and metadata operations, pipes, vectored I/O, `sendfile`,
and BusyBox filesystem applets.  Focused host-side tests are available through
`make -C tests`.
