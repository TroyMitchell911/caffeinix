# Network architecture

Caffeinix separates device discovery, VirtIO transport, network devices,
the protocol stack, and the Linux userspace ABI.  A network driver must not
include lwIP headers, and syscall code must not expose an lwIP descriptor or
structure.

The current receive path is:

```text
Device Tree -> platform bus -> virtio-mmio -> VirtIO bus -> virtio-net
            -> net_device -> net_packet -> lwIP netif -> tcpip thread
            -> lwIP socket -> Caffeinix socket file -> Linux RISC-V UAPI
```

Transmit follows the same path in reverse.  The initial adapter copies in
both directions.  This keeps ownership simple and permits a future protocol
stack or zero-copy adapter without changing NIC drivers.

## Layer ownership

`drivers/virtio/mmio.c` is a platform driver.  It obtains the register range
and interrupt from Device Tree, creates a `virtio_device`, and implements
transport operations.  It contains no block or Ethernet policy.

`drivers/virtio/core.c` matches a `virtio_device` to a `virtio_driver` by
device ID.  It performs the common reset, status, and feature-negotiation
sequence.  `drivers/virtio/ring.c` owns reusable split virtqueues.

`drivers/net/virtio_net.c` owns the VirtIO network queues and headers.  It
preposts receive buffers, completes transmit buffers asynchronously, and
registers one ordinary `net_device`.  Its interrupt callback only schedules
work; frame validation and delivery run in the system workqueue thread.

`net/core/` owns `net_device` registration, administrative and carrier
state, statistics, transmit backpressure, and the fixed packet pool.  It has
one stack-facing receive and state-change boundary and does not depend on
lwIP.

`net/lwip/port/netif.c` is the only packet conversion layer.  It copies a
received `net_packet` into a `pbuf`, and copies a transmit `pbuf` chain into a
`net_packet`.  The rest of lwIP owns ARP, IPv4, ICMP, DHCP, DNS, UDP, TCP,
protocol timers, netconn objects, and its private socket table.

`net/socket/` wraps each lwIP socket in an anonymous VFS file.  It translates
Linux constants, structures, errors, flags, and options explicitly.  File
descriptor allocation, duplication, close-on-exec, ordinary read/write,
polling, and lifetime remain VFS responsibilities.

## Control flow and synchronization

The core serializes registry mutation with `network.lock` and protects each
device's lifecycle, queue, and reference state with `device->lock`.  Neither
lock is held while a driver `start_xmit`, `open`, or `stop` callback runs.
Unregister first waits for borrowed references, lifecycle transitions, and
pending state notifications to drain. The device remains registered and lookup
can acquire new references during this wait. Once it observes those conditions
clear under the core locks, unregister removes the device from lookup, marks it
down, and prevents new transmissions. It then drains active transmit callbacks,
calls the driver's stop method if needed, and emits the final state notification
before returning. The driver may release private memory only after successful
unregister, and must not hold a borrowed reference while waiting for that call.

Receive and state notification each have one registered consumer.  The core
tracks callback ownership per thread and CPU, allowing a consumer to unregister
its own callback while draining other invocations without waiting for itself.
This callback removal is distinct from device unregister, which rejects
recursive teardown from callbacks that would block its progress. State
callbacks raised from workqueue context are requeued on core-owned work;
this prevents a driver-private work item from being freed beneath a callback.

## Packet ownership

`net_packet_alloc()` returns one reference.  A failed
`net_device_xmit()` leaves that reference with the caller.  A successful
transmit transfers it to the driver.  `virtio-net` releases it only after the
device reports the used descriptor.  Loopback transfers it directly to the
receive path.

`netif_receive()` always consumes the receive reference.  The registered
stack callback must either release it or transfer it onward.  The lwIP
adapter copies the frame and releases the packet before handing the `pbuf`
to `tcpip_input()`.

An RX buffer belongs to `virtio-net` from queue submission until completion.
After validating and copying the frame, the driver reposts the same buffer.
If reposting fails, it releases the buffer instead of publishing an invalid
descriptor.

## Adding another NIC

A new driver should:

1. discover and bind through its hardware bus;
2. allocate a `net_device`, set its MAC address, MTU, operations, parent,
   and private driver data;
3. register it with `net_device_register()`;
4. transfer packet ownership only after a successful `start_xmit()`;
5. call `netif_receive()` from sleepable process or deferred context for
   validated frames, never from hard IRQ;
6. use `netif_stop_queue()` and `netif_wake_queue()` for backpressure;
7. report link changes with `net_device_set_carrier()`; and
8. unregister the network device before releasing queues or private data.

The driver does not create a devfs node and does not call lwIP.  A platform
Ethernet MAC can therefore reuse the same network core, and a PCI or USB bus
can be added without changing the stack interface.

## Replacing the protocol stack

Another stack needs adapters for packet transmit, receive, device-state
notifications, kernel threads and waits, and the socket backend.  VirtIO,
NIC drivers, `net_device`, DMA, and Linux UAPI layouts do not need to change.
The current socket implementation does contain the lwIP backend; that code
would be replaced behind the same anonymous VFS-file and `ksocket` boundary.

## Current limits

The initial implementation supports one RX and one TX queue, split rings,
IPv4, and a 1500-byte Ethernet MTU.  It intentionally omits packed rings,
indirect descriptors, event index, multiqueue, offloads, hot unplug, IPv6,
AF_UNIX, packet sockets, netlink, routing configuration, and network
namespaces.  The copy-based data path favors correctness over throughput.

## Reproducible validation

Run `make -C tests qemu` from the kernel tree.  The harness verifies the
network core selftest, device enumeration independent of VirtIO MMIO order,
offline boot, no-DHCP boot, DHCP, and local TCP/UDP/HTTP fixtures.  Fixtures
run on the host's QEMU user backend; the suite deliberately makes no public
Internet request and requires neither a TAP device nor elevated privileges.
