#include <debug.h>
#include <printf.h>
#include <sbi.h>

#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494d45

#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMPL_ID 1
#define SBI_BASE_GET_IMPL_VERSION 2
#define SBI_BASE_PROBE_EXTENSION 3

#define SBI_TIME_SET_TIMER 0

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

void sbi_init(void)
{
	struct sbi_return probe;
	uint64 major, minor;

	sbi_spec_version = sbi_base_value(SBI_BASE_GET_SPEC_VERSION);
	major = (sbi_spec_version >> 24) & 0x7f;
	minor = sbi_spec_version & 0xffffff;
	if (!major && minor < 2)
		PANIC("SBI v0.2 or newer required");
	sbi_impl_id = sbi_base_value(SBI_BASE_GET_IMPL_ID);
	sbi_impl_version = sbi_base_value(SBI_BASE_GET_IMPL_VERSION);
	probe = sbi_base_call(SBI_BASE_PROBE_EXTENSION, SBI_EXT_TIME);
	if (probe.error != SBI_SUCCESS || !probe.value)
		PANIC("SBI TIME extension required");
}

void sbi_report(void)
{
	uint64 major = (sbi_spec_version >> 24) & 0x7f;
	uint64 minor = sbi_spec_version & 0xffffff;

	printf("SBI: spec=%d.%d implementation=%d version=%p\n",
	       (int)major, (int)minor, (int)sbi_impl_id, sbi_impl_version);
}

int64 sbi_set_timer(uint64 deadline)
{
	struct sbi_return result;

	result = sbi_ecall(SBI_EXT_TIME, SBI_TIME_SET_TIMER, deadline,
	                   0, 0, 0, 0, 0);
	return result.error;
}
