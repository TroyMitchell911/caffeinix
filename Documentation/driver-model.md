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
