#!/usr/bin/env bash

set -euo pipefail

if [ -n "${QEMU_ARG_LOG:-}" ]; then
	printf '%s\n' "$@" > "$QEMU_ARG_LOG"
	exit 0
fi

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
topdir=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/caffeinix qemu.XXXXXX")
fake_qemu="$test_dir/fake qemu"
arg_log="$test_dir/arguments"
root_image="$test_dir/root image.ext4"
fat_image="$test_dir/fat image.img"
firmware="$test_dir/fw dynamic.bin"

cleanup()
{
	rm -rf -- "$test_dir"
}
trap cleanup EXIT

ln -s "$script_dir/check-qemu-args.sh" "$fake_qemu"
touch "$root_image" "$fat_image" "$firmware"

QEMU_ARG_LOG="$arg_log" make --silent --no-print-directory -C "$topdir" \
	QEMU="$fake_qemu" \
	SBI_FIRMWARE="$firmware" \
	FS_IMG="$root_image" \
	FAT_IMG="$fat_image" \
	CPUS=2 \
	MEMORY=128M \
	NET_BACKEND=user \
	NET_OPTIONS=hostfwd=tcp:127.0.0.1:18082-:18082 \
	QEMU_PREBUILT=1 \
	qemu

mapfile -t actual < "$arg_log"
expected=(
	-machine virt
	-bios "$firmware"
	-kernel output/kernel
	-m 128M
	-smp 2
	-nographic
	-global virtio-mmio.force-legacy=false
	-drive "file=$root_image,if=none,format=raw,id=x0"
	-device "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0"
	-drive "file=$fat_image,if=none,format=raw,id=x1"
	-device "virtio-blk-device,drive=x1,bus=virtio-mmio-bus.1"
	-netdev "user,id=n0,hostfwd=tcp:127.0.0.1:18082-:18082"
	-device \
		"virtio-net-device,netdev=n0,bus=virtio-mmio-bus.2,mac=52:54:00:12:34:56"
)

if [ "${#actual[@]}" -ne "${#expected[@]}" ]; then
	echo "unexpected QEMU argument count: ${#actual[@]}" >&2
	exit 1
fi

for index in "${!expected[@]}"; do
	if [ "${actual[$index]}" != "${expected[$index]}" ]; then
		echo "QEMU argument $index was split or changed" >&2
		exit 1
	fi
done

echo QEMU_ARGS_OK
