# Logging, clocks, and diagnostics

## Clock domains

The RISC-V `time` counter and the DT `timebase-frequency` define the kernel's
time source. They are distinct from the active/idle scheduler tick interval.
`timer.c` programs per-hart deadlines through SBI TIME; the common `ktime`
helpers convert counter values without using floating point.

There are three intentionally different time domains:

| Interface | Epoch and use |
| --- | --- |
| `ktime_get_ticks()` | Raw hardware counter ticks |
| `ktime_get_ms()` / `ktime_get_ns()` | Counter epoch, for monotonic deadlines |
| `ktime_get_boot_ns()` | Kernel entry, for boot logs and uptime |
| `ktime_get_realtime_ns()` | Boot RTC sample plus elapsed counter time |

Early entry records the boot sample. A discovered goldfish RTC publishes the
realtime epoch during serialized boot, before ordinary concurrent readers.
Without a usable RTC the kernel explicitly keeps a zero-epoch fallback; this
is not a trustworthy wall clock. The setter is a boot-time publication API,
not a runtime clock-adjustment protocol.

Relative millisecond/nanosecond waits round upward to counter ticks so a
nonzero sub-tick duration does not expire immediately. Very large intervals
saturate where the conversion interface specifies it. Never subtract values
from different clock epochs or confuse timer-frequency ticks with scheduler
quanta.

## Formatting and console routing

`printf.c` is a small freestanding formatter, not libc stdio. It emits through
a callback, can be reused by bounded `snprintf`, and has only the conversions
implemented by `vprintf_emit()`. Kernel callers must not assume locale,
floating-point, or full libc format support.

`console.c` routes output to the registered console writer, with the polling
early console as fallback. Normal output remains synchronous: serial output
cost contributes to the time of the caller. There is no printk consumer
thread or guarantee that a long burst of output is cheap. Console pointer
publication does not pin driver lifetime; registration/removal requires
lifecycle exclusion against concurrent users.

The TTY/UART layer has its own userspace line discipline and transmit queue.
Kernel log formatting must not be confused with terminal echo or userspace
termios policy; see [the driver model](driver-model.md).

## Log records

`printk()` accepts a severity and stores a bounded record with:

- a monotonically increasing sequence number;
- a boot-relative nanosecond timestamp;
- severity, byte length, and a bounded newline-terminated message.

The ring has `PRINTK_RECORD_MAX` slots and each message has
`PRINTK_TEXT_MAX` bytes including its terminator. New records overwrite the
oldest slots. A reader obtains a copy using its sequence number and must
tolerate overwrite between querying the sequence range and reading a record;
no pointer into ring storage is lent to the reader.

All severities enter the ring. The console threshold only controls immediate
emission; its default is INFO. Timestamps are clamped under the log lock to
preserve emission order across CPUs. The displayed prefix shows seconds and
microseconds since kernel entry, not the RTC's calendar time.

Normal logging takes the log spinlock and then the formatter lock. Do not
log recursively while holding either. Before log initialization, messages go
directly to the console without ring storage or a timestamp. Panic mode
bypasses normal serialization to favor diagnostics over consistency; output
from another CPU may interleave and snapshots are not guaranteed coherent.

Boot messages should report meaningful milestones: firmware and clock source,
usable memory, topology, MMU, devices, root mount, and the first executable.
Errors should identify the failed operation and relevant resource. Do not add
unconditional messages to hot paths or report normal retries as failures.

## Load averages and diagnostic snapshots

The load-average worker samples active tasks every five seconds and updates
fixed-point one-, five-, and fifteen-minute averages. Values are scaled by
`LOADAVG_FIXED`; they measure runnable/running and uninterruptibly blocked
work, not a CPU utilization percentage. A dedicated spinlock makes each
three-value read internally consistent.

A serial break requests `debug_dump_state_request()`. A work item moves the
dump out of the UART interrupt and coalesces repeated requests. The dump
includes CPU/thread state, physical and cached memory, ext4, and VirtIO block
diagnostics between `DEBUG_STATE_BEGIN` and `DEBUG_STATE_END` markers.
Subsystem snapshots and unlocked scheduler fields are best-effort samples;
the dump does not stop other CPUs or provide a global consistency point.

## Validation and limits

Run host formatter/log tests with `make -C tests printk`, and all runtime
checks with `make -C tests qemu`. The latter checks boot timestamp ordering,
per-hart timer markers, realtime and no-RTC behavior, procfs time/load data,
serial-break diagnostics, and the intentional stack-overflow path.

There is no persistent log journal, asynchronous console logger, clock
discipline service, or unlimited history. Do not rely on panic output for
durable storage or use the no-RTC timestamp as proof of real-world time.
