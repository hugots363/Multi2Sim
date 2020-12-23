/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MEM_SYSTEM_CACHE_H
#define MEM_SYSTEM_CACHE_H

#include "cache-partitioning.h"

#include <lib/util/interval-kind.h>

#define block_invalid_tag -1

extern struct str_map_t cache_policy_map;
extern struct str_map_t cache_block_state_map;


struct mod_client_info_t;


enum cache_policy_t
{
	cache_policy_invalid = 0,
	cache_policy_lru,
	cache_policy_fifo,
	cache_policy_random,
	cache_policy_partitioned_lru
};

enum cache_block_state_t
{
	cache_block_invalid = 0,
	cache_block_noncoherent,
	cache_block_modified,
	cache_block_owned,
	cache_block_exclusive,
	cache_block_shared
};

struct write_buffer_block_t
{
	int tag;
	long long stack_id;
	enum cache_block_state_t state;
	struct mod_stack_t *wait_queue;
};

struct cache_write_buffer
{
	struct linked_list_t *blocks;
};

struct cache_block_t
{
	struct cache_block_t *way_next;
	struct cache_block_t *way_prev;

	int tag;
	int transient_tag;
	int way;
	int prefetched;
	int thread_id; /* Thread who has put the block */
	//Hugo guessing access type
	int access_type;	

	enum cache_block_state_t state;
};

struct cache_set_t
{
	struct cache_block_t *way_head;
	struct cache_block_t *way_tail;
	struct cache_block_t *blocks;
};

/* Prefetching */
struct stream_block_t
{
	int slot;
	int tag;
	int transient_tag;
	enum cache_block_state_t state;
};

struct stride_detector_camp_t
{
	int tag;
	int last_addr;
	int stride;
};

struct stream_buffer_t
{
	int stream;
	int stream_tag; /* Tag of stream */
	int stream_transcient_tag; /* Tag of stream being brougth */
	struct stream_buffer_t *stream_next;
	struct stream_buffer_t *stream_prev;
	struct stream_block_t *blocks;

	int pending_prefetches; /* Remaining prefetches of a prefetch group */
	long long time; /* When was last prefetch asigned to this stream. For debug. */
	int count;
	int head;
	int tail;
	int stride;
	int next_address;
	int dead : 1;
};

struct cache_t
{
	char *name;

	unsigned int num_sets;
	unsigned int block_size;
	unsigned int assoc;
	enum cache_policy_t policy;

	/* Cache partitioning */
	struct
	{
		enum cache_partitioning_policy_t policy;
		long long interval;
		enum interval_kind_t interval_kind;
	} partitioning;

	/* Thread pairing */
	struct
	{
		enum thread_pairing_policy_t policy;
		long long interval; /* Number of cache partitionings before changing thread pairs */
	} pairing;

	struct cache_set_t *sets;
	unsigned int block_mask;
	int log_block_size;

	struct {
		struct
		{
			struct linked_list_t *camps;
			long long strides_detected;
		} stride_detector;
	} prefetch;

	struct prefetcher_t *prefetcher;

	struct cache_write_buffer wb;

	int *assigned_ways; /* Number of ways assigned per thread, to enable cache partitioning */
	int *used_ways; /* Number of ways used per thread */

	int *used_ways_in_set; /* Tmp storage for computations */
	//Added by Hugo
	int mov_cabezal; /*Penality by moving the header throw blocks in cycles */
	int RTM;
};

struct cache_t *cache_create(char *name, unsigned int num_sets, unsigned int block_size, unsigned int assoc, enum cache_policy_t policy);
void cache_free(struct cache_t *cache);

void cache_decode_address(struct cache_t *cache, unsigned int addr,
	int *set_ptr, int *tag_ptr, unsigned int *offset_ptr);
int cache_find_block(struct cache_t *cache, unsigned int addr, int *set_ptr, int *pway,
	int *state_ptr);
void cache_set_block(struct cache_t *cache, int set, int way, int tag, int state, struct mod_client_info_t *client_info);
void cache_get_block(struct cache_t *cache, int set, int way, int *tag_ptr, int *state_ptr);

void cache_access_block(struct cache_t *cache, int set, int way);
int cache_replace_block(struct cache_t *cache, int set, struct mod_client_info_t *client_info);
void cache_set_transient_tag(struct cache_t *cache, int set, int way, int tag, struct mod_client_info_t *client_info);
void cache_set_thread_id(struct cache_t *cache, int set, int way, struct mod_client_info_t *client_info);

/* Prefetching */
int cache_find_stream(struct cache_t *cache, unsigned int stream_tag);
void cache_set_pref_block(struct cache_t *cache, int pref_stream, int pref_slot, int tag, int state);
struct stream_block_t * cache_get_pref_block(struct cache_t *cache, int pref_stream, int pref_slot);
void cache_get_pref_block_data(struct cache_t *cache, int pref_stream, int pref_slot, int *tag_ptr, int *state_ptr);
int cache_select_stream(struct cache_t *cache);
void cache_access_stream(struct cache_t *cache, int stream);
int cache_detect_stride(struct cache_t *cache, int addr);

#endif

