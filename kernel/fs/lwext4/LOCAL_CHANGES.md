# Caffeinix changes to lwext4

The VFS adapter and build compatibility files are kept outside the upstream
`src` and `include` directories. The following small upstream-source changes
are required:

- `ext4_raw_inode_fill_by_number()` keeps an unlinked but open inode usable
  after its pathname disappears.
- `ext4_fseek()` accepts a write position beyond EOF, and the write path
  allocates the requested logical extent so seek-created sparse files retain
  holes.
- Extent truncation handles files whose first allocated extent follows a
  leading hole, which is required for `O_TRUNC` and unlink after sparse writes.
- VFS sync writes lwext4's in-memory superblock counters before flushing the
  block device, so a running filesystem does not require unmount to persist
  allocation statistics.
