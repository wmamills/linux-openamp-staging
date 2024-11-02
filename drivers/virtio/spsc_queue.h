/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Hardened and lockless Single Producer Single Consumer Queue implemented
 * over shared-memory.
 *
 * The queue implementation does not look at packet contents, it's up to upper
 * layers to make sure data is produced and parsed safely. All data is copied
 * in/out from/to local private buffers so the peer cannot mess with them while
 * upper layers parse.
 *
 * The queue is split into a private and a shared part.
 * The private part contains cached and sanitized versions of the indexes that
 * indicate our position in the ring-buffer. Peers can corrupt the shared area
 * but have no access to the private area. So whenever we copy from the shared
 * area into the private one, we need to sanitize indexes and make sure they
 * are within bounds.
 *
 * A malicious peer can send corrupt data, it can stop receiving or flood the
 * queue causing a sort of denial of service but it can NOT cause our side
 * to copy data in or out of buffers outside of the shared memory area.
 *
 * This implementation expects the SHM area to be cache-coherent or uncached.
 * The shared area can be mapped in different ways and our peer may be anything
 * from another thread on our same OS to an FPGA implementation on a PCI card.
 * So local CPU cache-lines sizes, or spin-locks and things that work on a
 * single CPU cluster are not used. Instead the implementation sticks to atomic
 * load/stores of 32b values and to using memory-barriers to guarantee ordering.
 */

#ifndef SPSC_QUEUE_H__
#define SPSC_QUEUE_H__

#include <linux/nospec.h>

#define assert(x) BUG_ON(!(x))
#define read_atomic(p) ((p)[0])
#define write_atomic(p, v) (p)[0] = v

#define SPSC_QUEUE_MAX_PACKET_SIZE 64
/*
 * This cache-line size is used to align fields in the hope of
 * avoiding cache-line ping-pong:ing. Since the queue layout is
 * used across heterogeneous CPU clusters and across FPGA/HW implementations,
 * a fixed size must be used, i.e not the local CPU's cache-line size.
 */
#define SPSC_QUEUE_CACHE_LINE_SIZE 64

struct spsc_queue_shared {
	u32 head __aligned(SPSC_QUEUE_CACHE_LINE_SIZE);
	u32 tail __aligned(SPSC_QUEUE_CACHE_LINE_SIZE);
	u32 packets[][SPSC_QUEUE_MAX_PACKET_SIZE / 4]
		__aligned(SPSC_QUEUE_CACHE_LINE_SIZE);
};

struct spsc_queue {
	u32 cached_tail;
	u32 cached_head;
	struct spsc_queue_shared *shm;
	const char *name;
	unsigned int capacity;
};

/* Atomically load and sanitize an index from the SHM area.  */
static inline u32 spsc_atomic_load(struct spsc_queue *q, u32 *ptr)
{
	u32 val;

	val = read_atomic(ptr);
	/* Make sure packet reads are done after reading the index.  */
	smp_rmb();

	/* Bounds check that index is within queue size.  */
	if (val >= q->capacity)
		val = array_index_nospec(val, q->capacity);

	return val;
}

static inline void spsc_atomic_store(struct spsc_queue *q, u32 *ptr, u32 v)
{
	/* Make sure packet-data gets written before updating the index.  */
	smp_wmb();
	write_atomic(ptr, v);
}

/* Returns the capacity of a queue given a specific mapsize. */
static inline unsigned int spsc_capacity(size_t mapsize)
{
	unsigned int capacity;
	struct spsc_queue *q = NULL;

	if (mapsize < sizeof(*q->shm))
		return 0;

	/* Start with the size of the shared area. */
	mapsize -= sizeof(*q->shm);
	capacity = mapsize / sizeof(q->shm->packets[0]);

	/* Capacities of less than 2 are invalid. */
	if (capacity < 2)
		return 0;

	return capacity;
}

static inline size_t spsc_mapsize(unsigned int capacity)
{
	struct spsc_queue *q = NULL;
	size_t mapsize;

	assert(capacity >= 2);

	mapsize = sizeof(*q->shm);
	mapsize += sizeof(q->shm->packets[0]) * capacity;

	return mapsize;
}

static inline void spsc_init(struct spsc_queue *q, const char *name,
			     size_t capacity, void __iomem *mem)
{
	assert(mem);

	/* Initialize private queue area to all zeores */
	memset(q, 0, sizeof(*q));

	q->shm = mem;
	q->name = name;
	q->capacity = capacity;

	/* In case we're opening a pre-existing queue, pick up where we left off. */
	q->cached_tail = spsc_atomic_load(q, &q->shm->tail);
	q->cached_head = spsc_atomic_load(q, &q->shm->head);
}

static inline bool spsc_queue_is_full(struct spsc_queue *q)
{
	u32 next_head;
	u32 head;

	head = spsc_atomic_load(q, &q->shm->head);

	next_head = head + 1;
	if (next_head == q->capacity)
		next_head = 0;

	if (next_head == q->cached_tail) {
		q->cached_tail = spsc_atomic_load(q, &q->shm->tail);
		if (next_head == q->cached_tail)
			return true;
	}
	return false;
}

static inline bool spsc_send(struct spsc_queue *q, void *buf, size_t size)
{
	u32 next_head;
	u32 head;

	head = spsc_atomic_load(q, &q->shm->head);

	assert(size <= sizeof(q->shm->packets[0]));
	assert(size > 0);

	next_head = head + 1;
	if (next_head == q->capacity)
		next_head = 0;

	/* Is the queue full?  */
	if (next_head == q->cached_tail) {
		q->cached_tail = spsc_atomic_load(q, &q->shm->tail);
		if (next_head == q->cached_tail)
			return false;
	}

	memcpy(q->shm->packets[head], buf, size);

	/* Make packet visible before head update. */
	smp_wmb();
	write_atomic(&q->shm->head, next_head);
	return true;
}

static inline bool spsc_recv(struct spsc_queue *q, void *buf, size_t size)
{
	u32 tail;

	assert(size <= sizeof(q->shm->packets[0]));
	assert(size > 0);

	tail = spsc_atomic_load(q, &q->shm->tail);

	/* Is the queue empty?  */
	if (tail == q->cached_head) {
		q->cached_head = spsc_atomic_load(q, &q->shm->head);
		if (tail == q->cached_head)
			return false;
	}

	memcpy(buf, q->shm->packets[tail], size);

	/* Update the read pointer.  */
	tail++;
	if (tail == q->capacity)
		tail = 0;

	/* Copy all of the packet before tail update. */
	smp_wmb();
	write_atomic(&q->shm->tail, tail);
	return true;
}
#endif
