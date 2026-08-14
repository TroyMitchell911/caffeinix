/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-06-01
 * @FilePath: /caffeinix/kernel/main.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <mem_layout.h>
#include <vm.h>
#include <console.h>
#include <palloc.h>
#include <thread.h>
#include <scheduler.h>
#include <trap.h>
#include <printf.h>
#include <plic.h>
#include <process.h>
#include <block_device.h>
#include <file.h>
#include <mystring.h>
#include <ext4fs.h>
#include <fatfs.h>
#include <devfs.h>
#include <tmpfs.h>
#include <vfs.h>
#include <device_model.h>
#include <boot.h>
#include <of.h>
#include <platform_device.h>
#include <irq.h>
#include <char_device.h>
#include <earlycon.h>
#include <ns16550.h>
#include <tty.h>
#include <uart.h>
#include <sbi.h>
#include <timer.h>
#include <cpu.h>
#include <wait.h>
#include <workqueue.h>
#include <virtio.h>
#include <netdevice.h>
#include <network_stack.h>

volatile static uint8 start = 0;
extern char end[];

#ifdef CONFIG_STACK_OVERFLOW_TEST
extern void kernel_stack_overflow_test(void);
#endif

static void main_boot(void)
{
	thread_setup();
	scheduler_init();
	wait_queue_timeout_init();
	workqueue_init();
	trap_init_lock();
	trap_init();
#ifdef CONFIG_STACK_OVERFLOW_TEST
	kernel_stack_overflow_test();
#endif
	timer_init_hart();
	process_init();
	driver_core_init();
	if (driver_core_selftest())
		PANIC("driver core selftest");
	platform_bus_init();
	virtio_bus_init();
	if (platform_core_selftest() < 0)
		PANIC("platform core selftest");
	if (of_platform_populate() < 0)
		PANIC("populate platform devices");
	if (ns16550_init() < 0)
		PANIC("register NS16550 driver");
	if (uart_core_selftest() < 0)
		PANIC("UART core selftest");
	block_device_init();
	net_device_init();
	if (net_core_selftest() < 0)
		PANIC("network core selftest");
	if (net_loopback_init() < 0)
		PANIC("register loopback device");
	if (virtio_blk_init() < 0)
		PANIC("register virtio-blk driver");
	if (virtio_net_init() < 0)
		PANIC("register virtio-net driver");
	if (virtio_mmio_init() < 0)
		PANIC("register virtio-mmio driver");
	network_stack_init();
	ext4fs_init();
	fatfs_init();
	devfs_init();
	tmpfs_init();
	userinit();

	printf("Hello! Caffeinix\n");
	timer_wait_for_interrupt();
	cpu_mark_online();
	cpu_start_secondary_harts();
	__sync_synchronize();
	start = 1;
	cpu_wait_for_secondary_harts();
	scheduler();
	for (;;)
		;
}

static void main_secondary(void)
{
	cpu_secondary_boot_stack_release();
	plic_init_hart();
	trap_init();
	timer_init_hart();
	timer_wait_for_interrupt();
	cpu_mark_online();
	scheduler();
	for (;;)
		;
}

void main(void)
{
	if (cpuid() == 0) {
		int of_status = of_init((void *)boot_dtb_address);

		console_early_init();
		if (of_status < 0)
			PANIC("invalid boot DTB");
		palloc_init();
		cpu_topology_init(boot_hart_id);
		sbi_init(cpu_count());
		timer_init();
		file_init();
		vfs_init();
		irq_init();
		plic_init();
		plic_init_hart();
		char_device_init();
		tty_init();
		printf_init();
		sbi_report();
		kvm_create();
		if (kvm_map_mmio(earlycon_address(), earlycon_size()) < 0)
			PANIC("map early console");
		kvm_init();
		if (cpu_kernel_stack_selftest() < 0)
			PANIC("kernel stack guards");
		cpu_enter_stack(cpu_scheduler_stack_top(), main_boot);
	}

	while (start == 0)
		;
	__sync_synchronize();
	kvm_init();
	cpu_enter_stack(cpu_scheduler_stack_top(), main_secondary);
}
