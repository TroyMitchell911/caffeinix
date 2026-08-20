#!/usr/bin/env bash

set -euo pipefail

umask 0022

export LC_ALL=C.UTF-8

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
	curl sha256sum tar make sed install ln truncate \
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
		--syslibdir="$musl_sysroot/lib" \
		--enable-wrapper=gcc \
		'CFLAGS=-march=rv64gc -mabi=lp64d' \
		CROSS_COMPILE="$cross_compile"
	make -j"$jobs"
	make install
) >"$musl_log" 2>&1; then
	cat "$musl_log" >&2
	exit 1
fi

musl_build_interp=$musl_sysroot/lib/ld-musl-riscv64.so.1
musl_guest_interp=/lib/ld-musl-riscv64.so.1
musl_linker_rule="s|-dynamic-linker $musl_build_interp|"
musl_linker_rule+="-dynamic-linker $musl_guest_interp|"
sed -i "$musl_linker_rule" "$musl_sysroot/lib/musl-gcc.specs"

musl_cc=$musl_sysroot/bin/musl-gcc
busybox_source=$work_dir/busybox-$busybox_version
busybox_log=$test_output/busybox-build.log
make -C "$busybox_source" \
	ARCH=riscv \
	CROSS_COMPILE="$cross_compile" \
	CC="$musl_cc" \
	allnoconfig >"$busybox_log" 2>&1

apply_busybox_fragment()
{
	local setting symbol

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
}

busybox_fragment_matches()
{
	local setting

	while IFS= read -r setting; do
		case "$setting" in
			''|'#'*) continue ;;
		esac
		grep -Fqx "$setting" "$busybox_source/.config" || return 1
	done < "$topdir/configs/busybox.config"
}

for pass in 1 2 3 4; do
	apply_busybox_fragment
	set +o pipefail
	yes '' | make -C "$busybox_source" \
		ARCH=riscv \
		CROSS_COMPILE="$cross_compile" \
		CC="$musl_cc" \
		oldconfig >>"$busybox_log" 2>&1
	set -o pipefail
	if busybox_fragment_matches; then
		break
	fi
done
if ! busybox_fragment_matches; then
	echo "BusyBox rejected requested settings:" >&2
	while IFS= read -r setting; do
		case "$setting" in
			''|'#'*) continue ;;
		esac
		grep -Fqx "$setting" "$busybox_source/.config" ||
			echo "  $setting" >&2
	done < "$topdir/configs/busybox.config"
	exit 1
fi

if ! make -C "$busybox_source" -j"$jobs" \
		ARCH=riscv \
		CROSS_COMPILE="$cross_compile" \
		CC="$musl_cc" >>"$busybox_log" 2>&1; then
	cat "$busybox_log" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -l "$busybox_source/busybox" |
	grep -q '/lib/ld-musl-riscv64.so.1'; then
	echo "BusyBox must use the musl runtime linker" >&2
	exit 1
fi
if ! "${cross_compile}readelf" -d "$busybox_source/busybox" |
	grep -q 'Shared library: \[libc.so\]'; then
	echo "BusyBox must depend on shared musl libc" >&2
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

generated_applets=$test_output/busybox.applets.generated
{
	find "$staging" -type l -printf '%f\n'
	printf '%s\n' busybox
} | LC_ALL=C sort -u > "$generated_applets"
if [ ! -f "$topdir/configs/busybox.applets" ]; then
	echo "missing BusyBox applet manifest" >&2
	cat "$generated_applets" >&2
	exit 1
fi
if ! cmp -s "$topdir/configs/busybox.applets" "$generated_applets"; then
	echo "BusyBox applet manifest is stale" >&2
	diff -u "$topdir/configs/busybox.applets" "$generated_applets" >&2 ||
		true
	exit 1
fi

sed -i \
	's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' \
	"$busybox_source/.config"
set +o pipefail
yes '' | make -C "$busybox_source" \
	ARCH=riscv \
	CROSS_COMPILE="$cross_compile" \
	CC="$musl_cc" \
	oldconfig >>"$busybox_log" 2>&1
set -o pipefail
make -C "$busybox_source" clean >>"$busybox_log" 2>&1
if ! make -C "$busybox_source" -j"$jobs" \
		ARCH=riscv \
		CROSS_COMPILE="$cross_compile" \
		CC="$musl_cc" >>"$busybox_log" 2>&1; then
	cat "$busybox_log" >&2
	exit 1
fi
install -m 755 "$busybox_source/busybox" \
	"$staging/bin/busybox-static"
if "${cross_compile}readelf" -l "$staging/bin/busybox-static" |
	grep -q INTERP; then
	echo "recovery BusyBox must be statically linked" >&2
	exit 1
fi

install -d \
	"$staging/dev" \
	"$staging/etc" \
	"$staging/lib" \
	"$staging/mnt/fat" \
	"$staging/proc" \
	"$staging/root" \
	"$staging/sys" \
	"$staging/tmp" \
	"$staging/var/log" \
	"$staging/var/run" \
	"$staging/var/spool/cron/crontabs"

printf '%s\n' \
	'root:x:0:0:root:/root:/bin/sh' \
	'nobody:x:65534:65534:nobody:/tmp:/sbin/nologin' \
	> "$staging/etc/passwd"
printf '%s\n' \
	'root:x:0:' \
	'nobody:x:65534:' \
	> "$staging/etc/group"
printf '%s\n' \
	'root:*:0:0:99999:7:::' \
	'nobody:*:0:0:99999:7:::' \
	> "$staging/etc/shadow"
chmod 600 "$staging/etc/shadow"
printf '%s\n' '/bin/sh' '/bin/ash' > "$staging/etc/shells"
printf '%s\n' '127.0.0.1 localhost' > "$staging/etc/hosts"
printf '%s\n' 'nameserver 127.0.0.1' > "$staging/etc/resolv.conf"
install -m 644 "$topdir/configs/busybox.applets" \
	"$staging/etc/busybox.applets"
printf '%s\n' \
	'proc /proc proc defaults 0 0' \
	'devfs /dev devfs defaults 0 0' \
	'tmpfs /tmp tmpfs defaults 0 0' \
	> "$staging/etc/fstab"

install -m 755 "$musl_sysroot/lib/libc.so" "$staging/lib/libc.so"
ln -s libc.so "$staging/lib/ld-musl-riscv64.so.1"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -static -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_shared_page.ld" \
	"$tests_dir/elf_shared_page.S" \
	-o "$staging/bin/elf-shared-page"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -shared -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_interp_loader.ld" \
	"$tests_dir/elf_interp_loader.S" \
	-o "$staging/lib/ld-caffeinix-test-riscv64.so.1"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -shared -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_interp_main.ld" \
	"$tests_dir/elf_interp_main.S" \
	-o "$staging/bin/elf-interp"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -shared -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=2097152 \
	-Wl,-T,"$tests_dir/elf_interp_high.ld" \
	"$tests_dir/elf_interp_main.S" \
	-o "$staging/bin/elf-high-base"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -shared -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_interp_main.ld" \
	"$tests_dir/elf_missing_interp.S" \
	-o "$staging/bin/elf-missing-interp"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -static -march=rv64gc -mabi=lp64d \
	-DELF_FIXED_INTERPRETER \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_interp_fixed_loader.ld" \
	"$tests_dir/elf_interp_loader.S" \
	-o "$staging/lib/ld-caffeinix-riscv64.so.1"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -shared -march=rv64gc -mabi=lp64d \
	-DELF_FIXED_INTERPRETER \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_interp_main.ld" \
	"$tests_dir/elf_interp_main.S" \
	-o "$staging/bin/elf-fixed-interp"

"${cross_compile}gcc" \
	-nostdlib -nostartfiles -static -march=rv64gc -mabi=lp64d \
	-Wl,--build-id=none -Wl,-z,max-page-size=4096 \
	-Wl,-T,"$tests_dir/elf_permissions.ld" \
	"$tests_dir/elf_permissions.S" \
	-o "$staging/bin/elf-permissions"

if [ "$("${cross_compile}readelf" -h "$staging/bin/elf-interp" |
	awk '$1 == "Type:" { print $2 }')" != DYN ]; then
	echo "ELF interpreter selftest must be ET_DYN" >&2
	exit 1
fi
if [ "$("${cross_compile}readelf" -h \
	"$staging/lib/ld-caffeinix-test-riscv64.so.1" |
	awk '$1 == "Type:" { print $2 }')" != DYN ]; then
	echo "ELF interpreter fixture must be ET_DYN" >&2
	exit 1
fi
if ! "${cross_compile}readelf" -l "$staging/bin/elf-interp" |
	grep -q '/lib/ld-caffeinix-test-riscv64.so.1'; then
	echo "ELF interpreter selftest has the wrong PT_INTERP" >&2
	exit 1
fi
if "${cross_compile}readelf" -l \
	"$staging/lib/ld-caffeinix-test-riscv64.so.1" | grep -q INTERP; then
	echo "ELF interpreter fixture must not contain PT_INTERP" >&2
	exit 1
fi
if [ "$("${cross_compile}readelf" -h "$staging/bin/elf-interp" |
	awk '$1 == "Number" && $2 == "of" && $3 == "program" &&
	     $4 == "headers:" { print $5 }')" != 3 ]; then
	echo "ELF interpreter selftest must contain three program headers" >&2
	exit 1
fi
if [ "$("${cross_compile}readelf" -h \
	"$staging/lib/ld-caffeinix-riscv64.so.1" |
	awk '$1 == "Type:" { print $2 }')" != EXEC ]; then
	echo "fixed ELF interpreter fixture must be ET_EXEC" >&2
	exit 1
fi
if ! "${cross_compile}readelf" -l "$staging/bin/elf-fixed-interp" |
	grep -q '/lib/ld-caffeinix-riscv64.so.1'; then
	echo "fixed ELF interpreter selftest has the wrong PT_INTERP" >&2
	exit 1
fi
if "${cross_compile}readelf" -l \
	"$staging/lib/ld-caffeinix-riscv64.so.1" | grep -q INTERP; then
	echo "fixed ELF interpreter fixture must not contain PT_INTERP" >&2
	exit 1
fi

high_load_address=$("${cross_compile}readelf" -lW \
	"$staging/bin/elf-high-base" |
	awk '$1 == "LOAD" { print $3; exit }')
high_load_alignment=$("${cross_compile}readelf" -lW \
	"$staging/bin/elf-high-base" |
	awk '$1 == "LOAD" { print $NF; exit }')
if (( high_load_address != 0x20000000 ||
      high_load_alignment != 0x200000 )); then
	echo "ELF high-base selftest has the wrong PT_LOAD layout" >&2
	exit 1
fi

mapfile -t elf_load_addresses < <(
	"${cross_compile}readelf" -lW "$staging/bin/elf-shared-page" |
		awk '$1 == "LOAD" { print $3 }'
)
if [ "${#elf_load_addresses[@]}" -ne 3 ] ||
	(( elf_load_addresses[0] / 4096 != elf_load_addresses[1] / 4096 ||
	   elf_load_addresses[0] / 4096 != elf_load_addresses[2] / 4096 )); then
	echo "ELF boundary selftest must share a PT_LOAD page" >&2
	exit 1
fi
if "${cross_compile}readelf" -l "$staging/bin/elf-shared-page" |
	grep -q INTERP; then
	echo "ELF boundary selftest must be statically linked" >&2
	exit 1
fi

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/dynamic_hello.c" \
	-o "$staging/bin/dynamic-hello"

"$musl_cc" \
	-shared -fPIC -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	-Wl,-soname,libdynamic-fixture.so \
	"$tests_dir/dynamic_fixture.c" \
	-o "$staging/lib/libdynamic-fixture.so"

"$musl_cc" \
	-shared -fPIC -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	-Wl,-soname,libdynamic-dlopen.so \
	"$tests_dir/dynamic_dlopen_fixture.c" \
	-o "$staging/lib/libdynamic-dlopen.so"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	-Wl,-z,relro,-z,now \
	"$tests_dir/dynamic_child.c" \
	-o "$staging/bin/dynamic-child"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	-Wl,-z,relro,-z,now \
	"$tests_dir/dynamic_runtime.c" \
	-L"$staging/lib" -ldynamic-fixture \
	-o "$staging/bin/dynamic-runtime"

"$musl_cc" \
	-fPIE -pie -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/aslr_runtime.c" \
	-L"$staging/lib" -ldynamic-fixture \
	-o "$staging/bin/aslr-runtime"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/memory_runtime.c" \
	-o "$staging/bin/memory-dynamic"

for program in \
	"$staging/bin/dynamic-hello" \
	"$staging/bin/dynamic-child" \
	"$staging/bin/dynamic-runtime" \
	"$staging/bin/aslr-runtime" \
	"$staging/bin/memory-dynamic"; do
	if ! "${cross_compile}readelf" -l "$program" |
		grep -q '/lib/ld-musl-riscv64.so.1'; then
		echo "dynamic fixture has the wrong PT_INTERP: $program" >&2
		exit 1
	fi
	if ! "${cross_compile}readelf" -d "$program" |
		grep -q 'Shared library: \[libc.so\]'; then
		echo "dynamic fixture does not depend on musl: $program" >&2
		exit 1
	fi
done

if [ "$("${cross_compile}readelf" -h "$staging/bin/aslr-runtime" |
	awk '$1 == "Type:" { print $2 }')" != DYN ]; then
	echo "ASLR selftest must be a PIE executable" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -d "$staging/bin/dynamic-runtime" |
	grep -q 'Shared library: \[libdynamic-fixture.so\]'; then
	echo "dynamic runtime lacks its DT_NEEDED fixture" >&2
	exit 1
fi
if ! "${cross_compile}readelf" -l "$staging/bin/dynamic-runtime" |
	grep -q TLS; then
	echo "dynamic runtime lacks a PT_TLS segment" >&2
	exit 1
fi
if ! "${cross_compile}readelf" -l "$staging/bin/dynamic-runtime" |
	grep -q GNU_RELRO; then
	echo "dynamic runtime lacks a GNU_RELRO segment" >&2
	exit 1
fi

for library in \
	"$staging/lib/libdynamic-fixture.so" \
	"$staging/lib/libdynamic-dlopen.so" \
	"$staging/lib/libc.so"; do
	if [ "$("${cross_compile}readelf" -h "$library" |
		awk '$1 == "Type:" { print $2 }')" != DYN ]; then
		echo "shared fixture must be ET_DYN: $library" >&2
		exit 1
	fi
	if "${cross_compile}readelf" -l "$library" | grep -q INTERP; then
		echo "shared fixture must not contain PT_INTERP: $library" >&2
		exit 1
	fi
done

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
	"$tests_dir/job_control_runtime.c" \
	-o "$staging/bin/job-control-runtime"

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

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/pressure_runtime.c" \
	-o "$staging/bin/pressure-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/vm_runtime.c" \
	-o "$staging/bin/vm-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/exec_runtime.c" \
	-o "$staging/bin/exec-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/io_runtime.c" \
	-o "$staging/bin/io-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/pipe_runtime.c" \
	-o "$staging/bin/pipe-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/process_group_runtime.c" \
	-o "$staging/bin/process-group-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/time_runtime.c" \
	-o "$staging/bin/time-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/system_runtime.c" \
	-o "$staging/bin/system-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/timestamp_runtime.c" \
	-o "$staging/bin/timestamp-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/procfs_runtime.c" \
	-o "$staging/bin/procfs-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/credential_runtime.c" \
	-o "$staging/bin/credential-runtime"
install -m 755 "$staging/bin/credential-runtime" \
	"$staging/bin/setid-exec-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/file_admin_runtime.c" \
	-o "$staging/bin/file-admin-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/mount_runtime.c" \
	-o "$staging/bin/mount-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/memory_runtime.c" \
	-o "$staging/bin/memory-static"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/random_runtime.c" \
	-o "$staging/bin/random-runtime"

"$musl_cc" \
	-static -march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror \
	"$tests_dir/thread_runtime.c" "$tests_dir/thread_clone.S" \
	-o "$staging/bin/thread-runtime"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/pthread_runtime.c" \
	-o "$staging/bin/pthread-runtime"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/futex_runtime.c" \
	-o "$staging/bin/futex-runtime"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/membarrier_runtime.c" \
	-o "$staging/bin/membarrier-runtime"

"$musl_cc" \
	-march=rv64gc -mabi=lp64d \
	-O2 -Wall -Wextra -Werror -pthread \
	"$tests_dir/signal_runtime.c" \
	"$tests_dir/signal_register.S" \
	-o "$staging/bin/signal-runtime"

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

if "${cross_compile}readelf" -l "$staging/bin/pressure-runtime" |
	grep -q INTERP; then
	echo "pressure selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/vm-runtime" |
	grep -q INTERP; then
	echo "VM selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/exec-runtime" |
	grep -q INTERP; then
	echo "exec selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/io-runtime" |
	grep -q INTERP; then
	echo "I/O selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/pipe-runtime" |
	grep -q INTERP; then
	echo "pipe selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/mount-runtime" |
	grep -q INTERP; then
	echo "mount selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/memory-static" |
	grep -q INTERP; then
	echo "static memory benchmark must not contain PT_INTERP" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/random-runtime" |
	grep -q INTERP; then
	echo "random selftest must be statically linked" >&2
	exit 1
fi

if "${cross_compile}readelf" -l "$staging/bin/thread-runtime" |
	grep -q INTERP; then
	echo "thread selftest must be statically linked" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -l "$staging/bin/pthread-runtime" |
	grep -q '/lib/ld-musl-riscv64.so.1'; then
	echo "pthread selftest must use the musl runtime linker" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -l "$staging/bin/futex-runtime" |
	grep -q '/lib/ld-musl-riscv64.so.1'; then
	echo "futex selftest must use the musl runtime linker" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -l "$staging/bin/membarrier-runtime" |
	grep -q '/lib/ld-musl-riscv64.so.1'; then
	echo "membarrier selftest must use the musl runtime linker" >&2
	exit 1
fi

if ! "${cross_compile}readelf" -l "$staging/bin/signal-runtime" |
	grep -q '/lib/ld-musl-riscv64.so.1'; then
	echo "signal selftest must use the musl runtime linker" >&2
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
