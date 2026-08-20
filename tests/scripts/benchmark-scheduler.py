#!/usr/bin/env python3

import argparse
import json
import os
import pty
import re
import select
import statistics
import subprocess
import sys
import time


COMMANDS = (
    ":",
    "/bin/pwd >/dev/null",
    "/bin/ls /tmp >/dev/null",
    "/bin/ls /dev >/dev/null",
    "/bin/ls / >/dev/null",
    "/bin/ls /",
)
EXTERNAL_COMMANDS = COMMANDS[1:]
COMPLETION_PREFIX = b"/bin/ec"
COMPLETION_PATTERN = rb"/bin/echo "
PROMPT_PATTERN = rb"(?:^|\r?\n)(?:[~/][~/A-Za-z0-9_.-]* )?# "


class Guest:
    def __init__(self, qemu, kernel, root_image, cpus, memory, timeout):
        self.timeout = timeout
        self.buffer = b""
        self.master, slave = pty.openpty()
        boot_started = time.perf_counter_ns()
        command = [
            qemu,
            "-machine", "virt",
            "-bios", os.environ.get("SBI_FIRMWARE", "default"),
            "-kernel", kernel,
            "-m", memory,
            "-smp", str(cpus),
            "-nographic",
            "-snapshot",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={root_image},if=none,format=raw,id=x0",
            "-device",
            "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        ]
        self.process = subprocess.Popen(
            command,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
        )
        os.close(slave)
        try:
            self.read_regex(PROMPT_PATTERN)
            self.boot_to_shell_ms = (
                time.perf_counter_ns() - boot_started) / 1_000_000
        except Exception:
            self.close()
            raise

    def read_regex(self, pattern):
        expression = re.compile(pattern)
        deadline = time.monotonic() + self.timeout
        while True:
            match = expression.search(self.buffer)
            if match:
                result = self.buffer[:match.end()]
                self.buffer = self.buffer[match.end():]
                return result
            panic = self.buffer.find(b"[PANIC]")
            if panic >= 0 and b"\n" in self.buffer[panic:]:
                tail = self.buffer[-4096:].decode(
                    "utf-8", errors="replace")
                raise RuntimeError(
                    "kernel panic during scheduler benchmark:\n" + tail)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"guest did not produce {pattern!r}")
            readable, _, _ = select.select([self.master], [], [], remaining)
            if not readable:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                raise RuntimeError("QEMU terminal closed") from error
            if not data:
                raise RuntimeError("QEMU exited unexpectedly")
            self.buffer += data

    def run(self, command):
        start = time.perf_counter_ns()
        os.write(self.master, command.encode() + b"\n")
        output = self.read_regex(PROMPT_PATTERN)
        elapsed = (time.perf_counter_ns() - start) / 1_000_000
        if b"[PANIC]" in output:
            tail = output[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError(
                "kernel panic during scheduler benchmark:\n" + tail)
        for error in (b"Bad address", b"not found", b"No such file"):
            if error in output:
                raise RuntimeError(
                    f"benchmark command failed: {command}: {error!r}")
        return elapsed

    def complete(self):
        start = time.perf_counter_ns()
        os.write(self.master, COMPLETION_PREFIX + b"\t")
        output = self.read_regex(COMPLETION_PATTERN)
        elapsed = (time.perf_counter_ns() - start) / 1_000_000
        if b"[PANIC]" in output:
            tail = output[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError(
                "kernel panic during completion benchmark:\n" + tail)
        os.write(self.master, b"\x03")
        self.read_regex(PROMPT_PATTERN)
        return elapsed

    def cpu_ticks(self):
        with open(f"/proc/{self.process.pid}/stat", encoding="ascii") as file:
            fields = file.read().split()
        return int(fields[13]) + int(fields[14])

    def idle_cpu_percent(self, seconds):
        ticks = self.cpu_ticks()
        start = time.monotonic()
        time.sleep(seconds)
        elapsed = time.monotonic() - start
        ticks = self.cpu_ticks() - ticks
        return ticks / os.sysconf("SC_CLK_TCK") / elapsed * 100

    def close(self):
        if self.process.poll() is None:
            try:
                os.write(self.master, b"\x01x")
                self.process.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        os.close(self.master)


def benchmark(args, cpus):
    guest = Guest(args.qemu, args.kernel, args.root_image, cpus,
                  args.memory, args.timeout)
    try:
        for command in COMMANDS:
            for _ in range(args.warmups):
                guest.run(command)
        for _ in range(args.warmups):
            guest.complete()
        idle = guest.idle_cpu_percent(args.idle_seconds)
        commands = {}
        for command in COMMANDS:
            samples = [guest.run(command) for _ in range(args.samples)]
            commands[command] = {
                "median_ms": round(statistics.median(samples), 3),
                "min_ms": round(min(samples), 3),
                "max_ms": round(max(samples), 3),
            }
        samples = [guest.complete() for _ in range(args.samples)]
        return {
            "boot_to_shell_ms": round(guest.boot_to_shell_ms, 3),
            "idle_host_cpu_percent": round(idle, 1),
            "commands": commands,
            "completion": {
                "median_ms": round(statistics.median(samples), 3),
                "min_ms": round(min(samples), 3),
                "max_ms": round(max(samples), 3),
            },
        }
    finally:
        guest.close()


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Measure Caffeinix SMP scheduler behavior under QEMU")
    parser.add_argument("--qemu", default=os.environ.get(
        "QEMU", "qemu-system-riscv64"))
    parser.add_argument("--kernel", default=os.environ.get("KERNEL"))
    parser.add_argument("--root-image", default=os.environ.get("ROOT_IMAGE"))
    parser.add_argument("--memory", default="256M")
    parser.add_argument("--samples", type=int, default=13)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--idle-seconds", type=float, default=3)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--max-idle-cpu", type=float, default=100)
    parser.add_argument("--max-command-ms", type=float, default=500)
    parser.add_argument("--max-completion-ms", type=float, default=500)
    parser.add_argument("--max-smp-ratio", type=float, default=3)
    parser.add_argument("--max-boot-ms", type=float, default=15000)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    if not args.kernel or not args.root_image:
        parser.error("--kernel and --root-image are required")
    args.kernel = os.path.abspath(args.kernel)
    args.root_image = os.path.abspath(args.root_image)
    return args


def main():
    args = parse_arguments()
    result = {"results": {}}

    for cpus in (1, 8):
        result["results"][str(cpus)] = benchmark(args, cpus)
    one = result["results"]["1"]["commands"]
    eight = result["results"]["8"]["commands"]
    result["eight_to_one_median_ratio"] = {
        command: round(eight[command]["median_ms"] /
                       one[command]["median_ms"], 3)
        for command in COMMANDS
    }
    result["eight_to_one_completion_ratio"] = round(
        result["results"]["8"]["completion"]["median_ms"] /
        result["results"]["1"]["completion"]["median_ms"], 3)
    output = json.dumps(result, indent=2, sort_keys=True)
    print(output)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as file:
            file.write(output + "\n")
    if args.check:
        if result["results"]["8"][
                "idle_host_cpu_percent"] >= args.max_idle_cpu:
            print("eight-CPU guest consumes a full host CPU while idle",
                  file=sys.stderr)
            return 1
        for cpus in ("1", "8"):
            measured = result["results"][cpus]
            if measured["boot_to_shell_ms"] > args.max_boot_ms:
                print(
                    f"{cpus}-CPU boot took "
                    f"{measured['boot_to_shell_ms']:.3f} ms",
                    file=sys.stderr)
                return 1
            for command in EXTERNAL_COMMANDS:
                median = measured["commands"][command]["median_ms"]
                if median > args.max_command_ms:
                    print(
                        f"{cpus}-CPU {command!r} median was "
                        f"{median:.3f} ms",
                        file=sys.stderr)
                    return 1
            completion = measured["completion"]["median_ms"]
            if completion > args.max_completion_ms:
                print(
                    f"{cpus}-CPU completion median was "
                    f"{completion:.3f} ms",
                    file=sys.stderr)
                return 1
        for command in EXTERNAL_COMMANDS:
            ratio = result["eight_to_one_median_ratio"][command]
            if ratio > args.max_smp_ratio:
                print(
                    f"{command!r} slowed down {ratio:.3f} times "
                    "with eight CPUs",
                    file=sys.stderr)
                return 1
        if (result["eight_to_one_completion_ratio"] >
                args.max_smp_ratio):
            print(
                "completion slowed down "
                f"{result['eight_to_one_completion_ratio']:.3f} times "
                "with eight CPUs",
                file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
