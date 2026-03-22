#ifndef GOC_CONFIG_H
#define GOC_CONFIG_H

/*
 * Number of cancelled/dead waiter nodes tolerated on a channel list before
 * triggering a compaction sweep.
 */
#define GOC_DEAD_COUNT_THRESHOLD 8

/*
 * Maximum number of select arms (`goc_alts`) that use stack scratch storage
 * before switching to heap allocation for the temporary channel-pointer array.
 */
#define GOC_ALTS_STACK_THRESHOLD 8

/*
 * Conservative throughput-oriented memory factor used by the default
 * live-fiber admission formula:
 *   floor(factor * (available_hardware_memory / fiber_stack_size))
 *
 * A value below 1.0 intentionally reserves headroom for GC metadata,
 * channels/queues, allocator overhead, and other process memory.
 */
#define GOC_DEFAULT_LIVE_FIBER_MEMORY_FACTOR 0.6

#endif /* GOC_CONFIG_H */
