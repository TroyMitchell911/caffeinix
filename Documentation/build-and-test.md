# Build and validation boundaries

## Kernel build

The top-level Makefile builds a freestanding RV64GC/LP64D kernel using the
`CROSS_COMPILE` prefix. It selects compiler builtin headers explicitly and
does not link glibc, musl, a private syscall library, or compiler startup
objects. The linker script supplies the OpenSBI payload entry and memory
layout. Host dependencies and standard commands are in [README](../README.md).

`Makefile.build` consumes each directory's `obj-y` list. A trailing slash
denotes a recursive child; local objects and child `built-in.o` files form a
relocatable aggregate. Explicit child targets keep parallel links ordered.
Compiler-generated dependency files track headers. Do not bypass recursive
ordering by making a parent link race its children.

The top-level `qemu` target is the common machine definition used by both
interactive boots and test harnesses. `FS_IMG` is an external raw filesystem
image. Optional FAT storage, user networking, entropy, RAM, CPUs, and firmware
are explicit variables; an empty `NET_BACKEND` omits the NIC rather than
pretending a configured interface has no carrier.

## Userspace fixtures

Normal kernel builds never fetch or compile userspace. The test harness
`tests/scripts/build-rootfs.sh` separately downloads checksum-pinned musl and
BusyBox, builds the test sysroot and programs, and stages images under
`output/tests`. Its configuration and applet manifest live in `configs/`.
The broad dynamic BusyBox and static recovery BusyBox are test artifacts,
not kernel source or a second rootfs repository.

Use the README's manual image workflow for an independently managed rootfs.
Use the test builder for the exact fixtures expected by the runtime suite.
An existing image is not automatically updated when the kernel changes; its
contents come from the builder invocation that produced it.

## Validation layers

1. Build the kernel and run `make -C tests uapi opensbi`. The UAPI tests
   compare selected Linux RV64 numbers/layouts; the firmware check rejects
   machine-mode assumptions and an invalid payload entry.
2. Run host algorithm tests (`printk`, `memrange`, `buddy`, `sv39`, `rbtree`,
   `vma`, `elf`, `virtqueue`, and `block`) plus `qemu-args` and
   `user-trapframe`. These check bounded algorithms and launch/assembly
   invariants without replacing guest tests.
3. Run `make -C tests qemu` for the aggregate suite, including host tests,
   fixture construction, boot matrix, guest integration, controlled network
   peers, performance checks, and the expected stack-guard fault.
4. Run the same GitHub workflow locally with `act` when validating CI
   integration. A passing host build alone is not a runtime/CI pass.

The boot matrix includes different hart counts and RAM sizes; the exhaustive
guest workload is a separate eight-hart run. Do not describe every boot as a
full runtime-suite execution. The one-hart 4 GiB case tests large-memory boot
initialization without touching 4 GiB of guest data.

Network tests use host-side UDP/TCP/HTTP/DNS fixtures behind QEMU networking.
They are network-enabled but deterministic and independent of public servers.
Running a fixture-dependent guest program manually without its host peer can
correctly time out; that is not evidence that DHCP or every socket syscall
is broken. Separate cases omit the NIC or retain a NIC without a DHCP server.

## Results and debugging

Harnesses require explicit completion markers and reject panic, premature
exit, or timeout. Inspect logs and performance JSON under `output/tests`;
do not infer a pass merely from reaching a shell prompt. The exact covered
features and interactive checks are listed in [tests/README](../tests/README.md).

Keep functional changes and regression tests in separate focused patches.
For a comment-only sweep, additionally compare non-comment source tokens and
ensure vendor sources remain byte-for-byte unchanged. Script, assembler,
linker, and Makefile syntax need their own checks: a C lexer cannot validate a
shell shebang or a Makefile comment. Compiler-generated debug information can
change when source line numbers change even though executable behavior does
not.

The GitHub workflow runs for pull requests and pushes to `main`. It checks
out the tested revision, builds independently, then runs QEMU and uploads
diagnostics on failure. It does not notify, dispatch, or update any Caffeinix
web repository.
