#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <buddy.h>
#include <riscv.h>

#define TEST_ORDER 10
#define TEST_SIZE (PGSIZE << TEST_ORDER)

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "buddy check failed at line %d\n", __LINE__); \
		exit(1); \
	} \
} while (0)

static void *test_memory;
static uint8 *test_states;

static void setup_allocator(struct buddy_allocator *allocator,
			    uint64 reserved_pages)
{
	int status;

	status = posix_memalign(&test_memory, TEST_SIZE, TEST_SIZE);
	CHECK(status == 0);
	test_states = malloc(TEST_SIZE / PGSIZE);
	CHECK(test_states != 0);
	buddy_init(allocator);
	CHECK(buddy_add_region(allocator, (uint64)test_memory,
			       (uint64)test_memory + TEST_SIZE,
			       (uint64)test_memory + reserved_pages * PGSIZE,
			       test_states, TEST_SIZE / PGSIZE) == 0);
}

static void teardown_allocator(void)
{
	free(test_states);
	free(test_memory);
}

static void test_split_and_coalesce(void)
{
	struct buddy_allocator allocator;
	void *pages[1 << TEST_ORDER];
	uint64 i;

	setup_allocator(&allocator, 0);
	CHECK(buddy_free_page_count(&allocator) == 1UL << TEST_ORDER);
	for (i = 0; i < 1UL << TEST_ORDER; i++) {
		pages[i] = buddy_alloc(&allocator, 0);
		CHECK(pages[i] != 0);
		memset(pages[i], (int)i, PGSIZE);
	}
	CHECK(buddy_alloc(&allocator, 0) == 0);
	CHECK(buddy_free_page_count(&allocator) == 0);
	for (i = 0; i < 1UL << TEST_ORDER; i += 2)
		CHECK(buddy_free(&allocator, pages[i], 0) == 0);
	for (i = 1; i < 1UL << TEST_ORDER; i += 2)
		CHECK(buddy_free(&allocator, pages[i], 0) == 0);
	CHECK(buddy_free_page_count(&allocator) == 1UL << TEST_ORDER);
	CHECK(buddy_alloc(&allocator, TEST_ORDER) == test_memory);
	CHECK(buddy_free(&allocator, test_memory, TEST_ORDER) == 0);
	teardown_allocator();
}

static void test_orders_and_validation(void)
{
	struct buddy_allocator allocator;
	void *block, *page;

	setup_allocator(&allocator, 3);
	CHECK(buddy_free_page_count(&allocator) ==
	      (1UL << TEST_ORDER) - 3);
	CHECK(!buddy_contains(&allocator, (uint64)test_memory));
	CHECK(buddy_contains(&allocator,
			     (uint64)test_memory + 3 * PGSIZE));
	block = buddy_alloc(&allocator, 4);
	page = buddy_alloc(&allocator, 0);
	CHECK(block != 0 && (uint64)block % (PGSIZE << 4) == 0);
	CHECK(page != 0 && page != block);
	CHECK(buddy_free(&allocator, block, 3) < 0);
	CHECK(buddy_free(&allocator, block, 4) == 0);
	CHECK(buddy_free(&allocator, block, 4) < 0);
	CHECK(buddy_free(&allocator, page, 0) == 0);
	teardown_allocator();
}

static void test_separate_regions(void)
{
	struct buddy_allocator allocator;
	void *second_memory;
	uint8 *second_states;
	void *block;
	int status;

	setup_allocator(&allocator, 0);
	status = posix_memalign(&second_memory, TEST_SIZE, TEST_SIZE);
	CHECK(status == 0);
	second_states = malloc(TEST_SIZE / PGSIZE);
	CHECK(second_states != 0);
	CHECK(buddy_add_region(&allocator, (uint64)second_memory,
			       (uint64)second_memory + TEST_SIZE,
			       (uint64)second_memory, second_states,
			       TEST_SIZE / PGSIZE) == 0);
	CHECK(buddy_free_page_count(&allocator) == 2UL << TEST_ORDER);
	block = buddy_alloc(&allocator, TEST_ORDER);
	CHECK(block == second_memory || block == test_memory);
	CHECK(buddy_free(&allocator, block, TEST_ORDER) == 0);
	free(second_states);
	free(second_memory);
	teardown_allocator();
}

static uint64 random_state = 0x20030528;

static uint64 test_random(void)
{
	random_state = random_state * 6364136223846793005UL + 1;
	return random_state;
}

static void test_randomized_reuse(void)
{
	struct allocation {
		void *address;
		unsigned int order;
	};
	struct buddy_allocator allocator;
	struct allocation allocations[128] = { 0 };
	uint64 slot, iteration, other;

	setup_allocator(&allocator, 0);
	for (iteration = 0; iteration < 20000; iteration++) {
		slot = test_random() % 128;
		if (allocations[slot].address) {
			CHECK(buddy_free(&allocator, allocations[slot].address,
					 allocations[slot].order) == 0);
			allocations[slot].address = 0;
		} else {
			uint64 start, end;

			allocations[slot].order = test_random() % 7;
			allocations[slot].address = buddy_alloc(
				&allocator, allocations[slot].order);
			if (!allocations[slot].address)
				continue;
			start = (uint64)allocations[slot].address;
			end = start + (PGSIZE << allocations[slot].order);
			for (other = 0; other < 128; other++) {
				uint64 other_start, other_end;

				if (other == slot ||
				    !allocations[other].address)
					continue;
				other_start = (uint64)allocations[other].address;
				other_end = other_start +
					(PGSIZE << allocations[other].order);
				CHECK(start >= other_end || end <= other_start);
			}
		}
	}
	for (slot = 0; slot < 128; slot++) {
		if (allocations[slot].address)
			CHECK(buddy_free(&allocator, allocations[slot].address,
					 allocations[slot].order) == 0);
	}
	CHECK(buddy_free_page_count(&allocator) == 1UL << TEST_ORDER);
	teardown_allocator();
}

int main(void)
{
	test_split_and_coalesce();
	test_orders_and_validation();
	test_separate_regions();
	test_randomized_reuse();
	puts("BUDDY_OK");
	return 0;
}
