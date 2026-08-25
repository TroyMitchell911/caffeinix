#include <debug.h>
#include <printk.h>
#include <sbi.h>

#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494d45
#define SBI_EXT_HSM 0x48534d
#define SBI_EXT_IPI 0x735049
#define SBI_EXT_RFENCE 0x52464e43

#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMPL_ID 1
#define SBI_BASE_GET_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXTENSION 3

#define SBI_TIME_SET_TIMER 0
#define SBI_HSM_HART_START 0
#define SBI_HSM_HART_GET_STATUS 2
#define SBI_IPI_SEND_IPI 0
#define SBI_RFENCE_REMOTE_FENCE_I 0
#define SBI_RFENCE_REMOTE_SFENCE_VMA 1

#define SBI_SUCCESS 0

struct sbi_return {
	int64 error;
	int64 value;
};

static uint64 sbi_spec_version;
static uint64 sbi_impl_id;
static uint64 sbi_impl_version;

static struct sbi_return sbi_ecall(uint64 extension, uint64 function,
				   uint64 argument0, uint64 argument1,
				   uint64 argument2, uint64 argument3,
				   uint64 argument4, uint64 argument5)
{
	register uint64 a0 asm("a0") = argument0;
	register uint64 a1 asm("a1") = argument1;
	register uint64 a2 asm("a2") = argument2;
	register uint64 a3 asm("a3") = argument3;
	register uint64 a4 asm("a4") = argument4;
	register uint64 a5 asm("a5") = argument5;
	register uint64 a6 asm("a6") = function;
	register uint64 a7 asm("a7") = extension;
	struct sbi_return result;

	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1)
		     : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6),
		       "r"(a7)
		     : "memory");
	result.error = a0;
	result.value = a1;
	return result;
}

static struct sbi_return sbi_base_call(uint64 function, uint64 argument)
{
	return sbi_ecall(SBI_EXT_BASE, function, argument, 0, 0, 0, 0, 0);
}

static uint64 sbi_base_value(uint64 function)
{
	struct sbi_return result = sbi_base_call(function, 0);

	if (result.error != SBI_SUCCESS)
		PANIC("SBI BASE call failed");
	return result.value;
}

static int sbi_extension_available(uint64 extension)
{
	struct sbi_return probe;

	probe = sbi_base_call(SBI_BASE_PROBE_EXTENSION, extension);
	return probe.error == SBI_SUCCESS && probe.value;
}

void sbi_init(int requested_cpus)
{
	uint64 major, minor;

	sbi_spec_version = sbi_base_value(SBI_BASE_GET_SPEC_VERSION);
	major = (sbi_spec_version >> 24) & 0x7f;
	minor = sbi_spec_version & 0xffffff;
	if (!major && minor < 2)
		PANIC("SBI v0.2 or newer required");
	sbi_impl_id = sbi_base_value(SBI_BASE_GET_IMPL_ID);
	sbi_impl_version = sbi_base_value(SBI_BASE_GET_IMPL_VERSION);
	if (!sbi_extension_available(SBI_EXT_TIME))
		PANIC("SBI TIME extension required");
	if (requested_cpus > 1 && !sbi_extension_available(SBI_EXT_HSM))
		PANIC("SBI HSM extension required");
	if (requested_cpus > 1 && !sbi_extension_available(SBI_EXT_IPI))
		PANIC("SBI IPI extension required");
	if (requested_cpus > 1 && !sbi_extension_available(SBI_EXT_RFENCE))
		PANIC("SBI RFENCE extension required");
}

void sbi_report(void)
{
	uint64 major = (sbi_spec_version >> 24) & 0x7f;
	uint64 minor = sbi_spec_version & 0xffffff;

	pr_info("SBI: spec=%d.%d implementation=%d version=%p",
		(int)major, (int)minor, (int)sbi_impl_id, sbi_impl_version);
}

int64 sbi_set_timer(uint64 deadline)
{
	struct sbi_return result;

	result = sbi_ecall(SBI_EXT_TIME, SBI_TIME_SET_TIMER, deadline,
	                   0, 0, 0, 0, 0);
	return result.error;
}

int64 sbi_hart_start(uint64 hart_id, uint64 start_address, uint64 opaque)
{
	struct sbi_return result;

	result = sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_START, hart_id,
	                   start_address, opaque, 0, 0, 0);
	return result.error;
}

int64 sbi_hart_get_status(uint64 hart_id, uint64 *status)
{
	struct sbi_return result;

	if (!status)
		return -3;
	result = sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_GET_STATUS, hart_id,
	                   0, 0, 0, 0, 0);
	if (!result.error)
		*status = result.value;
	return result.error;
}

int64 sbi_send_ipi(uint64 hart_id)
{
	struct sbi_return result;

	/* A one-bit mask based at hart_id also handles sparse hart IDs. */
	result = sbi_ecall(SBI_EXT_IPI, SBI_IPI_SEND_IPI, 1, hart_id,
	                   0, 0, 0, 0);
	return result.error;
}

int64 sbi_remote_fence_i(uint64 hart_id)
{
	struct sbi_return result;

	/* A one-bit mask based at hart_id also handles sparse hart IDs. */
	result = sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_REMOTE_FENCE_I,
	                   1, hart_id, 0, 0, 0, 0);
	return result.error;
}

int64 sbi_remote_sfence_vma(uint64 hart_id, uint64 start, uint64 size)
{
	struct sbi_return result;

	/* A one-bit mask based at hart_id also handles sparse hart IDs. */
	result = sbi_ecall(SBI_EXT_RFENCE,
	                   SBI_RFENCE_REMOTE_SFENCE_VMA,
	                   1, hart_id, start, size, 0, 0);
	return result.error;
}
