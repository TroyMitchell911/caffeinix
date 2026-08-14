#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tests_dir=$(CDPATH='' cd -- "$script_dir/.." && pwd)
topdir=$(CDPATH='' cd -- "$tests_dir/.." && pwd)

test_output=${TEST_OUTPUT:-$topdir/output/tests}
jobs=${JOBS:-$(nproc)}
qemu=${QEMU:-qemu-system-riscv64}
sbi_firmware=${SBI_FIRMWARE:-default}
make_command=${MAKE:-make}

export LC_ALL=C.UTF-8

require_command()
{
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing test dependency: $1" >&2
		exit 1
	fi
}

for command in \
	"$make_command" "$qemu" expect e2fsck debugfs fsck.fat mtype python3 rg \
	"${CROSS_COMPILE:-riscv64-linux-gnu-}objdump" \
	"${CROSS_COMPILE:-riscv64-linux-gnu-}readelf"; do
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
fixture_log=$test_output/network-fixture.log
fixture_pid=

stop_fixture()
{
	if [ -n "$fixture_pid" ]; then
		kill "$fixture_pid" 2>/dev/null || true
		wait "$fixture_pid" 2>/dev/null || true
		fixture_pid=
	fi
}
trap stop_fixture EXIT

export QEMU=$qemu
export SBI_FIRMWARE=$sbi_firmware
export MAKE_COMMAND=$make_command
export TEST_TOPDIR=$topdir
export KERNEL=$topdir/output/kernel
export ROOT_IMAGE=$root_image
export FAT_IMAGE=$fat_image
export QEMU_LOG=$qemu_log
export QEMU_TIMEOUT=${QEMU_TIMEOUT:-60}

make -C "$topdir" check-opensbi

normalize_kernel_log()
{
	local input=$1
	local output=$2
	local timestamped=$output.timestamped

	tr -d '\r' < "$input" > "$timestamped"
	if ! awk '
		/^\[[[:space:]]*[0-9]+\.[0-9][0-9][0-9][0-9][0-9][0-9]\] / {
			stamp = $0
			sub(/^\[[[:space:]]*/, "", stamp)
			sub(/\].*$/, "", stamp)
			split(stamp, fields, ".")
			seconds = fields[1] + 0
			microseconds = substr(fields[2], 1, 6) + 0
			now = seconds * 1000000 + microseconds
			if (seen && now < previous)
				bad = 1
			previous = now
			seen++
		}
		END { exit seen < 10 || bad }
	' "$timestamped"; then
		echo "kernel timestamps are missing or non-monotonic" >&2
		exit 1
	fi
	if grep -Eq \
		-e '^(Caffeinix |OF: machine:|SBI: spec=|memory: )' \
		-e '^(clocksource: |smp: |irq: PLIC|mmu: |console: )' \
		-e '^(virtio-mmio: |virtio-blk|eth[0-9]+: virtio-net)' \
		-e '^(CPU: |lwIP: |VFS: |init: )' \
		"$timestamped"; then
		echo "kernel message without a timestamp" >&2
		exit 1
	fi
	if grep -Eq \
		-e '^\[[[:space:]]*[0-9]+\.[0-9]{6}\] OPENSBI_BOOT_OK' \
		-e '^\[[[:space:]]*[0-9]+\.[0-9]{6}\] (SCHED_|NETWORK_)' \
		-e '^\[[[:space:]]*[0-9]+\.[0-9]{6}\] (BUSYBOX_|FS_|TTY_)' \
		"$timestamped"; then
		echo "userspace output received a kernel timestamp" >&2
		exit 1
	fi
	sed -E \
		's/^\[[[:space:]]*[0-9]+\.[0-9]{6}\] //' \
		"$timestamped" > "$output"
}

check_boot_log()
{
	local clean=$1
	local cpus=$2
	local block_devices=${3:-1}
	local fat_mounts=0
	local logical
	local marker_count

	marker_count=$(awk \
		'$0 ~ /^SBI: spec=[0-9]+\.[0-9]+ implementation=1 / {
			count++
		} END { print count + 0 }' "$clean")
	if [ "$marker_count" -ne 1 ]; then
		echo "missing or duplicate OpenSBI BASE report" >&2
		exit 1
	fi
	for marker in \
		'^Caffeinix RISC-V 64-bit$' \
		'^OF: machine: .+$' \
		'^clocksource: riscv timer at [0-9]+ MHz$' \
		'^memory: [0-9]+ MiB usable$' \
		"^smp: detected $cpus CPUs$" \
		"^irq: PLIC configured for $cpus CPUs$" \
		'^mmu: Sv39 enabled$' \
		'^console: ttyS0 at 0x[0-9a-f]+ irq=[0-9]+$' \
		"^smp: brought up $cpus CPUs$" \
		'^VFS: mounted root [(]ext4[)] on virtio-blk[0-9]+$' \
		'^VFS: mounted devfs on /dev$' \
		'^VFS: mounted tmpfs on /tmp$' \
		'^init: starting /bin/sh$'; do
		marker_count=$(awk -v marker="$marker" \
			'$0 ~ marker { count++ } END { print count + 0 }' \
			"$clean")
		if [ "$marker_count" -ne 1 ]; then
			echo "unexpected boot summary count: $marker=$marker_count" >&2
			exit 1
		fi
	done
	marker_count=$(awk \
		'$0 ~ /^virtio-blk[0-9]+: [0-9]+ sectors \([0-9]+ MiB\)$/ {
			count++
		} END { print count + 0 }' "$clean")
	if [ "$marker_count" -ne "$block_devices" ]; then
		echo "unexpected block device count: $marker_count" >&2
		exit 1
	fi
	if [ "$block_devices" -gt 1 ]; then
		fat_mounts=1
	fi
	marker_count=$(awk '$0 == "VFS: mounted fat on /mnt/fat" { count++ }
		END { print count + 0 }' "$clean")
	if [ "$marker_count" -ne "$fat_mounts" ]; then
		echo "unexpected FAT mount count: $marker_count" >&2
		exit 1
	fi
	for logical in $(seq 0 $((cpus - 1))); do
		marker_count=$(awk -v logical="$logical" \
			'$0 ~ "^CPU: logical=" logical " hart=.* online$" {
				count++
			} END { print count + 0 }' "$clean")
		if [ "$marker_count" -ne 1 ]; then
			echo "unexpected CPU online count: $logical=$marker_count" >&2
			exit 1
		fi
		marker_count=$(awk -v logical="$logical" \
			'$0 == "CPU: logical=" logical " timer active" {
				count++
			} END { print count + 0 }' "$clean")
		if [ "$marker_count" -ne 1 ]; then
			echo "unexpected CPU timer count: $logical=$marker_count" >&2
			exit 1
		fi
	done
	if [ "$(awk '/^CPU: logical=.* hart=.* online$/ {
			print $3
		}' "$clean" | sort -u | wc -l)" -ne "$cpus" ]; then
		echo "physical hart IDs are not unique" >&2
		exit 1
	fi
	if grep -Eqi \
		'\[PANIC\]|illegal instruction|Unhandled interrupt|timeout' \
		"$clean"; then
		echo "OpenSBI boot log contains a kernel failure" >&2
		exit 1
	fi
}

check_net_device_log()
{
	local clean=$1
	local expected=$2
	local count
	local absent

	count=$(awk \
		'$0 ~ /^eth0: virtio-net MAC ([0-9a-f][0-9a-f]:){5}/ &&
		 $0 ~ /[0-9a-f][0-9a-f]$/ { count++ }
		END { print count + 0 }' "$clean")
	if [ "$count" -ne "$expected" ]; then
		echo "unexpected VirtIO network device count: $count" >&2
		exit 1
	fi
	absent=$(awk '$0 == "lwIP: no external network device" { count++ }
		END { print count + 0 }' "$clean")
	if { [ "$expected" -eq 0 ] && [ "$absent" -ne 1 ]; } ||
	   { [ "$expected" -ne 0 ] && [ "$absent" -ne 0 ]; }; then
		echo "unexpected absent network device count: $absent" >&2
		exit 1
	fi
}

check_boot_memory_latency()
{
	local log=$1
	local maximum_us=$2

	if ! awk -v maximum="$maximum_us" '
		function timestamp(line, value, seconds, microseconds) {
			value = line
			sub(/^\[[[:space:]]*/, "", value)
			sub(/\].*$/, "", value)
			split(value, fields, ".")
			seconds = (fields[1] + 0) * 1000000
			microseconds = substr(fields[2], 1, 6) + 0
			return seconds + microseconds
		}
		/clocksource: riscv timer/ {
			start = timestamp($0)
			found_start = 1
		}
		/mmu: Sv39 enabled/ {
			finish = timestamp($0)
			found_finish = 1
		}
		END {
			if (!found_start || !found_finish || finish < start ||
			    finish - start > maximum)
				exit 1
		}
	' "$log"; then
		echo "large-memory boot setup exceeded $maximum_us us" >&2
		exit 1
	fi
}

run_boot_smoke()
{
	local cpus=$1
	local memory=$2
	local log=$test_output/qemu-smp${cpus}-${memory}.log
	local clean=$test_output/qemu-smp${cpus}-${memory}.clean.log

	export QEMU_CPUS=$cpus
	export QEMU_MEMORY=$memory
	export QEMU_LOG=$log
	expect "$script_dir/run-boot.exp"
	normalize_kernel_log "$log" "$clean"
	if [ "$(awk '$0 == "OPENSBI_BOOT_OK" { count++ }
		END { print count + 0 }' "$clean")" -ne 1 ]; then
		echo "OpenSBI BusyBox smoke marker is missing" >&2
		exit 1
	fi
	if [ "$(awk '$0 == "SCHED_SMOKE_OK" { count++ }
		END { print count + 0 }' "$clean")" -ne 1 ]; then
		echo "scheduler smoke marker is missing" >&2
		exit 1
	fi
	if [ "$(awk '$0 == "NETWORK_LOOPBACK_OK" { count++ }
		END { print count + 0 }' "$clean")" -ne 1 ]; then
		echo "network loopback marker is missing" >&2
		exit 1
	fi
	check_net_device_log "$clean" 0
	if [ "$cpus" -eq 1 ] &&
	   [ "$(awk '$0 == "SCHED_CFS_FAIR_OK" { count++ }
		END { print count + 0 }' "$clean")" -ne 1 ]; then
		echo "CFS fairness marker is missing" >&2
		exit 1
	fi
	check_boot_log "$clean" "$cpus"
}

run_boot_smoke 1 64M
run_boot_smoke 2 192M
run_boot_smoke 3 96M
run_boot_smoke 4 128M
run_boot_smoke 8 256M
run_boot_smoke 9 256M

# A public Ubuntu runner has 16 GiB; keep the guest at one quarter of it.
large_memory=${QEMU_LARGE_MEMORY:-4G}
run_boot_smoke 1 "$large_memory"
check_boot_memory_latency \
	"$test_output/qemu-smp1-$large_memory.clean.log.timestamped" 10000000

export QEMU_CPUS=2
export QEMU_MEMORY=128M
export QEMU_LOG=$test_output/qemu-network-offline.log
expect "$script_dir/run-network-offline.exp"
offline_clean=$test_output/qemu-network-offline.clean.log
normalize_kernel_log "$QEMU_LOG" "$offline_clean"
if [ "$(awk '$0 == "NETWORK_OFFLINE_BOOT_OK" { count++ }
	END { print count + 0 }' "$offline_clean")" -ne 1 ]; then
	echo "offline network boot marker is missing" >&2
	exit 1
fi
check_net_device_log "$offline_clean" 1
check_boot_log "$offline_clean" "$QEMU_CPUS"

python3 "$tests_dir/network_fixture.py" >"$fixture_log" 2>&1 &
fixture_pid=$!
for attempt in $(seq 1 50); do
	if grep -q '^NETWORK_FIXTURE_READY$' "$fixture_log"; then
		break
	fi
	if ! kill -0 "$fixture_pid" 2>/dev/null; then
		cat "$fixture_log" >&2
		echo "network fixture exited before becoming ready" >&2
		exit 1
	fi
	sleep 0.1
done
if ! grep -q '^NETWORK_FIXTURE_READY$' "$fixture_log"; then
	echo "network fixture did not become ready" >&2
	exit 1
fi

export QEMU_CPUS=8
export QEMU_MEMORY=256M
export QEMU_LOG=$qemu_log
expect "$script_dir/run-qemu.exp"
normalize_kernel_log "$qemu_log" "$clean_log"
check_net_device_log "$clean_log" 1
check_boot_log "$clean_log" "$QEMU_CPUS" 2

for marker in \
	BUSYBOX_SHELL_OK \
	BUSYBOX_NC_OK \
	BUSYBOX_WGET_OK \
	SCHED_EXEC_OK \
	SCHED_RUNQUEUE_OK \
	SCHED_MIXED_OK \
	SCHED_RUNTIME_OK \
	SCHED_TTY_WAIT_OK \
	NETWORK_RUNTIME_OK \
	PRESSURE_RUNTIME_OK \
	TTY_METADATA_OK \
	TTY_NONBLOCK_OK \
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
	-e '\[PANIC\]|Unhandled interrupt|irq: unhandled interrupt' \
	-e 'FS_RUNTIME_FAIL=|NETWORK_RUNTIME_FAIL' \
	-e 'PRESSURE_RUNTIME_FAIL|SCHED_RUNTIME_FAIL=|TTY_RUNTIME_FAIL=' \
	"$clean_log"; then
	echo "QEMU log contains a guest failure" >&2
	exit 1
fi

stop_fixture

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

"$script_dir/benchmark-scheduler.py" \
	--qemu "$qemu" \
	--kernel "$KERNEL" \
	--root-image "$root_image" \
	--output "$test_output/scheduler-benchmark.json" \
	--check

make -C "$topdir" clean
make -C "$topdir" -j"$jobs" STACK_OVERFLOW_TEST=1
export QEMU_LOG=$test_output/qemu-stack-overflow.log
expect "$script_dir/run-stack-overflow.exp"
make -C "$topdir" clean
make -C "$topdir" -j"$jobs"

echo QEMU_RUNTIME_OK
