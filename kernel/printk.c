#include <debug.h>
#include <ktime.h>
#include <mystring.h>
#include <printf.h>
#include <printk.h>
#include <spinlock.h>
#include <stdarg.h>

volatile uint8 paniced;

static const char *const printk_level_names[] = {
	"emerg", "alert", "crit", "err",
	"warn", "notice", "info", "debug",
};

static struct {
	struct spinlock lock;
	struct printk_record records[PRINTK_RECORD_MAX];
	uint64 next_sequence;
	uint64 last_timestamp_ns;
	enum printk_level console_level;
	uint8 initialized;
	uint8 locking;
} printk_state;

struct printk_buffer {
	char *text;
	uint32 capacity;
	uint32 length;
};

static int printk_lock(void)
{
	int locking = printk_state.locking;

	if (locking)
		spinlock_acquire(&printk_state.lock);
	return locking;
}

static void printk_unlock(int locking)
{
	if (locking)
		spinlock_release(&printk_state.lock);
}

static uint64 printk_first_sequence_locked(void)
{
	uint64 next = printk_state.next_sequence;

	return next > PRINTK_RECORD_MAX ? next - PRINTK_RECORD_MAX : 0;
}

static void printk_buffer_emit(int character, void *context)
{
	struct printk_buffer *buffer = context;

	if (buffer->length + 1 < buffer->capacity)
		buffer->text[buffer->length++] = character;
}

static void printk_format(struct printk_record *record, const char *format,
			  va_list arguments)
{
	struct printk_buffer buffer = {
		.text = record->text,
		.capacity = sizeof(record->text),
	};

	vprintf_emit(printk_buffer_emit, &buffer, format, arguments);
	if (!buffer.length || buffer.text[buffer.length - 1] != '\n') {
		if (buffer.length + 1 < buffer.capacity)
			buffer.text[buffer.length++] = '\n';
		else
			buffer.text[buffer.length - 1] = '\n';
	}
	buffer.text[buffer.length] = 0;
	record->length = buffer.length;
}

static void printk_store(const struct printk_record *record)
{
	printk_state.records[record->sequence % PRINTK_RECORD_MAX] = *record;
}

static void printk_console(const struct printk_record *record)
{
	uint64 seconds = record->timestamp_ns / NSEC_PER_SEC;
	uint64 microseconds = record->timestamp_ns % NSEC_PER_SEC / 1000;

	printf("[%5lu.%06lu] %s", seconds, microseconds, record->text);
}

void printk_init(void)
{
	memset(&printk_state, 0, sizeof(printk_state));
	spinlock_init(&printk_state.lock, "printk");
	printk_state.console_level = PRINTK_INFO;
	printk_state.locking = 1;
	printk_state.initialized = 1;
}

void printk(enum printk_level level, const char *format, ...)
{
	struct printk_record record = { 0 };
	va_list arguments;
	int locking;

	if (level < PRINTK_EMERG || level >= PRINTK_LEVEL_COUNT)
		level = PRINTK_ERR;
	va_start(arguments, format);
	printk_format(&record, format, arguments);
	va_end(arguments);
	if (!printk_state.initialized) {
		printf("%s", record.text);
		return;
	}
	locking = printk_lock();
	record.sequence = printk_state.next_sequence++;
	record.timestamp_ns = ktime_get_boot_ns();
	if (record.timestamp_ns < printk_state.last_timestamp_ns)
		record.timestamp_ns = printk_state.last_timestamp_ns;
	else
		printk_state.last_timestamp_ns = record.timestamp_ns;
	record.level = level;
	printk_store(&record);
	if (level <= printk_state.console_level)
		printk_console(&record);
	printk_unlock(locking);
}

void panic(char *message)
{
	paniced = 1;
	printk_enter_panic();
	pr_emerg("[PANIC]: %s", message);
	for (;;)
		;
}

void printk_enter_panic(void)
{
	printk_state.locking = 0;
	printf_enter_panic();
}

void printk_set_console_level(enum printk_level level)
{
	int locking;

	if (level < PRINTK_EMERG || level >= PRINTK_LEVEL_COUNT)
		return;
	locking = printk_lock();
	printk_state.console_level = level;
	printk_unlock(locking);
}

enum printk_level printk_get_console_level(void)
{
	enum printk_level level;
	int locking = printk_lock();

	level = printk_state.console_level;
	printk_unlock(locking);
	return level;
}

const char *printk_level_name(enum printk_level level)
{
	if (level < PRINTK_EMERG || level >= PRINTK_LEVEL_COUNT)
		return "unknown";
	return printk_level_names[level];
}

uint64 printk_first_sequence(void)
{
	uint64 sequence;
	int locking = printk_lock();

	sequence = printk_first_sequence_locked();
	printk_unlock(locking);
	return sequence;
}

uint64 printk_next_sequence(void)
{
	uint64 sequence;
	int locking = printk_lock();

	sequence = printk_state.next_sequence;
	printk_unlock(locking);
	return sequence;
}

int printk_read_record(uint64 sequence, struct printk_record *record)
{
	uint64 first;
	int result = -1;
	int locking;

	if (!record || !printk_state.initialized)
		return -1;
	locking = printk_lock();
	first = printk_first_sequence_locked();
	if (sequence >= first && sequence < printk_state.next_sequence &&
	    printk_state.records[sequence % PRINTK_RECORD_MAX].sequence ==
		sequence) {
		*record = printk_state.records[sequence % PRINTK_RECORD_MAX];
		result = 0;
	}
	printk_unlock(locking);
	return result;
}
