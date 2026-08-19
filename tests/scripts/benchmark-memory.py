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


MEMORY_PATTERN = re.compile(
    rb"memory pages=(\d+) free=(\d+) used=(\d+)")
PAGE_CACHE_PATTERN = re.compile(
    rb"page-cache pages=(\d+) hits=(\d+) misses=(\d+) "
    rb"reclaimed=(\d+) mapped=(\d+) shared=(\d+) refs=(\d+)")
PROMPT_PATTERN = rb"(?:^|\r?\n)(?:[~/][~/A-Za-z0-9_.-]* )?# "


class Guest:
    def __init__(self, args):
        self.timeout = args.timeout
        self.buffer = b""
        self.sync_sequence = 0
        self.master, slave = pty.openpty()
        command = [
            args.qemu,
            "-machine", "virt",
            "-bios", args.sbi_firmware,
            "-kernel", args.kernel,
            "-m", args.memory,
            "-smp", "1",
            "-nographic",
            "-snapshot",
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={args.root_image},if=none,format=raw,id=x0",
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
                    "kernel panic during memory benchmark:\n" + tail)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                tail = self.buffer[-4096:].decode(
                    "utf-8", errors="replace")
                raise TimeoutError(
                    f"guest did not produce {pattern!r}:\n{tail}")
            readable, _, _ = select.select(
                [self.master], [], [], remaining)
            if not readable:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                raise RuntimeError("QEMU terminal closed") from error
            if not data:
                raise RuntimeError("QEMU exited unexpectedly")
            self.buffer += data

    def write(self, value):
        os.write(self.master, value)

    def sync(self):
        self.sync_sequence += 1
        marker = f"MEMORY_BENCH_SYNC_{self.sync_sequence}"
        self.write(f"echo {marker}\n".encode())
        self.read_regex(
            rb"\r?\n" + marker.encode() + rb"\r?\n")
        self.read_regex(PROMPT_PATTERN)

    def command(self, command, marker):
        start = time.perf_counter_ns()
        self.write(command.encode() + b"\n")
        self.read_regex(rb"\r?\n" + re.escape(marker) + rb"\r?\n")
        self.read_regex(PROMPT_PATTERN)
        return (time.perf_counter_ns() - start) / 1_000_000

    def shell(self, command):
        self.write(command.encode() + b"\n")
        self.read_regex(PROMPT_PATTERN)

    def launch_hold(self, binary, ready_path, release_path):
        command = f"{binary} hold {ready_path} {release_path} &"
        marker = f"MEMORY_HOLD_READY {ready_path}".encode()
        self.write(command.encode() + b"\n")
        self.read_regex(re.escape(marker) + rb"\r?\n")
        self.sync()

    def release_holds(self, release_path, count):
        self.write(f"touch {release_path}\n".encode())
        for _ in range(count):
            self.read_regex(rb"MEMORY_HOLD_DONE /tmp/[^\r\n]+\r?\n")
        self.sync()

    def snapshot(self):
        self.sync()
        self.write(b"\x01b")
        beginning = self.read_regex(rb"DEBUG_STATE_BEGIN[^\r\n]*\r?\n")
        ending = self.read_regex(rb"DEBUG_STATE_END\r?\n")
        output = beginning + ending
        memory = MEMORY_PATTERN.search(output)
        page_cache = PAGE_CACHE_PATTERN.search(output)
        if not memory or not page_cache:
            raise RuntimeError("debug dump lacks memory residency data")
        return {
            "memory": {
                "pages": int(memory.group(1)),
                "free": int(memory.group(2)),
                "used": int(memory.group(3)),
            },
            "page_cache": {
                "pages": int(page_cache.group(1)),
                "hits": int(page_cache.group(2)),
                "misses": int(page_cache.group(3)),
                "reclaimed": int(page_cache.group(4)),
                "mapped": int(page_cache.group(5)),
                "shared": int(page_cache.group(6)),
                "references": int(page_cache.group(7)),
            },
        }

    def close(self):
        if self.process.poll() is None:
            try:
                self.write(b"\x01x")
                self.process.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        os.close(self.master)


def latency_summary(samples):
    return {
        "median_ms": round(statistics.median(samples), 3),
        "min_ms": round(min(samples), 3),
        "max_ms": round(max(samples), 3),
    }


def measure_command(guest, binary, mode, marker, warmups, samples):
    command = f"{binary} {mode}"
    cold = guest.command(command, marker)
    for _ in range(warmups):
        guest.command(command, marker)
    measured = [guest.command(command, marker) for _ in range(samples)]
    result = latency_summary(measured)
    result["cold_ms"] = round(cold, 3)
    return result


def measure_resident(guest, binary, name):
    ready = f"/tmp/memory-{name}-ready"
    release = f"/tmp/memory-{name}-release"
    guest.shell(f"rm -f {ready} {release}")
    guest.launch_hold(binary, ready, release)
    held = guest.snapshot()
    guest.release_holds(release, 1)
    released = guest.snapshot()
    guest.shell(f"rm -f {ready} {release}")
    return {
        "held": held,
        "released": released,
        "resident_pages": held["memory"]["used"]
        - released["memory"]["used"],
        "mapped_page_references": held["page_cache"]["references"]
        - released["page_cache"]["references"],
    }


def measure_sharing(guest, count):
    release = "/tmp/memory-shared-release"
    ready_paths = [f"/tmp/memory-shared-{index}" for index in range(count)]
    guest.shell("rm -f /tmp/memory-shared-*")
    baseline = guest.snapshot()
    for ready in ready_paths:
        guest.launch_hold("/bin/memory-dynamic", ready, release)
    held = guest.snapshot()
    guest.release_holds(release, count)
    released = guest.snapshot()
    guest.shell("rm -f /tmp/memory-shared-*")
    resident_pages = held["memory"]["used"] - released["memory"]["used"]
    return {
        "processes": count,
        "baseline": baseline,
        "held": held,
        "released": released,
        "resident_pages": resident_pages,
        "resident_pages_per_process": round(resident_pages / count, 3),
        "shared_pages": held["page_cache"]["shared"]
        - released["page_cache"]["shared"],
        "mapping_references": held["page_cache"]["references"]
        - released["page_cache"]["references"],
    }


def check_result(args, result):
    failures = []
    startup = result["startup"]
    fork = result["fork"]
    resident = result["resident"]
    sharing = result["sharing"]
    for kind in ("static", "dynamic"):
        if startup[kind]["median_ms"] > args.max_startup_ms:
            failures.append(f"{kind} startup exceeds limit")
        if fork[kind]["median_ms"] > args.max_fork_ms:
            failures.append(f"{kind} fork exceeds limit")
        pages = resident[kind]["resident_pages"]
        if pages <= 0 or pages > args.max_single_resident_pages:
            failures.append(f"{kind} resident pages outside limit")
    if result["dynamic_to_static_startup_ratio"] > args.max_startup_ratio:
        failures.append("dynamic-to-static startup ratio exceeds limit")
    if sharing["shared_pages"] < args.min_shared_pages:
        failures.append("too few physically shared page-cache pages")
    minimum_references = args.min_shared_refs_per_process * args.processes
    if sharing["mapping_references"] < minimum_references:
        failures.append("too few shared page-cache mapping references")
    pages_per_process = sharing["resident_pages_per_process"]
    if pages_per_process <= 0 or pages_per_process > args.max_resident_pages:
        failures.append("dynamic resident pages per process outside limit")
    return failures


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Measure Caffeinix dynamic userspace memory behavior")
    parser.add_argument("--qemu", default=os.environ.get(
        "QEMU", "qemu-system-riscv64"))
    parser.add_argument("--kernel", default=os.environ.get("KERNEL"))
    parser.add_argument("--root-image", default=os.environ.get("ROOT_IMAGE"))
    parser.add_argument("--sbi-firmware", default=os.environ.get(
        "SBI_FIRMWARE", "default"))
    parser.add_argument("--memory", default="256M")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--fork-count", type=int, default=32)
    parser.add_argument("--processes", type=int, default=12)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--max-startup-ms", type=float, default=2000)
    parser.add_argument("--max-startup-ratio", type=float, default=8)
    parser.add_argument("--max-fork-ms", type=float, default=10000)
    parser.add_argument("--max-single-resident-pages", type=int, default=128)
    parser.add_argument("--max-resident-pages", type=float, default=128)
    parser.add_argument("--min-shared-pages", type=int, default=4)
    parser.add_argument("--min-shared-refs-per-process", type=int, default=2)
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
    guest = Guest(args)
    try:
        startup = {
            "static": measure_command(
                guest, "/bin/memory-static", "once",
                b"MEMORY_ONCE_OK", args.warmups, args.samples),
            "dynamic": measure_command(
                guest, "/bin/memory-dynamic", "once",
                b"MEMORY_ONCE_OK", args.warmups, args.samples),
        }
        fork_mode = f"fork {args.fork_count}"
        fork_marker = f"MEMORY_FORK_OK {args.fork_count}".encode()
        fork = {
            "static": measure_command(
                guest, "/bin/memory-static", fork_mode, fork_marker,
                1, 3),
            "dynamic": measure_command(
                guest, "/bin/memory-dynamic", fork_mode, fork_marker,
                1, 3),
        }
        result = {
            "startup": startup,
            "dynamic_to_static_startup_ratio": round(
                startup["dynamic"]["median_ms"]
                / startup["static"]["median_ms"], 3),
            "fork_count": args.fork_count,
            "fork": fork,
            "resident": {
                "static": measure_resident(
                    guest, "/bin/memory-static", "static"),
                "dynamic": measure_resident(
                    guest, "/bin/memory-dynamic", "dynamic"),
            },
            "sharing": measure_sharing(guest, args.processes),
        }
    finally:
        guest.close()

    failures = check_result(args, result) if args.check else []
    result["limits"] = {
        "max_startup_ms": args.max_startup_ms,
        "max_startup_ratio": args.max_startup_ratio,
        "max_fork_ms": args.max_fork_ms,
        "max_single_resident_pages": args.max_single_resident_pages,
        "max_resident_pages_per_process": args.max_resident_pages,
        "min_shared_pages": args.min_shared_pages,
        "min_shared_refs_per_process": args.min_shared_refs_per_process,
    }
    result["failures"] = failures
    output = json.dumps(result, indent=2, sort_keys=True)
    print(output)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as file:
            file.write(output + "\n")
    for failure in failures:
        print(f"memory benchmark: {failure}", file=sys.stderr)
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
