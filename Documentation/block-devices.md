# Block devices

The block layer presents sector-addressed devices to filesystems and raw
device files.  It owns stable registry IDs, open references, request
validation, synchronous helper operations, and completion waiting.  Hardware
drivers own queueing and must never free caller-owned request storage.

## Request lifecycle

A caller initializes `struct block_request` with `block_request_init()`, then
submits it once.  Read and write requests carry one or more non-empty sector
segments; flush carries no segments.  The core validates the operation,
segment count, device geometry, and range before calling the driver's
`submit()` method.

The driver may complete inline or later from interrupt context, but it calls
`block_request_complete()` exactly once.  The optional `end_io` callback runs
from the system workqueue rather than interrupt context.  `block_request_wait()`
may sleep and returns only after that callback, if any, has finished.  Request
storage, segment storage, buffers, and callback private data must therefore
remain valid until completion.

## Lifetime and locking

`block_device_open()` increments an open count under the registry lock.
`block_device_unregister()` first removes the device from lookup, then sleeps
until all open references close.  It does not cancel submitted requests; a
driver's remove path must first stop its hardware and drain those requests.

Raw writes use `raw_write_lock` where the filesystem-facing block-file layer
needs serialization.  The block core does not impose ordering between distinct
requests, so filesystem or driver code supplies durability ordering and calls
flush where the device supports it.

## Current drivers and limits

`virtio-blk` is the current implementation.  It uses modern VirtIO MMIO split
rings and supports synchronous read, write, and negotiated flush requests.
Requests may have multiple segments, up to `BLOCK_REQUEST_MAX_SEGMENTS` and
the device-specific `max_segments`.  There is no request elevator, discard,
write-zeroes, hot removal, or shared-IRQ support.

## Tests

Run the block core selftest during kernel boot, then run the full QEMU suite:

```
make -C tests qemu
```

The suite exercises registration, open/unregister lifetime, synchronous
completion, VirtIO root mounting, and filesystem I/O.
