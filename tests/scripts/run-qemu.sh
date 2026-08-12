#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tests_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
topdir=$(CDPATH='' cd -- "$tests_dir/.." && pwd)

test_output=${TEST_OUTPUT:-$topdir/output/tests}
jobs=${JOBS:-$(nproc)}
qemu=${QEMU:-qemu-system-riscv64}

export LC_ALL=C.UTF-8

require_command()
{
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing test dependency: $1" >&2
		exit 1
	fi
}

for command in "$qemu" expect e2fsck debugfs fsck.fat mtype; do
	require_command "$command"
done

make -C "$topdir" -j"$jobs"
TEST_OUTPUT="$test_output" JOBS="$jobs" \
	"$script_dir/build-rootfs.sh"

test_output=$(CDPATH='' cd -- "$test_output" && pwd)
root_image=$test_output/root.ext4
fat_image=$test_output/data-fat32.img
qemu_log=$test_output/qemu.log
clean_log=$test_output/qemu.clean.log

export QEMU=$qemu
export KERNEL=$topdir/output/kernel
export ROOT_IMAGE=$root_image
export FAT_IMAGE=$fat_image
export QEMU_LOG=$qemu_log
export QEMU_TIMEOUT=${QEMU_TIMEOUT:-60}

"$script_dir/run-qemu.exp"
tr -d '\r' < "$qemu_log" > "$clean_log"

for marker in \
	BUSYBOX_SHELL_OK \
	TTY_METADATA_OK \
	TTY_CANONICAL_OK \
	TTY_RAW_OK \
	TTY_LONG_BEGIN \
	TTY_LONG_END \
	DEVFS_OK \
	EXT4_OK \
	TMPFS_OK \
	FAT_OK \
	FS_RUNTIME_OK; do
	marker_count=$(awk -v marker="$marker" \
		'$0 == marker { count++ } END { print count + 0 }' \
		"$clean_log")
	if [ "$marker_count" -ne 1 ]; then
		echo "unexpected QEMU marker count: $marker=$marker_count" >&2
		exit 1
	fi
done

if ! awk '
	/^U+$/ {
		lines++
		if (length($0) != 1024)
			bad = 1
	}
	END { exit lines != 1 || bad }
' "$clean_log"; then
	echo "long UART output was not one exact 1024-byte line" >&2
	exit 1
fi

if ! grep -Fq $'abc\b \bd' "$qemu_log"; then
	echo "canonical erase echo sequence is missing" >&2
	exit 1
fi

if grep -Eq \
	'\[PANIC\]|Unhandled interrupt|FS_RUNTIME_FAIL=|TTY_RUNTIME_FAIL=' \
	"$clean_log"; then
	echo "QEMU log contains a guest failure" >&2
	exit 1
fi

set +e
e2fsck -fy "$root_image"
e2fsck_status=$?
set -e
if [ "$e2fsck_status" -gt 1 ]; then
	echo "ext4 recovery failed with status $e2fsck_status" >&2
	exit "$e2fsck_status"
fi
e2fsck -fn "$root_image"
fsck.fat -n "$fat_image"

ext_value=$(debugfs -R 'cat /ext-runtime/target' \
	"$root_image" 2>/dev/null)
if [ "$ext_value" != new ]; then
	echo "ext4 runtime value was not persisted" >&2
	exit 1
fi

if debugfs -R 'stat /tmp/tmp-runtime' "$root_image" 2>/dev/null |
	grep -q '^Inode:'; then
	echo "tmpfs runtime data leaked into ext4" >&2
	exit 1
fi

fat_value=$(mtype -i "$fat_image" ::/runtime/target)
if [ "$fat_value" != new ]; then
	echo "FAT runtime value was not persisted" >&2
	exit 1
fi

fat_utf8=$(mtype -i "$fat_image" '::/runtime/long-咖啡-file-name.txt')
if [ "$fat_utf8" != utf8 ]; then
	echo "FAT UTF-8 long-name value was not persisted" >&2
	exit 1
fi

echo QEMU_RUNTIME_OK
