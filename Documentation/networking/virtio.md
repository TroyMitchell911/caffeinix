# VirtIO and DMA contracts

The generic VirtIO code separates device drivers from their transport.  The
transport owns registers, interrupts, queue construction, and notification;
the block driver owns requests and their completion lifetime.

## Device and driver lifetime

The `virtio,mmio` platform driver maps each Device Tree resource and obtains
its PLIC interrupt.  Empty QEMU slots have device ID zero and remain harmless.
A non-empty modern transport registers a `virtio_device` on the VirtIO bus.

The VirtIO core resets the device, sets `ACKNOWLEDGE` and `DRIVER`, negotiates
64-bit features including `VIRTIO_F_VERSION_1`, verifies `FEATURES_OK`, and
calls the matching driver.  Only a successful probe receives `DRIVER_OK`.
A failed probe must unwind any queues and private allocations it created.
Removal resets the device before driver teardown, then deletes transport
queues.

Both driver-first and device-first registration are supported by the common
device model.  Unsupported VirtIO IDs remain unbound; they do not change
block-device numbering or root selection.

## DMA boundary

Drivers pass CPU addresses through `dma_map_single()` and use the returned
DMA address in descriptors.  Coherent queue pages come from
`dma_alloc_coherent()`.  The current QEMU implementation supports only
direct-mapped coherent memory, checks the device DMA mask, and rejects a
streaming range whose physical pages are not contiguous.

`dma_wmb()` orders descriptor and available-ring writes before publishing an
available index.  `dma_rmb()` orders a used-index observation before reading
the used element and device-written buffers.  The API deliberately retains
direction, sync, and mask arguments so a real non-coherent platform or IOMMU
can replace the direct mapping without changing drivers.

## Split virtqueues

`virtqueue_create()` allocates separate page-aligned descriptor, available,
and used areas.  Queue size must be a non-zero power of two whose individual
ring areas fit in a page.  The MMIO transport selects the largest supported
power of two up to 128 entries.

`virtqueue_add()` maps every scatter/gather buffer, constructs a descriptor
chain, associates one non-null completion token with its head, publishes the
head in the available ring, and transfers the mapped chain to the device.
It returns a distinct queue-full result so callers can block or apply
backpressure.  A mapping failure unmaps and returns all allocated
descriptors.

`virtqueue_get_used()` validates the used-index distance, completion ID,
token, and complete descriptor chain before unmapping or returning anything
to the free list.  Sixteen-bit shadow indices intentionally wrap.  A device
cannot report more unconsumed used elements than the queue size.

The IRQ handler acknowledges MMIO interrupt status before invoking bounded
queue callbacks.  The block callback completes already-submitted requests
and does not allocate objects, copy userspace data, or sleep.

## Block driver

`drivers/block/virtio_blk.c` uses the generic queue for synchronous read,
write, and optional flush requests.  Multiple callers may have stack-owned
requests in flight.  Descriptor exhaustion sleeps on a driver wait queue;
each completion wakes the request and queue-space waiters.

The generic block core assigns IDs independently of the MMIO slot.  During
boot Caffeinix scans registered block devices and selects the first one that
mounts as the configured ext4 root.  It then mounts a different FAT device,
if present.  Inserting a network or unsupported VirtIO device before the
disks therefore cannot redirect storage.

## Supported VirtIO subset

The implementation requires modern VirtIO MMIO version 2 and split rings.
It supports block ID 2.  Packed rings, indirect descriptors, event index,
network devices, shared interrupts, reset recovery, hot removal, and legacy
MMIO are deferred.  `virtio-blk` negotiates optional flush.
