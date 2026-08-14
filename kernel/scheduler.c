#include <cpu.h>
#include <debug.h>
#include <ktime.h>
#include <mem_layout.h>
#include <rbtree.h>
#include <riscv.h>
#include <sbi.h>
#include <scheduler.h>
#include <timer.h>

#define NICE_0_LOAD 1024U
#define SCHED_TARGET_LATENCY_NS 48000000ULL
#define SCHED_MIN_GRANULARITY_NS 6000000ULL
#define SCHED_WAKEUP_GRANULARITY_NS 4000000ULL
#define SCHED_MIN_SLICE_NS 1000000ULL

static const uint32 nice_weights[40] = {
	88761, 71755, 56483, 46273, 36291,
	29154, 23254, 18705, 14949, 11916,
	9548, 7620, 6100, 4904, 3906,
	3121, 2501, 1991, 1586, 1277,
	1024, 820, 655, 526, 423,
	335, 272, 215, 172, 137,
	110, 87, 70, 56, 45,
	36, 29, 23, 18, 15,
};

extern void switchto(context_t c, context_t p);

static struct cpu boot_cpu;
static cpu_t boot_cpu_table[] = { &boot_cpu };
cpu_t *cpus = boot_cpu_table;

static struct {
	struct spinlock lock;
	struct rb_root timeline;
	uint64 total_weight;
	uint64 min_vruntime;
	uint32 count;
} runqueue;

int cpuid(void)
{
	return (int)tp_r();
}

cpu_t cur_cpu(void)
{
	return cpus[cpuid()];
}

thread_t cur_thread(void)
{
	return cur_cpu()->current;
}

process_t cur_proc(void)
{
	thread_t current = cur_thread();

	return current ? current->home : 0;
}

void scheduler_init(void)
{
	spinlock_init(&runqueue.lock, "CFS runqueue");
	rb_root_init(&runqueue.timeline);
	runqueue.total_weight = 0;
	runqueue.min_vruntime = 0;
	runqueue.count = 0;
}

static uint64 add_saturate(uint64 left, uint64 right)
{
	return left > ~(uint64)0 - right ? ~(uint64)0 : left + right;
}

static uint64 scale_runtime(uint64 runtime, uint32 weight)
{
	return runtime / weight * NICE_0_LOAD +
	       runtime % weight * NICE_0_LOAD / weight;
}

static uint64 entity_vruntime_now(thread_t thread, uint64 now)
{
	uint64 runtime = thread->sched.vruntime;

	if (thread->state == THREAD_RUNNING && thread->sched.exec_start &&
	    now > thread->sched.exec_start)
		runtime = add_saturate(runtime,
			scale_runtime(now - thread->sched.exec_start,
				      thread->sched.weight));
	return runtime;
}

static void account_runtime(thread_t thread, uint64 now)
{
	uint64 delta;

	if (!thread->sched.exec_start || now <= thread->sched.exec_start) {
		thread->sched.exec_start = now;
		return;
	}
	delta = now - thread->sched.exec_start;
	thread->sched.sum_exec_runtime =
		add_saturate(thread->sched.sum_exec_runtime, delta);
	thread->sched.vruntime = add_saturate(thread->sched.vruntime,
		scale_runtime(delta, thread->sched.weight));
	thread->sched.exec_start = now;
}

static int entity_before(thread_t left, thread_t right)
{
	if (left->sched.vruntime != right->sched.vruntime)
		return left->sched.vruntime < right->sched.vruntime;
	if (left->tid != right->tid)
		return left->tid < right->tid;
	return left < right;
}

static void enqueue_entity_locked(thread_t thread)
{
	struct rb_node **link = &runqueue.timeline.node;
	struct rb_node *parent = 0;

	while (*link) {
		thread_t queued;

		parent = *link;
		queued = rb_entry(parent, struct thread, sched.run_node);
		if (entity_before(thread, queued))
			link = &parent->left;
		else
			link = &parent->right;
	}
	rb_link_node(&thread->sched.run_node, parent, link);
	rb_insert_color(&thread->sched.run_node, &runqueue.timeline);
	thread->sched.on_runqueue = 1;
	runqueue.count++;
	runqueue.total_weight += thread->sched.weight;
}

static void dequeue_entity_locked(thread_t thread)
{
	if (!thread->sched.on_runqueue || !runqueue.count ||
	    runqueue.total_weight < thread->sched.weight)
		PANIC("invalid CFS dequeue");
	rb_erase(&thread->sched.run_node, &runqueue.timeline);
	thread->sched.on_runqueue = 0;
	runqueue.count--;
	runqueue.total_weight -= thread->sched.weight;
}

static void place_entity_locked(thread_t thread)
{
	uint64 floor = runqueue.min_vruntime;

	if (floor > SCHED_WAKEUP_GRANULARITY_NS)
		floor -= SCHED_WAKEUP_GRANULARITY_NS;
	else
		floor = 0;
	if (!thread->sched.initialized) {
		thread->sched.vruntime = runqueue.min_vruntime;
		thread->sched.initialized = 1;
	} else if (thread->sched.vruntime < floor) {
		thread->sched.vruntime = floor;
	}
}

static void update_min_vruntime_locked(thread_t selected, uint64 now)
{
	struct rb_node *node = rb_first(&runqueue.timeline);
	uint64 minimum = ~(uint64)0;
	int logical;

	if (selected)
		minimum = selected->sched.vruntime;
	if (node) {
		thread_t queued = rb_entry(node, struct thread,
					    sched.run_node);

		if (queued->sched.vruntime < minimum)
			minimum = queued->sched.vruntime;
	}
	for (logical = 0; logical < cpu_count(); logical++) {
		thread_t current = cpus[logical]->current;
		thread_t pending = cpus[logical]->selected;
		uint64 vruntime;

		if (current && current != selected &&
		    current->state == THREAD_RUNNING) {
			vruntime = entity_vruntime_now(current, now);
			if (vruntime < minimum)
				minimum = vruntime;
		}
		if (pending && pending != selected &&
		    pending->sched.vruntime < minimum)
			minimum = pending->sched.vruntime;
	}
	if (minimum != ~(uint64)0 && minimum > runqueue.min_vruntime)
		runqueue.min_vruntime = minimum;
}

static int claim_idle_cpu_locked(void)
{
	cpu_t current = cur_cpu();
	int logical;

	if (current->idle) {
		current->idle = 0;
		return -1;
	}
	for (logical = 0; logical < cpu_count(); logical++) {
		if (cpus[logical] == current || !cpus[logical]->online ||
		    !cpus[logical]->idle)
			continue;
		cpus[logical]->idle = 0;
		return logical;
	}
	return -1;
}

static int select_preempt_cpu_locked(thread_t waking, uint64 now)
{
	uint64 wake_vruntime = waking->sched.vruntime;
	uint64 greatest = wake_vruntime;
	int logical, target = -1;

	for (logical = 0; logical < cpu_count(); logical++) {
		thread_t current = cpus[logical]->current;
		uint64 current_vruntime;

		if (!cpus[logical]->online || !current ||
		    current->state != THREAD_RUNNING)
			continue;
		current_vruntime = entity_vruntime_now(current, now);
		if (current_vruntime <=
		    add_saturate(wake_vruntime,
				 SCHED_WAKEUP_GRANULARITY_NS) ||
		    (target >= 0 && current_vruntime <= greatest))
			continue;
		greatest = current_vruntime;
		target = logical;
	}
	if (target >= 0)
		__atomic_store_n(&cpus[target]->need_resched, 1,
				 __ATOMIC_RELEASE);
	return target;
}

static void wake_cpu(int target)
{
	if (target < 0 || target == cpuid())
		return;
	if (sbi_send_ipi(cpu_hart_id(target)))
		PANIC("SBI send IPI failed");
}

static void scheduler_enqueue(thread_t thread, int wake_idle)
{
	thread_state_t previous;
	uint64 now = ktime_get_ns();
	int target = -1;

	if (!spinlock_holding(&thread->lock))
		PANIC("runnable lock");
	previous = thread->state;

	spinlock_acquire(&runqueue.lock);
	if (thread->sched.on_runqueue ||
	    (previous != THREAD_ALLOCATED && previous != THREAD_RUNNING &&
	     previous != THREAD_SLEEPING))
		PANIC("invalid runnable thread");
	if (previous == THREAD_RUNNING)
		account_runtime(thread, now);
	update_min_vruntime_locked(previous == THREAD_RUNNING ? thread : 0,
				   now);
	if (previous != THREAD_RUNNING)
		place_entity_locked(thread);
	thread->state = THREAD_RUNNABLE;
	enqueue_entity_locked(thread);
	if (wake_idle)
		target = claim_idle_cpu_locked();
	if (wake_idle && target < 0)
		target = select_preempt_cpu_locked(thread, now);
	spinlock_release(&runqueue.lock);
	wake_cpu(target);
}

void scheduler_make_runnable(thread_t thread)
{
	scheduler_enqueue(thread, 1);
}

static uint64 calculate_slice_locked(thread_t selected)
{
	uint64 total_weight = runqueue.total_weight + selected->sched.weight;
	uint32 runnable = runqueue.count + 1;
	uint64 period, slice;
	int logical;

	for (logical = 0; logical < cpu_count(); logical++) {
		thread_t current = cpus[logical]->current;
		thread_t pending = cpus[logical]->selected;

		if (current && current != selected &&
		    current->state == THREAD_RUNNING) {
			runnable++;
			total_weight += current->sched.weight;
		}
		if (pending && pending != selected) {
			runnable++;
			total_weight += pending->sched.weight;
		}
	}
	period = runnable * SCHED_MIN_GRANULARITY_NS;
	if (period < SCHED_TARGET_LATENCY_NS)
		period = SCHED_TARGET_LATENCY_NS;
	slice = period / total_weight * selected->sched.weight +
		period % total_weight * selected->sched.weight / total_weight;
	return slice < SCHED_MIN_SLICE_NS ? SCHED_MIN_SLICE_NS : slice;
}

void scheduler_inherit(thread_t child, thread_t parent)
{
	uint64 now;

	if (!child || !parent || !spinlock_holding(&child->lock) ||
	    child->state != THREAD_ALLOCATED || child->sched.on_runqueue)
		PANIC("invalid scheduler inheritance");
	now = ktime_get_ns();
	spinlock_acquire(&runqueue.lock);
	child->sched.nice = parent->sched.nice;
	child->sched.weight = parent->sched.weight;
	child->sched.vruntime = entity_vruntime_now(parent, now);
	child->sched.initialized = parent->sched.initialized;
	spinlock_release(&runqueue.lock);
}

int scheduler_set_nice(thread_t thread, int nice)
{
	int logical, queued, running, target = -1;

	if (!thread || nice < -20 || nice > 19)
		return -1;
	spinlock_acquire(&thread->lock);
	running = thread->state == THREAD_RUNNING;
	spinlock_acquire(&runqueue.lock);
	if (running)
		account_runtime(thread, ktime_get_ns());
	queued = thread->sched.on_runqueue;
	if (queued)
		dequeue_entity_locked(thread);
	thread->sched.nice = nice;
	thread->sched.weight = nice_weights[nice + 20];
	if (queued)
		enqueue_entity_locked(thread);
	if (running) {
		for (logical = 0; logical < cpu_count(); logical++) {
			if (cpus[logical]->current != thread)
				continue;
			__atomic_store_n(&cpus[logical]->need_resched, 1,
					 __ATOMIC_RELEASE);
			target = logical;
			break;
		}
	}
	spinlock_release(&runqueue.lock);
	spinlock_release(&thread->lock);
	wake_cpu(target);
	return 0;
}

int scheduler_get_nice(thread_t thread)
{
	int nice;

	if (!thread)
		return 0;
	spinlock_acquire(&thread->lock);
	nice = thread->sched.nice;
	spinlock_release(&thread->lock);
	return nice;
}

static thread_t scheduler_next(cpu_t cpu)
{
	struct rb_node *node;
	thread_t thread = 0;
	uint64 now = ktime_get_ns();

	if (intr_status())
		PANIC("scheduler interrupts enabled");
	spinlock_acquire(&runqueue.lock);
	if (cpu->current || cpu->selected)
		PANIC("busy scheduler CPU");
	cpu->idle = 0;
	node = rb_first(&runqueue.timeline);
	if (node) {
		thread = rb_entry(node, struct thread, sched.run_node);
		dequeue_entity_locked(thread);
		update_min_vruntime_locked(thread, now);
		thread->sched.slice_ns = calculate_slice_locked(thread);
		cpu->selected = thread;
	} else if (runqueue.count || runqueue.total_weight) {
		PANIC("CFS runqueue count");
	} else {
		cpu->idle = 1;
	}
	spinlock_release(&runqueue.lock);
	return thread;
}

void scheduler(void)
{
	volatile cpu_t cpu = cur_cpu();
	thread_t next;

	cpu->current = 0;
	cpu->selected = 0;
	cpu->idle = 0;
	for (;;) {
		/*
		 * Keep global interrupts disabled through WFI. An IPI arriving
		 * after the empty check remains pending and makes WFI return.
		 */
		intr_off();
		next = scheduler_next(cpu);
		if (!next) {
			timer_set_idle();
			wait_for_interrupt();
			intr_on();
			continue;
		}
		timer_set_active();
		intr_on();

		spinlock_acquire(&next->lock);
		spinlock_acquire(&runqueue.lock);
		if (next->state != THREAD_RUNNABLE ||
		    next->sched.on_runqueue ||
		    (!next->home && !next->kernel_thread) ||
		    cpu->selected != next || cpu->current)
			PANIC("invalid scheduled thread");
		next->state = THREAD_RUNNING;
		next->sched.exec_start = ktime_get_ns();
		cpu->selected = 0;
		__atomic_store_n(&cpu->need_resched, 0, __ATOMIC_RELEASE);
		cpu->current = next;
		spinlock_release(&runqueue.lock);
		if (next->home)
			next->home->tinfo->addr = TRAPFRAME(next->id_p);
		switchto(&cpu->context, &next->context);
		spinlock_acquire(&runqueue.lock);
		if (cpu->current != next || cpu->selected)
			PANIC("invalid current thread");
		cpu->current = 0;
		spinlock_release(&runqueue.lock);
		if (next->state == THREAD_EXITED && next->kernel_thread)
			kernel_thread_reap(next);
		spinlock_release(&next->lock);
	}
}

void sched(void)
{
	cpu_t cpu = cur_cpu();
	thread_t current = cpu->current;
	uint8 before_lock;

	if (!current || !spinlock_holding(&current->lock))
		PANIC("sched holding");
	if (intr_status())
		PANIC("sched intr open");
	if (cpu->lock_nest_depth != 1) {
		printf("%d->", cpu->lock_nest_depth);
		PANIC("sched lock_nest_depth");
	}
	if (current->state == THREAD_RUNNING)
		PANIC("sched running");

	before_lock = cpu->before_lock;
	switchto(&current->context, &cpu->context);
	/* A resumed thread may have migrated to another CPU. */
	cur_cpu()->before_lock = before_lock;
}

void yield(void)
{
	thread_t current = cur_thread();

	spinlock_acquire(&current->lock);
	scheduler_enqueue(current, 0);
	sched();
	spinlock_release(&current->lock);
}

void scheduler_block_current(void)
{
	thread_t current = cur_thread();

	if (!current || !spinlock_holding(&current->lock) ||
	    current->state != THREAD_RUNNING)
		PANIC("block non-running thread");
	spinlock_acquire(&runqueue.lock);
	account_runtime(current, ktime_get_ns());
	update_min_vruntime_locked(current, current->sched.exec_start);
	current->state = THREAD_SLEEPING;
	spinlock_release(&runqueue.lock);
}

void scheduler_exit_locked(void)
{
	thread_t current = cur_thread();

	if (!current || !spinlock_holding(&current->lock) ||
	    current->state != THREAD_RUNNING)
		PANIC("exit non-running thread");
	spinlock_acquire(&runqueue.lock);
	account_runtime(current, ktime_get_ns());
	update_min_vruntime_locked(current, current->sched.exec_start);
	current->state = THREAD_EXITED;
	spinlock_release(&runqueue.lock);
	sched();
	PANIC("scheduled exited thread");
}

void scheduler_exit(void)
{
	thread_t current = cur_thread();

	if (!current)
		PANIC("exit without thread");
	spinlock_acquire(&current->lock);
	scheduler_exit_locked();
}

void scheduler_request_resched(void)
{
	cpu_t cpu = cur_cpu();

	if (cpu->current)
		__atomic_store_n(&cpu->need_resched, 1, __ATOMIC_RELEASE);
}

void scheduler_tick(void)
{
	cpu_t cpu = cur_cpu();
	thread_t current = cpu->current;
	struct rb_node *node;
	uint64 now, elapsed, current_vruntime;
	int resched = 0;

	if (!current || current->state != THREAD_RUNNING)
		return;
	now = ktime_get_ns();
	elapsed = now > current->sched.exec_start ?
		now - current->sched.exec_start : 0;
	spinlock_acquire(&runqueue.lock);
	node = rb_first(&runqueue.timeline);
	if (node) {
		thread_t leftmost;

		leftmost = rb_entry(node, struct thread, sched.run_node);
		current_vruntime = entity_vruntime_now(current, now);
		if (elapsed >= current->sched.slice_ns ||
		    current_vruntime > add_saturate(
			leftmost->sched.vruntime,
			SCHED_WAKEUP_GRANULARITY_NS))
			resched = 1;
	}
	spinlock_release(&runqueue.lock);
	if (resched)
		__atomic_store_n(&cpu->need_resched, 1, __ATOMIC_RELEASE);
}

int scheduler_should_resched(void)
{
	cpu_t cpu = cur_cpu();

	if (!cpu->current || cpu->current->state != THREAD_RUNNING)
		return 0;
	return __atomic_exchange_n(&cpu->need_resched, 0,
				   __ATOMIC_ACQ_REL);
}
