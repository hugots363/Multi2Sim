/*
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

#include <assert.h>

#include <arch/x86/timing/cpu.h>
#include <lib/esim/trace.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/linked-list.h>
#include <lib/util/misc.h>
#include <lib/util/string.h>

#include "atd.h"
#include "cache.h"
#include "mem-system.h"
#include "module.h"
#include "prefetcher.h"


/*
 * Public Variables
 */
long long MRU_hits = 0;


struct str_map_t cache_policy_map =
{
	4, {
		{ "LRU", cache_policy_lru },
		{ "FIFO", cache_policy_fifo },
		{ "Random", cache_policy_random },
		{ "PLRU", cache_policy_partitioned_lru }
	}
};

struct str_map_t cache_block_state_map =
{
	6, {
		{ "N", cache_block_noncoherent },
		{ "M", cache_block_modified },
		{ "O", cache_block_owned },
		{ "E", cache_block_exclusive },
		{ "S", cache_block_shared },
		{ "I", cache_block_invalid }
	}
};


/*
 * Private Functions
 */


enum cache_waylist_enum
{
	cache_waylist_head,
	cache_waylist_tail
};

static void cache_update_waylist(struct cache_set_t *set,
	struct cache_block_t *blk, enum cache_waylist_enum where)
{
	if (!blk->way_prev && !blk->way_next)
	{
		assert(set->way_head == blk && set->way_tail == blk);
		return;

	}
	else if (!blk->way_prev)
	{
		assert(set->way_head == blk && set->way_tail != blk);
		if (where == cache_waylist_head)
			return;
		set->way_head = blk->way_next;
		blk->way_next->way_prev = NULL;

	}
	else if (!blk->way_next)
	{
		assert(set->way_head != blk && set->way_tail == blk);
		if (where == cache_waylist_tail)
			return;
		set->way_tail = blk->way_prev;
		blk->way_prev->way_next = NULL;

	}
	else
	{
		assert(set->way_head != blk && set->way_tail != blk);
		blk->way_prev->way_next = blk->way_next;
		blk->way_next->way_prev = blk->way_prev;
	}

	if (where == cache_waylist_head)
	{
		blk->way_next = set->way_head;
		blk->way_prev = NULL;
		set->way_head->way_prev = blk;
		set->way_head = blk;
	}
	else
	{
		blk->way_prev = set->way_tail;
		blk->way_next = NULL;
		set->way_tail->way_next = blk;
		set->way_tail = blk;
	}
}


/*
 * Public Functions
 */


struct cache_t *cache_create(char *name, unsigned int num_sets, unsigned int block_size,
	unsigned int assoc, enum cache_policy_t policy)
{
	int total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;
	struct cache_t *cache;
	struct cache_block_t *block;
	unsigned int set, way;

	/* Initialize */
	cache = xcalloc(1, sizeof(struct cache_t));
	cache->name = xstrdup(name);
	cache->num_sets = num_sets;
	cache->block_size = block_size;
	cache->assoc = assoc;
	cache->policy = policy;

	/* Derived fields */
	assert(!(num_sets & (num_sets - 1)));
	assert(!(block_size & (block_size - 1)));
	cache->log_block_size = log_base2(block_size);
	cache->block_mask = block_size - 1;

	/* Write buffer between streams and cache */
	cache->wb.blocks = linked_list_create();

	/* Stride detector */
	cache->prefetch.stride_detector.camps = linked_list_create();

	/* Initialize array of sets */
	cache->sets = xcalloc(num_sets, sizeof(struct cache_set_t));
	for (set = 0; set < num_sets; set++)
	{
		/* Initialize array of blocks */
		cache->sets[set].blocks = xcalloc(assoc, sizeof(struct cache_block_t));
		cache->sets[set].way_head = &cache->sets[set].blocks[0];
		cache->sets[set].way_tail = &cache->sets[set].blocks[assoc - 1];
		for (way = 0; way < assoc; way++)
		{
			block = &cache->sets[set].blocks[way];
			block->way = way;
			block->way_prev = way ? &cache->sets[set].blocks[way - 1] : NULL;
			block->way_next = way < assoc - 1 ? &cache->sets[set].blocks[way + 1] : NULL;
			block->thread_id = -1; /* Invalid value */
		}
	}

	/* Allocate arrays for cache partitioning */
	cache->assigned_ways = xcalloc(total_num_threads, sizeof(int));
	cache->used_ways = xcalloc(total_num_threads, sizeof(int));

	/* Scratch memory, only for tmporal storage */
	cache->used_ways_in_set = xcalloc(total_num_threads, sizeof(int));

	/* Set to invalid value */
	for (int i = 0; i < total_num_threads; i++)
		cache->assigned_ways[i] = -1;

	/* Return it */
	return cache;
}


int cache_find_stream(struct cache_t *cache, unsigned int stream_tag){
	struct prefetcher_t *pref = cache->prefetcher;
	int stream;
	/* Look both stream tag and transcient tag */
	for (stream = 0; stream < pref->max_num_streams; stream++){
		if (pref->streams[stream].stream_transcient_tag == stream_tag || pref->streams[stream].stream_tag == stream_tag)
			return stream;
	}
	return -1;
}


int cache_detect_stride(struct cache_t *cache, int addr)
{
	struct linked_list_t *sd = cache->prefetch.stride_detector.camps;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stride_detector_camp_t *camp;
	int tag = addr & ~pref->czone_mask;
	int stride;
	const int table_max_size = 128;
	LINKED_LIST_FOR_EACH(sd){
		/* Search through the table looking for a stream tag match */
		camp = linked_list_get(sd);
		if(camp->tag == tag){
			/* Stream tag present */
			stride = addr - camp->last_addr;
			if(stride == camp->stride){
				/* There is a stride and it matches */
				linked_list_remove(sd);
				free(camp);
				cache->prefetch.stride_detector.strides_detected++; /* Statistics */
				return stride;
			}else{
				/* There isn't a stride or it doesn't match */
				if(abs(stride) >= cache->block_size){
					/* Update camps only if stride is greater than block's size */
					camp->stride = stride;
					camp->last_addr = addr;
				}
				return 0;
			}
		}
	}

	/* Strem tag not present*/
	if(linked_list_count(sd) >= table_max_size){
		/* Table is full, free oldest entry */
		linked_list_head(sd);
		camp = linked_list_get(sd);
		free(camp);
		linked_list_remove(sd);
	}
	camp = xcalloc(1, sizeof(struct stride_detector_camp_t));
	camp->last_addr = addr;
	camp->tag = tag;
	linked_list_add(sd, camp);

	return 0;
}


void cache_free(struct cache_t *cache)
{
	struct linked_list_t *sd = cache->prefetch.stride_detector.camps;
	struct stride_detector_camp_t *camp;
	int set;

	if (!cache)
		return;

	for (set = 0; set < cache->num_sets; set++)
		free(cache->sets[set].blocks);
	free(cache->sets);

	/* Destroy write buffer */
	assert(!linked_list_count(cache->wb.blocks));
	linked_list_free(cache->wb.blocks);

	/* Destroy stream detector */
	while(linked_list_count(sd)){
		linked_list_head(sd);
		camp = linked_list_get(sd);
		free(camp);
		linked_list_remove(sd);
	}
	linked_list_free(sd);

	prefetcher_free(cache->prefetcher);

	free(cache->assigned_ways);
	free(cache->used_ways);
	free(cache->used_ways_in_set);
	free(cache->name);
	free(cache);
}


/* Return {set, tag, offset} for a given address */
void cache_decode_address(struct cache_t *cache, unsigned int addr, int *set_ptr, int *tag_ptr,
	unsigned int *offset_ptr)
{
	PTR_ASSIGN(set_ptr, (addr >> cache->log_block_size) % cache->num_sets);
	PTR_ASSIGN(tag_ptr, addr & ~cache->block_mask);
	PTR_ASSIGN(offset_ptr, addr & cache->block_mask);
}


/* Look for a block in the cache. If it is found and its state is other than 0,
 * the function returns 1 and the state and way of the block are also returned.
 * The set where the address would belong is returned anyways. */
int cache_find_block(struct cache_t *cache, unsigned int addr, int *set_ptr, int *way_ptr,
	int *state_ptr)
{
	int set, tag, way;

	/* Locate block */
	tag = addr & ~cache->block_mask;
	set = (addr >> cache->log_block_size) % cache->num_sets;
	PTR_ASSIGN(set_ptr, set);
	PTR_ASSIGN(state_ptr, 0);  /* Invalid */
	for (way = 0; way < cache->assoc; way++)
		if (cache->sets[set].blocks[way].tag == tag && cache->sets[set].blocks[way].state)
			break;

	/* Block not found */
	if (way == cache->assoc)
		return 0;

	/* Block found */
	PTR_ASSIGN(way_ptr, way);
	PTR_ASSIGN(state_ptr, cache->sets[set].blocks[way].state);
	return 1;
}


/* Set the tag and state of a block.
 * If replacement policy is FIFO, update linked list in case a new
 * block is brought to cache, i.e., a new tag is set. */
void cache_set_block(struct cache_t *cache, int set, int way, int tag, int state, struct mod_client_info_t *client_info)
{
	assert(set >= 0 && set < cache->num_sets);
	assert(way >= 0 && way < cache->assoc);

	mem_trace("mem.set_block cache=\"%s\" set=%d way=%d tag=0x%x state=\"%s\"\n",
			cache->name, set, way, tag,
			str_map_value(&cache_block_state_map, state));

	if (cache->policy == cache_policy_fifo
		&& cache->sets[set].blocks[way].tag != tag)
		cache_update_waylist(&cache->sets[set],
			&cache->sets[set].blocks[way],
			cache_waylist_head);
	cache->sets[set].blocks[way].tag = tag;
	cache->sets[set].blocks[way].state = state;
	cache->sets[set].blocks[way].prefetched = 0; /* Reset prefetched state */

	if (cache->policy == cache_policy_partitioned_lru)
	{
		assert(client_info);
		assert(client_info->core >= 0 && client_info->core < x86_cpu_num_cores);
		assert(client_info->thread >= 0 && client_info->thread < x86_cpu_num_threads);
		cache_set_thread_id(cache, set, way, client_info);
	}
}


/* Set tag and state of prefetched block */
void cache_set_pref_block(struct cache_t *cache, int pref_stream, int pref_slot, int tag, int state)
{
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;

	assert(pref_stream >= 0 && pref_stream < pref->max_num_streams);
	assert(pref_slot >= 0 && pref_slot < pref->max_num_slots);

	sb = &pref->streams[pref_stream];

	mem_trace("mem.set_block in prefetch buffer of \"%s\"\
			pref_stream=%d tag=0x%x state=\"%s\"\n",
			cache->name, pref_stream, tag,
			str_map_value(&cache_block_state_map, state));

	sb->blocks[pref_slot].tag = tag;
	sb->blocks[pref_slot].state = state;
}


void cache_get_block(struct cache_t *cache, int set, int way, int *tag_ptr, int *state_ptr)
{
	assert(set >= 0 && set < cache->num_sets);
	assert(way >= 0 && way < cache->assoc);
	PTR_ASSIGN(tag_ptr, cache->sets[set].blocks[way].tag);
	PTR_ASSIGN(state_ptr, cache->sets[set].blocks[way].state);
}


struct stream_block_t * cache_get_pref_block(struct cache_t *cache,
	int pref_stream, int pref_slot)
{
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;
	assert(pref_stream >= 0 && pref_stream < pref->max_num_streams);
	assert(pref_slot >= 0 && pref_slot < pref->max_num_slots);
	sb = &pref->streams[pref_stream];
	return &sb->blocks[pref_slot];
}


void cache_get_pref_block_data(struct cache_t *cache, int pref_stream,
	int pref_slot, int *tag_ptr, int *state_ptr)
{
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;
	assert(pref_stream >= 0 && pref_stream < pref->max_num_streams);
	assert(pref_slot >= 0 && pref_slot < pref->max_num_slots);
	sb = &pref->streams[pref_stream];
	PTR_ASSIGN(tag_ptr, sb->blocks[pref_slot].tag);
	PTR_ASSIGN(state_ptr, sb->blocks[pref_slot].state);
}


/* Update LRU counters, i.e., rearrange linked list in case
 * replacement policy is LRU. */
void cache_access_block(struct cache_t *cache, int set, int way)
{
	int move_to_head;

	assert(set >= 0 && set < cache->num_sets);
	assert(way >= 0 && way < cache->assoc);

	/* A block is moved to the head of the list for LRU policy.
	 * It will also be moved if it is its first access for FIFO policy, i.e., if the
	 * state of the block was invalid. */
	move_to_head = cache->policy == cache_policy_lru || cache->policy == cache_policy_partitioned_lru ||
		(cache->policy == cache_policy_fifo && !cache->sets[set].blocks[way].state);
	if (move_to_head && cache->sets[set].blocks[way].way_prev)
		cache_update_waylist(&cache->sets[set],
			&cache->sets[set].blocks[way],
			cache_waylist_head);
}


void cache_access_stream(struct cache_t *cache, int stream)
{
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *accessed;

	/* Integrity tests */
	assert(stream >= 0 && stream < pref->max_num_streams);
	#ifndef NDEBUG
		for(accessed = pref->stream_head; accessed->stream_next; accessed = accessed->stream_next) {};
		assert(accessed == pref->stream_tail);
		for(accessed = pref->stream_tail; accessed->stream_prev; accessed = accessed->stream_prev) {};
	#endif
	assert(accessed == pref->stream_head);

	/* Return if only one stream */
	if(pref->max_num_streams < 2) return;

	accessed = &pref->streams[stream];
	/* Is tail */
	if(!accessed->stream_next && accessed->stream_prev){
		accessed->stream_prev->stream_next = NULL;
		pref->stream_tail = accessed->stream_prev;
	/* Is in the middle */
	} else if(accessed->stream_next && accessed->stream_prev) {
		accessed->stream_prev->stream_next = accessed->stream_next;
		accessed->stream_next->stream_prev = accessed->stream_prev;
	/* Is already in the head */
	} else {
		return;
	}

	/* Put first */
	accessed->stream_prev = NULL;
	accessed->stream_next = pref->stream_head;
	accessed->stream_next->stream_prev = accessed;
	pref->stream_head = accessed;
}


/* Return LRU or empty stream buffer */
int cache_select_stream(struct cache_t *cache)
{
	struct prefetcher_t *pref = cache->prefetcher;
	int s = pref->stream_tail->stream;

	/* Update LRU */
	cache_access_stream(cache, s);
	return s;
}


/* Return the way of the block to be replaced in a specific set,
 * depending on the replacement policy */
int cache_replace_block(struct cache_t *cache, int set, struct mod_client_info_t *client_info)
{
	int way = -1;
	int thread_id = client_info->core * x86_cpu_num_threads + client_info->thread;
	struct cache_block_t *block;

	/* Try to find an invalid block. Do this in the LRU order, to avoid picking the
	 * MRU while its state has not changed to valid yet. */
	assert(set >= 0 && set < cache->num_sets);

	/* LRU and FIFO replacement: return block at the
	 * tail of the linked list */
	if (cache->policy == cache_policy_lru ||
		cache->policy == cache_policy_fifo)
	{
		way = cache->sets[set].way_tail->way;
		cache_update_waylist(&cache->sets[set], cache->sets[set].way_tail,
			cache_waylist_head);
	}

	/* Partitioned LRU: if the number of allocated blocks for the thread is greater than
	 * the number of ways assigned by the partitioning algorithm, replace a block previously allocated
	 * by the thread. Otherwise replace a block from any of the competing threads. */
	else if (cache->policy == cache_policy_partitioned_lru)
	{
		for (int i = 0; i < x86_cpu_num_threads * x86_cpu_num_cores; i++)
			cache->used_ways_in_set[i] = 0;

		/* Only apply the partitioning algorithm if the lru is a valid block
		 * and if the number of assigned ways is valid */
		way = cache->sets[set].way_tail->way;
		block = &cache->sets[set].blocks[way];
		if (block->state && cache->assigned_ways[thread_id] != -1)
		{
			assert(block->thread_id != -1);

			/* Count allocated blocks for this thread */
			for (int w = 0; w < cache->assoc; w++)
			{
				block = &cache->sets[set].blocks[w];
				cache->used_ways_in_set[block->thread_id]++;
			}

			/* Too many ways allocated for this thread */
			if (cache->used_ways_in_set[thread_id] >= cache->assigned_ways[thread_id])
			{
				for (block = cache->sets[set].way_tail; block; block = block->way_prev)
				{
					if (block->thread_id == thread_id)
					{
						way = block->way;
						break;
					}
				}
			}

			/* Can allocate more ways */
			else
			{
				for (block = cache->sets[set].way_tail; block; block = block->way_prev)
				{
					if (block->thread_id != thread_id &&
							cache->used_ways_in_set[block->thread_id] >= cache->assigned_ways[block->thread_id])
					{
						way = block->way;
						break;
					}
				}
			}
		}

		if (way == -1)
			fatal("No suitable way found for replacement %s", __FUNCTION__);

		cache_update_waylist(&cache->sets[set], &cache->sets[set].blocks[way], cache_waylist_head);
	}

	/* Random replacement */
	else if (cache->policy == cache_policy_random)
		way = random() % cache->assoc;

	else
		fatal("Invalid replacement policy %s", __FUNCTION__);

	return way;
}


void cache_set_transient_tag(struct cache_t *cache, int set, int way, int tag, struct mod_client_info_t *client_info)
{
	struct cache_block_t *block;

	/* Set transient tag */
	block = &cache->sets[set].blocks[way];
	block->transient_tag = tag;

	cache_set_thread_id(cache, set, way, client_info);
}


/* Set the ID of the thread who has brought the block */
void cache_set_thread_id(struct cache_t *cache, int set, int way, struct mod_client_info_t *client_info)
{
	struct cache_block_t *block = &cache->sets[set].blocks[way];
	int thread_id = client_info->core * x86_cpu_num_threads + client_info->thread;

	/* Statistics */
	if (thread_id != block->thread_id)
	{
		cache->used_ways[thread_id]++;
		if (block->thread_id != -1)
		{
			cache->used_ways[block->thread_id]--;
			assert(cache->used_ways[block->thread_id] >= 0);
		}
	}
	assert(cache->used_ways[thread_id] >= 0);

	/* Set thread id */
	block->thread_id = thread_id;
}
