#include <debug.h>
#include <ktime.h>
#include <mystring.h>
#include <palloc.h>
#include <printk.h>
#include <random.h>
#include <riscv.h>
#include <spinlock.h>

#define RANDOM_KEY_WORDS 8
#define RANDOM_BLOCK_WORDS 16
#define RANDOM_OUTPUT_OFFSET 8
#define RANDOM_OUTPUT_BYTES 32
#define RANDOM_STRONG_BITS 256

static struct {
	struct spinlock lock;
	uint32 key[RANDOM_KEY_WORDS];
	uint32 nonce[2];
	uint64 counter;
	uint32 entropy_bits;
	uint8 initialized;
	uint8 finalized;
} random_state;

static uint32 rotate_left(uint32 value, uint32 shift)
{
	return (value << shift) | (value >> (32 - shift));
}

#define QUARTER_ROUND(a, b, c, d) do { \
	(a) += (b); (d) ^= (a); (d) = rotate_left((d), 16); \
	(c) += (d); (b) ^= (c); (b) = rotate_left((b), 12); \
	(a) += (b); (d) ^= (a); (d) = rotate_left((d), 8); \
	(c) += (d); (b) ^= (c); (b) = rotate_left((b), 7); \
} while (0)

static void chacha20_block(uint32 output[RANDOM_BLOCK_WORDS])
{
	static const uint32 constants[4] = {
		0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
	};
	uint32 input[RANDOM_BLOCK_WORDS];
	int round;

	memmove(input, constants, sizeof(constants));
	memmove(&input[4], random_state.key, sizeof(random_state.key));
	input[12] = random_state.counter;
	input[13] = random_state.counter >> 32;
	input[14] = random_state.nonce[0];
	input[15] = random_state.nonce[1];
	memmove(output, input, sizeof(input));
	for (round = 0; round < 10; round++) {
		QUARTER_ROUND(output[0], output[4], output[8], output[12]);
		QUARTER_ROUND(output[1], output[5], output[9], output[13]);
		QUARTER_ROUND(output[2], output[6], output[10], output[14]);
		QUARTER_ROUND(output[3], output[7], output[11], output[15]);
		QUARTER_ROUND(output[0], output[5], output[10], output[15]);
		QUARTER_ROUND(output[1], output[6], output[11], output[12]);
		QUARTER_ROUND(output[2], output[7], output[8], output[13]);
		QUARTER_ROUND(output[3], output[4], output[9], output[14]);
	}
	for (round = 0; round < RANDOM_BLOCK_WORDS; round++)
		output[round] += input[round];
	random_state.counter++;
}

#undef QUARTER_ROUND

static void random_mix_locked(const void *buffer, uint32 length)
{
	const uint8 *bytes = buffer;
	uint32 block[RANDOM_BLOCK_WORDS];
	uint32 index, shift;

	for (index = 0; index < length; index++) {
		shift = (index & 3) * 8;
		random_state.key[index & 7] ^=
			(uint32)bytes[index] << shift;
		random_state.key[(index + 3) & 7] +=
			((uint32)bytes[index] + index + 1) * 0x9e3779b9U;
		random_state.key[(index + 5) & 7] = rotate_left(
			random_state.key[(index + 5) & 7], 11);
	}
	chacha20_block(block);
	memmove(random_state.key, block, sizeof(random_state.key));
	random_state.nonce[0] = block[8] ^ block[12];
	random_state.nonce[1] = block[9] ^ block[13];
	random_state.counter ^= (uint64)block[10] |
				((uint64)block[11] << 32);
	memset(block, 0, sizeof(block));
}

void random_init(void)
{
	uint64 seed[] = {
		time_r(),
		ktime_get_ns(),
		(uint64)&random_state,
		(uint64)&random_init,
		palloc_heap_start(),
		palloc_usable_bytes(),
	};
	static const uint32 initial_key[RANDOM_KEY_WORDS] = {
		0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344,
		0xa4093822, 0x299f31d0, 0x082efa98, 0xec4e6c89,
	};

	spinlock_init(&random_state.lock, "random");
	memmove(random_state.key, initial_key, sizeof(initial_key));
	random_state.nonce[0] = 0x452821e6;
	random_state.nonce[1] = 0x38d01377;
	random_state.counter = 1;
	random_state.entropy_bits = 0;
	random_state.finalized = 0;
	random_state.initialized = 1;
	random_mix_locked(seed, sizeof(seed));
	memset(seed, 0, sizeof(seed));
}

void random_add_hardware(const void *buffer, uint32 length)
{
	uint32 bits;

	if (!buffer || !length || !random_state.initialized)
		return;
	spinlock_acquire(&random_state.lock);
	random_mix_locked(buffer, length);
	bits = length > RANDOM_STRONG_BITS / 8 ?
		RANDOM_STRONG_BITS : length * 8;
	if (random_state.entropy_bits < RANDOM_STRONG_BITS - bits)
		random_state.entropy_bits += bits;
	else
		random_state.entropy_bits = RANDOM_STRONG_BITS;
	spinlock_release(&random_state.lock);
}

void random_finalize_boot(void)
{
	int strong;

	if (!random_state.initialized)
		PANIC("finalize uninitialized random generator");
	spinlock_acquire(&random_state.lock);
	random_state.finalized = 1;
	strong = random_state.entropy_bits >= RANDOM_STRONG_BITS;
	spinlock_release(&random_state.lock);
	if (strong)
		pr_info("random: crng initialized");
	else
		pr_warn("random: using untrusted boot-time seed");
}

int get_random_bytes(void *buffer, uint64 length)
{
	uint8 *destination = buffer;
	uint32 block[RANDOM_BLOCK_WORDS];
	uint32 count;

	if ((!buffer && length) || !random_state.initialized)
		return -1;
	spinlock_acquire(&random_state.lock);
	if (!random_state.finalized) {
		spinlock_release(&random_state.lock);
		return -1;
	}
	while (length) {
		chacha20_block(block);
		memmove(random_state.key, block, sizeof(random_state.key));
		count = length > RANDOM_OUTPUT_BYTES ?
			RANDOM_OUTPUT_BYTES : length;
		memmove(destination, &block[RANDOM_OUTPUT_OFFSET], count);
		destination += count;
		length -= count;
	}
	memset(block, 0, sizeof(block));
	spinlock_release(&random_state.lock);
	return 0;
}

int get_random_u64(uint64 *value)
{
	return get_random_bytes(value, sizeof(*value));
}

int random_is_strong(void)
{
	int strong;

	spinlock_acquire(&random_state.lock);
	strong = random_state.entropy_bits >= RANDOM_STRONG_BITS;
	spinlock_release(&random_state.lock);
	return strong;
}
