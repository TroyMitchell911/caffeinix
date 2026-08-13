#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tests_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
topdir=$(CDPATH='' cd -- "$tests_dir/.." && pwd)

test_output=${TEST_OUTPUT:-$topdir/output/tests}
download_dir=${DOWNLOAD_DIR:-$test_output/downloads}
jobs=${JOBS:-$(nproc)}
cross_compile=${CROSS_COMPILE:-riscv64-linux-gnu-}

musl_version=1.2.6
musl_sha256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
musl_origin=https://musl.libc.org/releases
musl_mirror=https://sources.buildroot.net/musl
busybox_version=1.38.0
busybox_sha256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
busybox_origin=https://busybox.net/downloads
busybox_mirror=https://sources.buildroot.net/busybox

mkdir -p "$test_output" "$download_dir"
test_output=$(CDPATH='' cd -- "$test_output" && pwd)
download_dir=$(CDPATH='' cd -- "$download_dir" && pwd)
work_dir=$(mktemp -d "$test_output/work.XXXXXX")

cleanup()
{
	rm -rf -- "$work_dir"
}
trap cleanup EXIT

require_command()
{
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing test dependency: $1" >&2
		exit 1
	fi
}

download()
{
	local destination=$1
	local checksum=$2
	local temporary
	local url

	shift 2

	if [ -f "$destination" ] &&
	   echo "$checksum  $destination" | sha256sum --check --status; then
		return
	fi

	for url in "$@"; do
		temporary=$(mktemp "$destination.tmp.XXXXXX")
		if curl --fail --location \
			--retry 5 --retry-all-errors --retry-delay 2 \
			--connect-timeout 20 --show-error --silent \
			"$url" --output "$temporary" &&
		   echo "$checksum  $temporary" |
			sha256sum --check --status; then
			mv -- "$temporary" "$destination"
			return
		fi
		rm -f -- "$temporary"
	done

	echo "failed to download $(basename -- "$destination")" >&2
	return 1
}

for command in \
	curl sha256sum tar make sed install truncate \
	mke2fs e2fsck mkfs.fat fsck.fat; do
	require_command "$command"
done
require_command "${cross_compile}gcc"
require_command "${cross_compile}readelf"

musl_archive=$download_dir/musl-$musl_version.tar.gz
busybox_archive=$download_dir/busybox-$busybox_version.tar.bz2

download \
	"$musl_archive" "$musl_sha256" \
	"$musl_origin/musl-$musl_version.tar.gz" \
	"$musl_mirror/musl-$musl_version.tar.gz"
download \
	"$busybox_archive" "$busybox_sha256" \
	"$busybox_origin/busybox-$busybox_version.tar.bz2" \
	"$busybox_mirror/busybox-$busybox_version.tar.bz2"

tar -xzf "$musl_archive" -C "$work_dir"
tar -xjf "$busybox_archive" -C "$work_dir"

musl_source=$work_dir/musl-$musl_version
musl_build=$work_dir/musl-build
musl_sysroot=$work_dir/musl-sysroot
mkdir -p "$musl_build" "$musl_sysroot"

musl_log=$test_output/musl-build.log
if ! (
	cd "$musl_build"
	"$musl_source/configure" \
		--target=riscv64-linux-musl \
		--prefix="$musl_sysroot" \
		--disable-shared \
		--enable-wrapper=gcc \
		'CFLAGS=-march=rv64gc -mabi=lp64d' \
		CROSS_COMPILE="$cross_compile"
	make -j"$jobs"
	make install
) >"$musl_log" 2>&1; then
	cat "$musl_log" >&2
	exit 1
fi

busybox_source=$work_dir/busybox-$busybox_version
busybox_log=$test_output/busybox-build.log
make -C "$busybox_source" allnoconfig >"$busybox_log" 2>&1

while IFS= read -r setting; do
	case "$setting" in
		''|'#'*) continue ;;
	esac
	symbol=${setting%%=*}
	sed -i \
		-e "s|^# $symbol is not set$|$setting|" \
		-e "s|^$symbol=.*|$setting|" \
		"$busybox_source/.config"
done < "$topdir/configs/busybox.config"

set +o pipefail
yes '' | make -C "$busybox_source" oldconfig >>"$busybox_log" 2>&1
set -o pipefail

musl_cc=$musl_sysroot/bin/musl-gcc
if ! make -C "$busybox_source" -j"$jobs" \
		ARCH=riscv \
		CROSS_COMPILE="$cross_compile" \
		CC="$musl_cc" >>"$busybox_log" 2>&1; then
	cat "$busybox_log" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$busybox_source/busybox" |
	grep -q INTERP; then
	echo "BusyBox must be statically linked" >&2
	exit 1
fi

staging=$work_dir/rootfs
if ! make -C "$busybox_source" \
		ARCH=riscv \
		CROSS_COMPILE="$cross_compile" \
		CC="$musl_cc" \
		CONFIG_PREFIX="$staging" \
		install >>"$busybox_log" 2>&1; then
	cat "$busybox_log" >&2
	exit 1
fi

install -d \
	"$staging/dev" \
	"$staging/tmp" \
	"$staging/mnt/fat" \
	"$staging/proc" \
	"$staging/sys"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/fs_runtime.c" \
	-o "$staging/bin/fs-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/tty_runtime.c" \
	-o "$staging/bin/tty-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/scheduler_runtime.c" \
	-o "$staging/bin/scheduler-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/network_runtime.c" \
	-o "$staging/bin/network-runtime"

if "${cross_compile}readelf" -l "$staging/bin/fs-runtime" |
	grep -q INTERP; then
	echo "guest selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/tty-runtime" |
	grep -q INTERP; then
	echo "TTY selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/scheduler-runtime" |
	grep -q INTERP; then
	echo "scheduler selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/network-runtime" |
	grep -q INTERP; then
	echo "network selftest must be statically linked" >&2
	exit 1
fi

root_image=$test_output/root.ext4
fat_image=$test_output/data-fat32.img
ext4_features=none,has_journal,extent,filetype,dir_index
ext4_features=$ext4_features,ext_attr,sparse_super,large_file

truncate -s 64M "$root_image"
mke2fs -q -F \
	-t ext4 \
	-b 1024 \
	-I 256 \
	-L caffeinix \
	-O "$ext4_features" \
	-E root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0 \
	-d "$staging" \
	"$root_image"

truncate -s 128M "$fat_image"
mkfs.fat -F 32 -n CAFFEINIX "$fat_image" >/dev/null

e2fsck -fn "$root_image"
fsck.fat -n "$fat_image"

echo "Created $root_image"
echo "Created $fat_image"
