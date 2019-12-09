#ifndef MEM_SYSTEM_STREAM_PREFETCHER_H
#define MEM_SYSTEM_STREAM_PREFETCHER_H


/*
 * This file implements a czone-based stride detection
 * prefetcher with stream allocation.
 */


enum stream_request_kind_t
{
	stream_request_invalid = 0,
	stream_request_single, /* Prefetch enqueued after a hit in stream buffer */
	stream_request_grouped /* One of the prefetches enqueued for filling a stream after detection of stride */
};
extern struct str_map_t stream_request_kind_map;

/* TODO: En un futur s'usarà açò... */
// struct stream_prefetcher_t
// {
// 	enum prefetcher_type_t type; /* Type of prefetcher */
// 	unsigned int num_streams; 	/* Number of streams for prefetch */
// 	unsigned int num_slots; 	/* Number of blocks per stream */
// 	unsigned int stream_mask; 	/* For obtaining stream_tag */
//
// 	struct stream_buffer_t *streams;
// 	struct stream_buffer_t *stream_head;
// 	struct stream_buffer_t *stream_tail;
//
// 	enum adapt_pref_policy_t adapt_policy; /* Adaptative policy used */
// 	long long adapt_interval; /* Interval at wich the adaptative policy is evaluated and aplied */
// 	enum interval_kind_t adapt_interval_kind; /* Tells if the interval is in cycles or in instructions */
//
// 	struct
// 	{
// 		struct linked_list_t *camps;
// 		long long strides_detected;
// 		long long last_strides_detected;
// 	} stride_detector;
// } prefetch;

#endif
