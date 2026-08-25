#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
topdir=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
cross_compile=${CROSS_COMPILE:-riscv64-linux-gnu-}
kernel=$topdir/output/kernel
disassembly=$("${cross_compile}objdump" -d "$kernel")

if rg -n 'TRAPFRAME_INFO|tinfo' \
	"$topdir/arch" "$topdir/kernel"; then
	echo "process-global user trapframe selection remains" >&2
	exit 1
fi

if ! rg -q 'csrrw[[:space:]]+a0,sscratch,a0' <<<"$disassembly"; then
	echo "user trap entry does not obtain its trapframe from sscratch" >&2
	exit 1
fi

if ! rg -q 'csrw[[:space:]]+sscratch,a1' <<<"$disassembly"; then
	echo "user trap return does not publish the per-hart trapframe" >&2
	exit 1
fi

echo USER_TRAPFRAME_OK
