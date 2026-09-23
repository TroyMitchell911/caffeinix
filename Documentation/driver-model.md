# Platform and serial driver model

Caffeinix discovers non-enumerable devices from the flattened Device Tree
passed in the RISC-V boot register `a1`. The serial path is deliberately split
into independent layers:

```text
FDT/OF -> platform bus -> NS16550 driver -> UART core -> TTY core
        -> character device -> devfs
```

- The driver core owns bus membership, device lifetime, matching, and probe.
- The platform bus turns `compatible`, `reg`, and `interrupts` properties into
  devices and typed resources.
- The IRQ core owns PLIC handler registration and dispatch.
- The NS16550 driver alone interprets the controller's normal-operation
  registers and FIFO state.
- The UART core owns port numbering and transmit queues.
- The TTY core owns termios, input buffering, and the line discipline.
- The character-device core owns major/minor ranges and operation dispatch.
- devfs exposes names registered by character drivers.

## Lifetime and probe rules

The driver core retains a registration reference on every device. Callers
acquire additional references with `device_get()` only while the device and bus
are live and acquisition is serialized against unregistration. It is not a
concurrent lookup primitive: its registration check precedes the bus lock.
Pair successful acquisition with `device_put()`. The reference protects device
storage but does not prevent driver removal or keep driver-private state alive.
The release callback is the sole authority that may free device storage.
Bus locks protect registry state only: match is checked while
the bus is stable, then `probe()` and `remove()` run without that lock so they
may sleep.  Drivers must leave no asynchronous callback or DMA request
referencing private state when remove returns.

Platform enumeration happens once from available FDT nodes.  `reg` properties
become inclusive memory resources and `interrupts` becomes IRQ resources.
`ioremap()` installs missing identity mappings and may allocate page tables;
callers serialize this setup after kernel page-table creation, outside IRQ
context. It does not choose a separate virtual address, and `iounmap()` leaves
the mappings intact. The initial IRQ implementation has exclusive handlers and
routes enabled sources through the boot-hart PLIC context.

## TTY and UART rules

The UART core owns its bounded transmit ring under the port lock.  Hardware
drivers supply register operations and acknowledge their controller-specific
IRQ state before returning.  The TTY core owns termios and input state under
its TTY lock; process-table state owns foreground process groups.  Blocking
user writes may return short counts when a pending signal interrupts a full
transmit queue.  A serial driver must stop receive/transmit IRQs before it
unregisters the TTY so no callback can retain the port.

The polling early console is selected from `/chosen/stdout-path` before page
tables, allocation, and interrupts are ready. The matching normal UART takes
over kernel output after its platform probe. `/dev/console` forwards user I/O
to that selected TTY; it is not another hardware driver.

## Add an NS16550 port

Describe the controller in the board Device Tree. No trap, PLIC, devfs, or
syscall change is required:

```dts
aliases {
	serial0 = &uart0;
	serial1 = &uart1;
};

chosen {
	stdout-path = "serial0:38400n8";
};

uart1: serial@10010000 {
	compatible = "ns16550a";
	reg = <0x0 0x10010000 0x0 0x100>;
	interrupts = <11>;
	clock-frequency = <3686400>;
	status = "okay";
};
```

`reg-shift` defaults to zero and `reg-io-width` defaults to one; the driver
also accepts a 32-bit register width. A `serialN` alias assigns stable line
`N`. A port without an alias receives the lowest free line. The example
therefore appears as `/dev/ttyS1`, with Linux device number 4:65.

A new UART controller needs a hardware driver with `struct uart_operations`.
It reuses the platform, IRQ, UART, TTY, character-device, console, and devfs
layers. The initial IRQ implementation routes external device interrupts to
the boot hart and does not support shared IRQs or hot removal.

## Tests

`make -C tests qemu` runs boot-time driver-core, platform, UART, block, and
VirtIO selftests before exercising `/dev`, terminal, storage, and networking
runtime paths.  See `Documentation/block-devices.md` for block request
lifetime details and `Documentation/networking/virtio.md` for DMA and queue
ordering.
