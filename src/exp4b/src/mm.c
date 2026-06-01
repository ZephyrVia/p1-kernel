#include "mm.h"
#include "sched.h"

struct free_page {
	struct free_page *next;
};

extern char kernel_end;

static struct free_page *free_list;
static unsigned long alloc_start;
static unsigned long next_unallocated_page;
static unsigned long boot_stack_reserve_start;
static unsigned long free_pages_count;

static unsigned long align_up(unsigned long value, unsigned long alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int is_valid_page(unsigned long p)
{
	return p >= alloc_start &&
	       p < HIGH_MEMORY &&
	       (p < boot_stack_reserve_start || p >= LOW_MEMORY) &&
	       (p & (PAGE_SIZE - 1)) == 0;
}

static unsigned long count_pages(unsigned long start, unsigned long end)
{
	if (end <= start) {
		return 0;
	}
	return (end - start) / PAGE_SIZE;
}

static unsigned long count_available_pages(unsigned long start)
{
	unsigned long pages = count_pages(start, HIGH_MEMORY);
	unsigned long reserved_start;
	unsigned long reserved_end;

	if (pages == 0) {
		return 0;
	}

	reserved_start = start > boot_stack_reserve_start ? start : boot_stack_reserve_start;
	reserved_end = LOW_MEMORY < HIGH_MEMORY ? LOW_MEMORY : HIGH_MEMORY;
	pages -= count_pages(reserved_start, reserved_end);

	return pages;
}

static unsigned long skip_reserved_pages(unsigned long p)
{
	if (p >= boot_stack_reserve_start && p < LOW_MEMORY) {
		return LOW_MEMORY;
	}
	return p;
}

void mm_init(void)
{
	unsigned long start = align_up((unsigned long)&kernel_end, PAGE_SIZE);

	free_list = 0;
	alloc_start = start;
	next_unallocated_page = start;
	boot_stack_reserve_start = LOW_MEMORY - 4 * PAGE_SIZE;
	free_pages_count = count_available_pages(start);
}

unsigned long get_free_page()
{
	struct free_page *page;

	preempt_disable();
	page = free_list;
	if (page) {
		free_list = page->next;
		free_pages_count--;
	} else {
		unsigned long p = skip_reserved_pages(next_unallocated_page);
		if (p < HIGH_MEMORY) {
			page = (struct free_page *)p;
			next_unallocated_page = p + PAGE_SIZE;
			free_pages_count--;
		}
	}
	preempt_enable();

	return (unsigned long)page;
}

void free_page(unsigned long p){
	struct free_page *page;

	if (!is_valid_page(p)) {
		return;
	}

	preempt_disable();
	page = (struct free_page *)p;
	page->next = free_list;
	free_list = page;
	free_pages_count++;
	preempt_enable();
}

unsigned long get_alloc_start(void)
{
	return alloc_start;
}

unsigned long get_free_pages_count(void)
{
	return free_pages_count;
}
