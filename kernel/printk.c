/*
 * Bounded timestamped kernel log with synchronous console emission.
 *
 * The ring retains every accepted severity independently of the console
 * threshold. Sequence numbers detect overwritten records; timestamps are
 * boot-relative and clamped to keep emission order monotonic. Panic output
 * bypasses locks and sacrifices concurrent consistency for diagnostics.
 */
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

/*
 * Snapshot whether serialization is enabled before acquiring the log lock;
 * the returned flag pairs with unlock even when panic mode disables future
 * locking.
 */
static int printk_lock(void)
{
	int locking = printk_state.locking;

	if (locking)
		spinlock_acquire(&printk_state.lock);
	return locking;
}

/*
 * Release the log lock only if this call actually acquired it, using the
 * saved state rather than the current panic flag.
 */
static void printk_unlock(int locking)
{
	if (locking)
		spinlock_release(&printk_state.lock);
}

/*
 * Compute the oldest sequence still retained by the fixed-size ring; the
 * caller holds log serialization when enabled.
 */
static uint64 printk_first_sequence_locked(void)
{
	uint64 next = printk_state.next_sequence;

	return next > PRINTK_RECORD_MAX ? next - PRINTK_RECORD_MAX : 0;
}

/*
 * Truncate formatting to the fixed record buffer while reserving one byte for
 * the terminator; the formatter can continue emitting discarded characters.
 */
static void printk_buffer_emit(int character, void *context)
{
	struct printk_buffer *buffer = context;

	if (buffer->length + 1 < buffer->capacity)
		buffer->text[buffer->length++] = character;
}

/*
 * Build a bounded newline-terminated record. Even a completely filled record
 * ends with a newline and NUL so downstream readers do not need
 * truncation-specific framing.
 */
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

/*
 * Replace the sequence-selected ring slot while the log lock is held; readers
 * must validate sequence numbers because a slot is reused.
 */
static void printk_store(const struct printk_record *record)
{
	printk_state.records[record->sequence % PRINTK_RECORD_MAX] = *record;
}

/*
 * Render boot-relative seconds and microseconds before the message. Called
 * under the log lock, which orders timestamps and console lines across CPUs.
 */
static void printk_console(const struct printk_record *record)
{
	uint64 seconds = record->timestamp_ns / NSEC_PER_SEC;
	uint64 microseconds = record->timestamp_ns % NSEC_PER_SEC / 1000;

	printf("[%5lu.%06lu] %s", seconds, microseconds, record->text);
}

/**
 * printk_init() - Initialize the bounded kernel log
 *
 * Resets sequence numbering and selects INFO as the console threshold.
 *
 * Context: Boot once after printf and clock setup, before concurrent loggers.
 */
void printk_init(void)
{
	memset(&printk_state, 0, sizeof(printk_state));
	spinlock_init(&printk_state.lock, "printk");
	printk_state.console_level = PRINTK_INFO;
	printk_state.locking = 1;
	printk_state.initialized = 1;
}

/**
 * printk() - Record a severity-tagged message and optionally print it
 * @level: Message severity; invalid values are treated as PRINTK_ERR.
 * @format: Non-NULL format accepted by vprintf_emit().
 * @...: Arguments matching @format.
 *
 * Before initialization, messages go directly to the console without ring
 * storage or a timestamp. Afterwards all levels enter the bounded ring even
 * when the console threshold suppresses emission.
 *
 * Context: Non-sleeping console context; takes the log lock then the printf
 *          lock during normal operation. Must not recurse while either is
 *          held.
 */
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

/**
 * panic() - Report a fatal kernel error and stop the caller
 * @message: Readable diagnostic string passed to the emergency logger.
 *
 * Disables log and printf locking before emission. Other harts are not
 * synchronously stopped by this routine.
 *
 * Context: Fatal path from any supervisor context; does not return.
 */
void panic(char *message)
{
	paniced = 1;
	printk_enter_panic();
	pr_emerg("[PANIC]: %s", message);
	for (;;)
		;
}

/**
 * printk_enter_panic() - Bypass logging and formatting locks
 *
 * Context: Fatal path only; concurrent ring consistency is no longer
 *          guaranteed.
 */
void printk_enter_panic(void)
{
	printk_state.locking = 0;
	printf_enter_panic();
}

/**
 * printk_set_console_level() - Change the console severity threshold
 * @level: Highest numeric severity to print; invalid values are ignored.
 *
 * This affects console output, not whether records enter the ring.
 *
 * Context: After printk_init(); takes the log spinlock unless panic mode is
 *          active.
 */
void printk_set_console_level(enum printk_level level)
{
	int locking;

	if (level < PRINTK_EMERG || level >= PRINTK_LEVEL_COUNT)
		return;
	locking = printk_lock();
	printk_state.console_level = level;
	printk_unlock(locking);
}

/**
 * printk_get_console_level() - Read the current console threshold
 *
 * Context: After printk_init(); takes the log spinlock unless panic mode is
 *          active.
 * Return: Current highest numeric severity eligible for console emission.
 */
enum printk_level printk_get_console_level(void)
{
	enum printk_level level;
	int locking = printk_lock();

	level = printk_state.console_level;
	printk_unlock(locking);
	return level;
}

/**
 * printk_level_name() - Name a message severity
 * @level: Severity value to describe.
 *
 * Context: Any context; does not lock or sleep.
 * Return: Borrowed static string, or "unknown" for an invalid value.
 */
const char *printk_level_name(enum printk_level level)
{
	if (level < PRINTK_EMERG || level >= PRINTK_LEVEL_COUNT)
		return "unknown";
	return printk_level_names[level];
}

/**
 * printk_first_sequence() - Snapshot the oldest retained record number
 *
 * Context: After printk_init(); briefly takes the log spinlock.
 * Return: Oldest currently readable sequence, which may advance before a
 *         later read.
 */
uint64 printk_first_sequence(void)
{
	uint64 sequence;
	int locking = printk_lock();

	sequence = printk_first_sequence_locked();
	printk_unlock(locking);
	return sequence;
}

/**
 * printk_next_sequence() - Snapshot the next record number to allocate
 *
 * Context: After printk_init(); briefly takes the log spinlock.
 * Return: Exclusive upper bound of currently recorded sequences.
 */
uint64 printk_next_sequence(void)
{
	uint64 sequence;
	int locking = printk_lock();

	sequence = printk_state.next_sequence;
	printk_unlock(locking);
	return sequence;
}

/**
 * printk_read_record() - Copy a retained record by sequence
 * @sequence: Exact sequence number to retrieve, not a ring index.
 * @record: Non-NULL kernel destination for a copied record.
 *
 * The returned structure belongs to the caller and remains valid after its
 * ring slot is overwritten.
 *
 * Context: Non-sleeping context; takes the log spinlock during a normal read.
 * Return: %0 on success; %-1 for NULL output, an uninitialized log, or an
 *         absent/overwritten sequence.
 */
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
