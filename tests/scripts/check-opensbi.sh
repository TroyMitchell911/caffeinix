#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
topdir=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
cross_compile=${CROSS_COMPILE:-riscv64-linux-gnu-}
kernel=$topdir/output/kernel

source_pattern='(^|[^[:alnum:]_])(mhartid|mstatus|mepc|mtvec|mscratch|'
source_pattern+='medeleg|mideleg|mie|mret|pmpaddr[0-9]*|pmpcfg[0-9]*)'
source_pattern+='([^[:alnum:]_]|$)|CLINT|0x0*2000000|-bios[[:space:]]+none'
instruction_pattern='\b(mret|mhartid|mstatus|mepc|mtvec|mscratch|'
instruction_pattern+='medeleg|mideleg|mie|pmpaddr[0-9]*|pmpcfg[0-9]*)\b'

if rg -n "$source_pattern" \
	"$topdir/arch" \
	"$topdir/drivers" \
	"$topdir/kernel" \
	"$topdir/Makefile" \
	"$topdir/tests/scripts/run-qemu.exp" \
	"$topdir/tests/scripts/run-boot.exp"; then
	echo "machine-mode firmware coupling remains in the kernel" >&2
	exit 1
fi

if "${cross_compile}objdump" -d "$kernel" |
	rg -n "$instruction_pattern"; then
	echo "machine-mode instruction remains in the kernel image" >&2
	exit 1
fi

entry=$("${cross_compile}readelf" -h "$kernel" |
	awk '/Entry point address:/ { print $4 }')
if [ "$entry" != 0x80200000 ]; then
	echo "unexpected kernel entry address: $entry" >&2
	exit 1
fi

echo OPENSBI_STATIC_OK
