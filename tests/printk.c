#include <string.h>
#include <unistd.h>

#include <ktime.h>
#include <printk.h>
#include <printf.h>
#include <spinlock.h>

#define CONSOLE_OUTPUT_MAX 1024

static char console_output[CONSOLE_OUTPUT_MAX];
static uint32 console_output_length;
static uint32 lock_acquisitions;
static uint64 test_time_ns;

void console_putc(int character)
{
	if (console_output_length + 1 < sizeof(console_output))
		console_output[console_output_length++] = character;
	console_output[console_output_length] = 0;
}

uint64 ktime_get_boot_ns(void)
{
	return test_time_ns;
}

void spinlock_init(spinlock_t lock, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->name = name;
}

void spinlock_acquire(spinlock_t lock)
{
	if (lock->locked)
		_exit(2);
	lock->locked = 1;
	lock_acquisitions++;
}

void spinlock_release(spinlock_t lock)
{
	if (!lock->locked)
		_exit(2);
	lock->locked = 0;
}

int spinlock_holding(spinlock_t lock)
{
	return lock->locked;
}

static void console_clear(void)
{
	console_output_length = 0;
	console_output[0] = 0;
}

static int fail(const char *message)
{
	write(STDERR_FILENO, message, strlen(message));
	write(STDERR_FILENO, "\n", 1);
	return 1;
}

static int test_levels(void)
{
	static const char *const names[] = {
		"emerg", "alert", "crit", "err",
		"warn", "notice", "info", "debug",
	};
	int level;

	for (level = 0; level < PRINTK_LEVEL_COUNT; level++) {
		if (strcmp(printk_level_name(level), names[level]))
			return -1;
	}
	return strcmp(printk_level_name(PRINTK_LEVEL_COUNT), "unknown") ?
		-1 : 0;
}

static int test_format_and_filter(void)
{
	struct printk_record record;

	test_time_ns = 12345678;
	pr_info("boot %d", 7);
	if (strcmp(console_output, "[    0.012345] boot 7\n") ||
	    printk_read_record(0, &record) ||
	    record.sequence != 0 || record.timestamp_ns != test_time_ns ||
	    record.level != PRINTK_INFO || strcmp(record.text, "boot 7\n"))
		return -1;

	console_clear();
	printk_set_console_level(PRINTK_WARN);
	test_time_ns++;
	pr_info("hidden");
	test_time_ns++;
	pr_warn("number=%05d long=%lu hex=%04x", -7,
		123456789UL, 0xabU);
	if (strcmp(console_output,
		   "[    0.012345] number=-0007 long=123456789 hex=00ab\n"))
		return -1;
	if (printk_read_record(1, &record) ||
	    strcmp(record.text, "hidden\n") || record.level != PRINTK_INFO)
		return -1;
	return 0;
}

static int test_monotonic_timestamp(void)
{
	struct printk_record first, second;
	uint64 sequence = printk_next_sequence();

	test_time_ns = 90000000;
	pr_debug("later");
	test_time_ns = 80000000;
	pr_debug("earlier clock value");
	if (printk_read_record(sequence, &first) ||
	    printk_read_record(sequence + 1, &second))
		return -1;
	return first.timestamp_ns == 90000000 &&
	       second.timestamp_ns == first.timestamp_ns ? 0 : -1;
}

static int test_ring_wrap(void)
{
	struct printk_record record;
	uint64 old_first = printk_first_sequence();
	uint64 sequence;
	int index;

	printk_set_console_level(PRINTK_EMERG);
	for (index = 0; index < PRINTK_RECORD_MAX + 5; index++)
		pr_debug("wrap %d", index);
	sequence = printk_next_sequence();
	if (printk_first_sequence() != sequence - PRINTK_RECORD_MAX ||
	    !printk_read_record(old_first, &record) ||
	    printk_read_record(sequence - 1, &record) ||
	    record.sequence != sequence - 1)
		return -1;
	return 0;
}

static int test_truncation(void)
{
	struct printk_record record;
	char message[PRINTK_TEXT_MAX + 64];
	uint64 sequence = printk_next_sequence();
	uint32 index;

	for (index = 0; index < sizeof(message) - 1; index++)
		message[index] = 'x';
	message[index] = 0;
	pr_debug("%s", message);
	if (printk_read_record(sequence, &record) ||
	    record.length != PRINTK_TEXT_MAX - 1 ||
	    record.text[PRINTK_TEXT_MAX - 2] != '\n' ||
	    record.text[PRINTK_TEXT_MAX - 1])
		return -1;
	return 0;
}

static int test_time_conversion(void)
{
	if (ktime_ticks_to_ns(12345678, 10000000) != 1234567800ULL ||
	    ktime_ticks_to_ns(1, 0) != 0 ||
	    ktime_ticks_to_ns(~(uint64)0, 1) != ~(uint64)0)
		return -1;
	return 0;
}

static int test_emergency_output(void)
{
	uint32 acquisitions = lock_acquisitions;

	console_clear();
	test_time_ns = 100000000;
	printk_enter_panic();
	pr_emerg("panic-safe");
	if (lock_acquisitions != acquisitions ||
	    strcmp(console_output, "[    0.100000] panic-safe\n"))
		return -1;
	return 0;
}

int main(void)
{
	printf_init();
	printk_init();
	if (test_levels())
		return fail("printk level validation failed");
	if (test_format_and_filter())
		return fail("printk formatting validation failed");
	if (test_monotonic_timestamp())
		return fail("printk monotonic timestamp validation failed");
	if (test_ring_wrap())
		return fail("printk ring validation failed");
	if (test_truncation())
		return fail("printk truncation validation failed");
	if (test_time_conversion())
		return fail("printk time conversion validation failed");
	if (test_emergency_output())
		return fail("printk emergency output validation failed");
	write(STDOUT_FILENO, "PRINTK_OK\n", 10);
	return 0;
}
