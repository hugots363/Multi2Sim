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

#include <assert.h>

#include <lib/esim/esim.h> /* esim_cycle() */
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/bloom.h>
#include <lib/util/misc.h>
#include <lib/util/string.h>

#include "directory.h"
#include "mem-system.h"
#include "mmu.h"
#include "prefetcher.h"
#include "cache.h"
#include "mod-stack.h"

int valid_prefetch_addr(struct mod_t *mod, unsigned int pref_addr, int stride);
int can_prefetch(struct mod_stack_t *stack);
void prefetch_to_stream_buffer(struct mod_t *mod, struct mod_client_info_t *client_info, int stream, int num_prefetches);
void stream_buffer_allocate_stream(struct mod_t *mod, struct mod_client_info_t *client_info, unsigned int miss_addr, int stride);
void stream_buffer_prefetch_in_stream(struct mod_t *mod, struct mod_client_info_t *client_info, int stream, int slot);
int get_it_index_tag(struct prefetcher_t *pref, struct mod_stack_t *stack, int *it_index, unsigned *tag);
int prefetcher_ghb_cs_find_stride(struct prefetcher_t *pref, int it_index);

/* If a new type is added don't forget to update functions
 * prefetcher_uses_XXX */
struct str_map_t prefetcher_type_map =
{
	5, {
		{ "PC_CS", prefetcher_type_pc_cs },
		{ "PC_DC", prefetcher_type_pc_dc },
		{ "PC_CS_SB", prefetcher_type_pc_cs_sb }, /* PC indexed constant stride prefetched to stream buffers */
		{ "CZ_CS_SB", prefetcher_type_cz_cs_sb }, /* CZONE indexed constant stride prefetched to stream buffers */
		{ "CZ_CS", prefetcher_type_cz_cs }, /* CZONE indexed constant stride prefetched to cache */
	}
};

struct str_map_t adapt_pref_policy_map =
{
	5, {
		{ "None", adapt_pref_policy_none },
		{ "ADP", adapt_pref_policy_adp },
		{ "FDP", adapt_pref_policy_fdp },
		{ "HPAC", adapt_pref_policy_hpac },
		{ "MBP", adapt_pref_policy_mbp },
	}
};

struct str_map_t interval_kind_map =
{
	3, {
		{ "cycles", interval_kind_cycles },
		{ "instructions", interval_kind_instructions },
		{ "evictions", interval_kind_evictions },
	}
};


struct prefetcher_t *prefetcher_create(int prefetcher_ghb_size, int prefetcher_it_size,
				       int prefetcher_lookup_depth, enum prefetcher_type_t type, int aggr)
{
	struct prefetcher_t *pref;

	/* Initialize */
	/* The global history buffer and index table cannot be 0
	 * if the prefetcher object is created. */
	assert(prefetcher_ghb_size >= 1 && prefetcher_it_size >= 1);
	assert(aggr > 0);
	pref = xcalloc(1, sizeof(struct prefetcher_t));
	pref->enabled = 1;
	pref->ghb_size = prefetcher_ghb_size;
	pref->it_size = prefetcher_it_size;
	pref->lookup_depth = prefetcher_lookup_depth;
	pref->type = type;
	pref->ghb = xcalloc(prefetcher_ghb_size, sizeof(struct prefetcher_ghb_t));
	pref->index_table = xcalloc(prefetcher_it_size, sizeof(struct prefetcher_it_t));
	pref->ghb_head = -1;
	pref->aggr = aggr;

	for (int i = 0; i < prefetcher_it_size; i++)
	{
		pref->index_table[i].tag = -1;
		pref->index_table[i].ptr = -1;
	}

	for (int i = 0; i < prefetcher_ghb_size; i++)
	{
		pref->ghb[i].addr = -1;
		pref->ghb[i].next = -1;
		pref->ghb[i].prev = -1;
	}

	/* Return */
	return pref;
}


void prefetcher_free(struct prefetcher_t *pref)
{
	if (pref)
	{
		free(pref->ghb);
		free(pref->index_table);
		if(pref->streams)
		{
			for (int stream = 0; stream < pref->max_num_streams; stream++)
			{
				struct stream_buffer_t *sb = &pref->streams[stream];
				if (sb->pending_prefetches)
					fprintf(stderr, "WARNING: %d pending prefetches in cache %s stream %d since %lld\n",
						sb->pending_prefetches, pref->parent_cache->name, sb->stream, sb->time);
				free(pref->streams[stream].blocks);
			}
			free(pref->streams);
		}
		free(pref);
	}
}


int get_it_index_tag(struct prefetcher_t *pref, struct mod_stack_t *stack,
			     int *it_index, unsigned *tag)
{
	*it_index = -1;

	/* CZONE */
	if (prefetcher_uses_czone_indexed_ghb(pref))
	{
		PTR_ASSIGN(it_index, (stack->addr >> pref->czone_bits) % pref->it_size);
		PTR_ASSIGN(tag, stack->addr & pref->stream_tag_mask);
	}

	/* PC */
	else if (prefetcher_uses_pc_indexed_ghb(pref))
	{
		/* No PC information */
		if (stack->client_info->prefetcher_eip == -1)
			return 0;
		PTR_ASSIGN(it_index, stack->client_info->prefetcher_eip % pref->it_size);
		PTR_ASSIGN(tag, stack->client_info->prefetcher_eip);
	}

	/* ERROR */
	else
	{
		fatal("%s: Invalid prefetcher type", __FUNCTION__);
	}

	return 1;
}


/* Returns it_index >= 0 if any valid update is made, negative otherwise. */
int prefetcher_update_tables(struct mod_stack_t *stack)
{
	struct mod_t *mod = stack->target_mod ? stack->target_mod : stack->mod;
	struct prefetcher_t *pref = mod->cache->prefetcher;
	unsigned int it_tag;
	int ghb_index;
	int it_index;
	int prev;

	if (!mod->cache->prefetcher)
		return -1;

	/* Get the index table index and test if is valid */
	if(!get_it_index_tag(pref, stack, &it_index, &it_tag))
		return -1;

	assert(it_index < pref->it_size);

	ghb_index = (++(pref->ghb_head)) % pref->ghb_size;

	/* Remove the current entry in ghb_index, if its valid */
	if (pref->ghb[ghb_index].addr != -1) /* Implicit cast of -1 to unsigned (0xFFF...FFF) */
	{
		prev = pref->ghb[ghb_index].prev;

		/* The prev field may point to either index table or ghb. */
		if (pref->ghb[ghb_index].prev_it_ghb == prefetcher_ptr_ghb)
		{
			/* prev_it_gb == 0 implies the previous element is in the GHB */
			assert(prev >= 0 && prev < pref->ghb_size);
			assert(pref->ghb[prev].next == ghb_index);
			pref->ghb[prev].next = -1;
		}
		else
		{
			assert(pref->ghb[ghb_index].prev_it_ghb == prefetcher_ptr_it);

			/* The element in index_table may have been replaced since this
			 * entry was put into the ghb. */
			if (prev >= 0)
			{
				assert(pref->index_table[prev].ptr == ghb_index);
				pref->index_table[prev].ptr = -1;
			}
		}
	}
	pref->ghb[ghb_index].addr = -1; /* Not necessary, it will be overwritten */
	pref->ghb[ghb_index].next = -1; /* Same */
	pref->ghb[ghb_index].prev = -1; /* Same */

	/* Index table entry is valid */
	if (pref->index_table[it_index].tag != -1) /* Implicit cast of -1 to unsigned int */
	{
		/* Replace entry in index_table */
		if (pref->index_table[it_index].tag != it_tag)
		{
			mem_debug("  %lld it_index = %d, old_tag = 0x%x, new_tag = 0x%x"
				  "prefetcher: replace index_table entry\n", stack->id,
				  it_index, pref->index_table[it_index].tag, it_tag);

			prev = pref->index_table[it_index].ptr;

			/* The element in the ghb may have gone out by now. */
			if (prev >= 0)
			{
				/* The element that this is pointing to must be pointing back. */
				assert(pref->ghb[prev].prev_it_ghb == prefetcher_ptr_it &&
				       pref->ghb[prev].prev == it_index);
				pref->ghb[prev].prev = -1;
			}

			pref->index_table[it_index].tag = -1;
			pref->index_table[it_index].ptr = -1;
		}
	}

	/* Intex table entry is invalid */
	else
	{
		/* Just an initialization. Tag == -1 implies the entry has never been used. */
		pref->index_table[it_index].ptr = -1;
	}

	/* Add new element into ghb. */
	pref->ghb[ghb_index].addr = stack->addr;
	pref->ghb[ghb_index].next = pref->index_table[it_index].ptr;
	if (pref->index_table[it_index].ptr >= 0)
	{
	    prev = pref->index_table[it_index].ptr;
	    assert(pref->ghb[prev].prev_it_ghb == prefetcher_ptr_it &&
		   pref->ghb[prev].prev == it_index);
	    pref->ghb[prev].prev_it_ghb = prefetcher_ptr_ghb;
	    pref->ghb[prev].prev = ghb_index;
	}
	pref->ghb[ghb_index].prev_it_ghb = prefetcher_ptr_it;
	pref->ghb[ghb_index].prev = it_index;

	/* Make the index table entries point to current ghb_index. */
	pref->index_table[it_index].tag = it_tag;
	pref->index_table[it_index].ptr = ghb_index;

	/* Update pref->ghb_head so that its in the range possible. */
	pref->ghb_head = ghb_index;

	return it_index;
}


void prefetch_to_cache(
		struct mod_t *mod, struct mod_client_info_t *client_info,
		unsigned int pref_addr)
{
	struct mod_client_info_t *ci;

	assert(pref_addr != -1);

	ci = mod_client_info_create(mod);
	ci->core = client_info->core;
	ci->thread = client_info->thread;
	ci->ctx = client_info->ctx;

	mod_access(mod, mod_access_prefetch, pref_addr, NULL, NULL, NULL, ci);
}


/* This function implements the GHB based PC/CS prefetching as described in the
 * 2005 paper by Nesbit and Smith. The index table lookup is based on the PC
 * of the instruction causing the miss. The GHB entries are looked at for finding
 * constant stride accesses. Based on this, prefetching is done. */
static void prefetcher_ghb_cs(struct mod_t *mod, struct mod_stack_t *stack, int it_index)
{
	int stride;
	struct prefetcher_t *pref = mod->cache->prefetcher;

	assert(mod->kind == mod_kind_cache && mod->cache != NULL);

	/* The table should've been updated before calling this function. */
	assert(pref->ghb[pref->index_table[it_index].ptr].addr == stack->addr);

	stride = prefetcher_ghb_cs_find_stride(pref, it_index);

	if (!stride)
		return;

	switch(pref->type)
	{
		case prefetcher_type_pc_cs:
		case prefetcher_type_cz_cs:
		{
			for(int i = 1; i <= pref->aggr; i++)
			{
				unsigned int pref_addr = stack->addr + i * stride;
				if (!valid_prefetch_addr(mod, pref_addr, stride))
					return;
				prefetch_to_cache(mod, stack->client_info, stack->addr + i * stride);
			}
			break;
		}

		case prefetcher_type_pc_cs_sb:
		case prefetcher_type_cz_cs_sb:
		{
			if (stack->stream_hit)
				stream_buffer_prefetch_in_stream(mod, stack->client_info, stack->pref_stream, stack->pref_slot);
			else if(stack->stride)
				stream_buffer_allocate_stream(mod, stack->client_info, stack->addr, stack->stride);
			break;
		}

		default:
		{
			fatal("%s: Invalid prefetcher type", __FUNCTION__);
			break;
		}
	}
}


int prefetcher_ghb_cs_find_stride(struct prefetcher_t *pref, int it_index)
{
	int stride;
	int chain;
	unsigned int prev_addr;
	unsigned int cur_addr;

	chain = pref->index_table[it_index].ptr;

	/* The lookup depth must be at least 2 - which essentially means
	 * two strides have been seen so far, prefetch for the next.
	 * It doesn't really help to prefetch on a lookup of depth 1.
	 * It is too low an accuracy and leads to lot of illegal and
	 * redundant prefetches. Hence keeping the minimum at 2. */
	assert(pref->lookup_depth >= 2);

	/* If there's only one element in this linked list, nothing to do. */
	if (pref->ghb[chain].next == -1)
		return 0;

	prev_addr = pref->ghb[chain].addr;
	chain = pref->ghb[chain].next;
	cur_addr = pref->ghb[chain].addr;
	stride = prev_addr - cur_addr;

	for (int i = 2; i <= pref->lookup_depth; i++)
	{
		prev_addr = cur_addr;
		chain = pref->ghb[chain].next;

		/* The linked list (history) is smaller than the lookup depth */
		if (chain == -1)
			goto no_stride;

		cur_addr = pref->ghb[chain].addr;

		/* The stride changed, can't prefetch */
		if (stride != prev_addr - cur_addr)
			goto no_stride;
	}
	return stride;

no_stride:
	return 0;
}


/* This function implements the GHB based PC/DC prefetching as described in the
 * 2005 paper by Nesbit and Smith. The index table lookup is based on the PC
 * of the instruction causing the miss. The last three accesses are looked at
 * to find the last two strides (deltas). The list is then looked up backwards
 * to see if this pair of strides occurred earlier, if yes, the next stride
 * is obtained from the history there. This stride decides the new prefetch_addr. */
static void prefetcher_ghb_dc(struct mod_t *mod, struct mod_stack_t *stack, int it_index)
{
	struct prefetcher_t *pref;
	int chain, chain2, stride[PREFETCHER_LOOKUP_DEPTH_MAX], i, pref_stride;
	unsigned int prev_addr, cur_addr, prefetch_addr = 0;

	assert(mod->kind == mod_kind_cache && mod->cache != NULL);
	pref = mod->cache->prefetcher;

	chain = pref->index_table[it_index].ptr;

	/* The lookup depth must be at least 2 - which essentially means
	 * two strides have been seen so far, predict the next stride. */
	assert(pref->lookup_depth >= 2 && pref->lookup_depth <= PREFETCHER_LOOKUP_DEPTH_MAX);

	/* The table should've been updated before calling this function. */
	assert(pref->ghb[chain].addr == stack->addr);

	/* Collect "lookup_depth" number of strides (deltas).
	 * This doesn't really make sense for a depth > 2, but
	 * I'll just have the code here for generality. */
	for (i = 0; i < pref->lookup_depth; i++)
	{
		prev_addr = pref->ghb[chain].addr;
		chain = pref->ghb[chain].next;

		/* The chain isn't long enough */
		if (chain == -1)
			return;

		cur_addr = pref->ghb[chain].addr;
		stride[i] = prev_addr - cur_addr;
	}

	chain = pref->index_table[it_index].ptr;
	chain = pref->ghb[chain].next;
	assert(chain != -1);

	/* "chain" now points to the second element of the list.
	 * Try to match the stride array starting from here. */
	while (chain != -1)
	{
		/* This really doesn't look realistic to implement in
		 * hardware. Too much time consuming I feel. */
		chain2 = chain;
		for (i = 0; i < pref->lookup_depth; i++)
		{
			prev_addr = pref->ghb[chain2].addr;
			chain2 = pref->ghb[chain2].next;

			/* The chain isn't long enough and we
			 * haven't found a match till now. */
			if (chain2 == -1)
				return;

			cur_addr = pref->ghb[chain2].addr;
			if (stride[i] != prev_addr - cur_addr)
				break;
		}

		/* If we traversed the above loop full, we have a match. */
		if (i == pref->lookup_depth)
		{
		    cur_addr = pref->ghb[chain].addr;
		    assert(pref->ghb[chain].prev != -1 &&
			   pref->ghb[chain].prev_it_ghb == prefetcher_ptr_ghb);
		    chain = pref->ghb[chain].prev;
		    prev_addr = pref->ghb[chain].addr;
		    pref_stride = prev_addr - cur_addr;
		    prefetch_addr = stack->addr + pref_stride;
		    break;
		}

		chain = pref->ghb[chain].next;
	}

	if (prefetch_addr > 0)
	{
		prefetch_to_cache(mod, stack->client_info, prefetch_addr);
	}
}


void prefetcher_cache_miss(struct mod_stack_t *stack, struct mod_t *target_mod)
{
	int it_index;

	if (!can_prefetch(stack))
		return;

	assert(!stack->stream_hit);

	/* Get the index table index and test if is valid */
	if(!get_it_index_tag(target_mod->cache->prefetcher, stack, &it_index, NULL))
		return;

	switch(target_mod->cache->prefetcher->type)
	{
		case prefetcher_type_cz_cs:
		case prefetcher_type_pc_cs:
		{
			/* Perform ghb based CS prefetching
			* (Program Counter/Czone based index, Constant Stride) */
			prefetcher_ghb_cs(target_mod, stack, it_index);
			break;
		}

		case prefetcher_type_pc_dc:
		{
			/* Perform ghb based PC/DC prefetching
			* (Program Counter based index, Delta Correlation) */
			prefetcher_ghb_dc(target_mod, stack, it_index);
			break;
		}

		default:
		{
			fatal("%s: Invalid prefetcher type", __FUNCTION__);
			break;
		}
	}
}


void prefetcher_cache_hit(struct mod_stack_t *stack, struct mod_t *target_mod)
{
	int it_index;

	assert(!stack->stream_hit);

	if (!can_prefetch(stack))
		return;

	/* Get the index table index and test if is valid */
	if(!get_it_index_tag(target_mod->cache->prefetcher, stack, &it_index, NULL))
		return;

	/* Enqueue prefetch */
	switch(target_mod->cache->prefetcher->type)
	{
		case prefetcher_type_cz_cs:
		case prefetcher_type_pc_cs:
		{
			/* Perform ghb based {PC,CZ}/CS prefetching */
			prefetcher_ghb_cs(target_mod, stack, it_index);
			break;
		}

		case prefetcher_type_pc_dc:
		{
			/* Perform ghb based PC/DC prefetching
			* (Program Counter based index, Delta Correlation) */
			prefetcher_ghb_dc(target_mod, stack, it_index);
			break;
		}

		default:
			fatal("%s: Invalid prefetcher type", __FUNCTION__);
			break;
	}
}


int prefetcher_uses_stream_buffers(struct prefetcher_t *pref)
{
	if (!pref) return 0;
	switch(pref->type)
	{
		case prefetcher_type_cz_cs_sb:
		case prefetcher_type_pc_cs_sb:
			return 1;
		default:
	 		return 0;
	}
}


int prefetcher_uses_pc_indexed_ghb(struct prefetcher_t *pref)
{
	if (!pref) return 0;
	switch(pref->type)
	{
		case prefetcher_type_pc_cs:
		case prefetcher_type_pc_dc:
		case prefetcher_type_pc_cs_sb:
			return 1;
		default:
			return 0;
	}
}


int prefetcher_uses_czone_indexed_ghb(struct prefetcher_t *pref)
{
	if (!pref) return 0;
	switch(pref->type)
	{
		case prefetcher_type_cz_cs:
		case prefetcher_type_cz_cs_sb:
			return 1;
		default:
	 		return 0;
	}
}


int valid_prefetch_addr(struct mod_t *mod, unsigned int pref_addr, int stride)
{
	unsigned int prev_addr = pref_addr - stride;
	unsigned int mmu_page_tag = pref_addr & ~mmu_page_mask;

	return (mmu_page_tag == (prev_addr & ~mmu_page_mask)) && /* No page boundary crossed */
		!(stride > 0 && pref_addr < prev_addr) && /* No overflow */
		!(stride < 0 && pref_addr > prev_addr) && /* No underflow */
		mod_serves_address(mod, pref_addr); /* The address is served by the module */
}


int can_prefetch(struct mod_stack_t *stack)
{
	struct mod_t *mod = stack->target_mod ? stack->target_mod : stack->mod;

	return mod->cache->prefetcher && /* This module has a prefetcher */
		mod->cache->prefetcher->enabled && /* Prefetch is enabled */
		!stack->prefetch && /* A prefetch cannot trigger more prefetches */
		!stack->background && /* Background stacks can't enqueue prefetches */
		mod->kind == mod_kind_cache && /* Only enqueue prefetches in cache modules */
		stack->request_dir == mod_request_up_down; /* Only a up-down request can trigger a prefetch */
}


void prefetch_to_stream_buffer(
		struct mod_t *mod, struct mod_client_info_t *client_info,
		int stream, int num_prefetches)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;
	unsigned int prev_address;
	int max_num_slots;
	int max_num_streams;
	int i;

	sb = &pref->streams[stream];

	max_num_slots = pref->max_num_slots;
	max_num_streams = pref->max_num_streams;

	/* Assertions */
	assert(sb->stride);
	assert(num_prefetches < max_num_slots);
	assert(stream >= 0 && stream < max_num_streams);
	assert(!sb->dead);
	assert(sb->next_address % cache->block_size == 0);

	/* Set time */
	sb->time = esim_time;

	/* Debug */
	mem_debug("    Enqueued %d prefetches at addr=0x%x to stream=%d with stride=0x%x\n", num_prefetches, sb->next_address, stream, sb->stride);
	mem_debug("    head = %d tail = %d\n", sb->head, sb->tail);

	/* Insert prefetches */
	for (i = 0; i < num_prefetches; i++)
	{
		/* Reached maximum prefetching distance */
		if (sb->pending_prefetches == max_num_slots)
			break;

		client_info = mod_client_info_clone(mod, client_info);
		client_info->stream = stream;
		client_info->slot = sb->tail;

		mod_access(mod, mod_access_prefetch, sb->next_address, NULL, NULL, NULL, client_info);

		sb->tail = (sb->tail + 1) % max_num_slots; //TAIL
		sb->pending_prefetches++;

		prev_address = sb->next_address;
		sb->next_address += sb->stride;

		/* End of page */
		if((prev_address & ~mmu_page_mask) != (sb->next_address & ~mmu_page_mask))
		{
			sb->dead = 1;
			break;
		}

		/* Underflow */
		if(sb->stride < 0 && sb->next_address > prev_address)
		{
			sb->dead = 1;
			break;
		}

		/* Overflow */
		if(sb->stride > 0 && sb->next_address < prev_address)
		{
			sb->dead = 1;
			break;
		}

		/* End of stream */
		if (sb->stream_transcient_tag != (sb->next_address & pref->stream_tag_mask))
			break;
	}
}


void stream_buffer_allocate_stream(struct mod_t *mod, struct mod_client_info_t *client_info, unsigned int miss_addr, int stride)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;
	unsigned int stream_tag;
	unsigned int base_addr;
	int stream;

	assert(stride);

	base_addr = miss_addr + stride;
	stream_tag = base_addr & pref->stream_tag_mask;

	/* If there is a stream with the same tag, replace it. If not, replace the last recently used one. */
	stream = cache_find_stream(cache, stream_tag);
	if (stream == -1) /* Stream tag not found */
		stream = cache_select_stream(cache);
	sb = &pref->streams[stream];

	/* Invalid prefetch address */
	if (!valid_prefetch_addr(mod, base_addr, stride))
		return;

	/* Pending prefetches */
	if (sb->pending_prefetches)
		return;

	/* Locked blocks */
	for (int slot = 0; slot < pref->max_num_slots; slot++)
	{
		struct dir_lock_t *dir_lock = dir_pref_lock_get(mod->dir, stream, slot);
		if (dir_lock->lock)
		{
			printf("Ouch!\n");
			return;
		}
	}

	/* Invalidate all blocks in the stream */
	for (int i = 0; i < pref->max_num_slots; i++)
	{
		sb->blocks[i].tag = block_invalid_tag;
		sb->blocks[i].transient_tag = block_invalid_tag;
		sb->blocks[i].state = cache_block_invalid;
	}

	sb->head = 0;
	sb->tail = 0;
	sb->next_address = base_addr;
	sb->stream_transcient_tag = stream_tag;
	sb->dead = 0;
	sb->stride = stride;
	sb->stream_tag = block_invalid_tag;

	prefetch_to_stream_buffer(mod, client_info, stream, pref->aggr_ini);
}


void stream_buffer_prefetch_in_stream(struct mod_t *mod, struct mod_client_info_t *client_info, int stream, int slot)
{
	struct prefetcher_t *pref;
	struct stream_buffer_t *sb;
	unsigned int prev_address;
	unsigned int mmu_page_tag;

	pref = mod->cache->prefetcher;

	/* Assure that the stream and the slot are correct */
	assert(stream >= 0 && stream < pref->max_num_streams);
	assert(slot >= 0 && slot < pref->max_num_slots);

	sb = &pref->streams[stream];
	prev_address = sb->next_address - sb->stride;
	mmu_page_tag = prev_address & ~mmu_page_mask;

	/* No more prefetching in this stream */
	if (sb->dead)
		return;

	/* Test for invalid states */
	assert(sb->stride); /* Invalid stride */
	assert(mmu_page_tag == (sb->next_address & ~mmu_page_mask)); /* End of page */
	assert(sb->stride > 0 || (prev_address + sb->stride) < prev_address); /* Underflow */
	assert(sb->stride < 0 || (prev_address + sb->stride) > prev_address); /* Overflow */

	/* Allocate a new stream if next_address is not in the stream anymore */
	if (sb->stream_transcient_tag != (sb->next_address & pref->stream_tag_mask))
	{
		sb->dead = 1; /* Only do this the first time */
		stream_buffer_allocate_stream(mod, client_info, sb->next_address - sb->stride, sb->stride);
		return;
	}

	prefetch_to_stream_buffer(mod, client_info, stream, pref->aggr);
}


void prefetcher_stream_buffer_hit(struct mod_stack_t *stack)
{
	struct mod_t *mod;
	assert(stack->stream_hit);

	if (!can_prefetch(stack))
		return;

	mod = stack->target_mod ? stack->target_mod : stack->mod;

	if (stack->stream_head_hit)
		stream_buffer_prefetch_in_stream(mod, stack->client_info, stack->pref_stream, stack->pref_slot);
}


void prefetcher_stream_buffer_miss(struct mod_stack_t *stack)
{
	int it_index;
	int stride;
	struct mod_t *mod;

	assert(!stack->stream_hit);

	if (!can_prefetch(stack))
		return;

	mod = stack->target_mod ? stack->target_mod : stack->mod;

	/* Get the index table index and test if is valid */
	if(!get_it_index_tag(mod->cache->prefetcher, stack, &it_index, NULL))
		return;

	stride = prefetcher_ghb_cs_find_stride(mod->cache->prefetcher, it_index);
	if (stride)
		stream_buffer_allocate_stream(mod, stack->client_info, stack->addr, stride);
}


void prefetcher_stream_buffers_create(struct prefetcher_t *pref, int max_num_streams, int max_num_slots)
{
	pref->max_num_slots = max_num_slots;
	pref->max_num_streams = max_num_streams;

	/* Create matrix of prefetched blocks */
	pref->streams = xcalloc(max_num_streams, sizeof(struct stream_buffer_t));
	for(int stream = 0; stream < max_num_streams; stream++)
		pref->streams[stream].blocks = xcalloc(max_num_slots, sizeof(struct stream_block_t));

	/* Initialize streams */
	pref->stream_head = &pref->streams[0];
	pref->stream_tail = &pref->streams[max_num_streams - 1];
	for (int stream = 0; stream < max_num_streams; stream++)
	{
		struct stream_buffer_t *sb = &pref->streams[stream];
		sb->stream = stream;
		sb->stream_tag = -1; /* 0xFFFF...FFFF */
		sb->stream_transcient_tag = -1; /* 0xFFFF...FFFF */
		sb->stream_prev = stream ? &pref->streams[stream - 1] : NULL;
		sb->stream_next = stream < max_num_streams - 1 ?
			&pref->streams[stream + 1] : NULL;
		for(int slot = 0; slot < max_num_slots; slot++)
			sb->blocks[slot].slot = slot;
	}
}


/* Adaptive prefetcher that uses bloom filters to estimate pollution */
int prefetcher_uses_pollution_filters(struct prefetcher_t *pref)
{
	if (!pref || prefetcher_uses_stream_buffers(pref) || !pref->adapt_policy)
		return 0;
	switch(pref->adapt_policy)
	{
		case adapt_pref_policy_fdp:
		case adapt_pref_policy_hpac:
			return 1;
		default:
			return 0;
	}
}


void prefetcher_set_default_adaptive_thresholds(struct prefetcher_t *pref)
{
	if (!pref) return;

	switch (pref->adapt_policy)
	{
		case adapt_pref_policy_adp:
			#define POLICY adp
			pref->th.POLICY.a1 = 0;
			pref->th.POLICY.a2 = 2;
			pref->th.POLICY.a3 = 4;
			pref->th.POLICY.bwno = 2.75;
			pref->th.POLICY.cov = 0.3;
			pref->th.POLICY.acc_very_low = 0.2;
			pref->th.POLICY.acc_low = 0.4;
			pref->th.POLICY.acc_high = 0.8;
			pref->th.POLICY.misses = 1.15;
			pref->th.POLICY.ipc = 0.9;
			pref->th.POLICY.rob_stall = 0.6;
			#undef POLICY
			break;

		case adapt_pref_policy_hpac:
			#define POLICY hpac
			pref->th.POLICY.a1 = 1;
			pref->th.POLICY.a2 = 2;
			pref->th.POLICY.a3 = 4;
			pref->th.POLICY.acc = 0.60;
			pref->th.POLICY.bwc = 50000;
			pref->th.POLICY.bwno = 75000;
			pref->th.POLICY.pollution = 90; /* NOTE: These fucking morons use here an absolute value while in FDP use a ratio */
			#undef POLICY
			/* No 'break' since HPAC needs FDP */

		case adapt_pref_policy_fdp:
			#define POLICY fdp
			pref->th.POLICY.a1 = 1;
			pref->th.POLICY.a2 = 2;
			pref->th.POLICY.a3 = 4;
			pref->th.POLICY.acc_low = 0.40;
			pref->th.POLICY.acc_high = 0.75;
			pref->th.POLICY.lateness = 0.01;
			pref->th.POLICY.pollution = 0.005; /* NOTE: See HPAC thresholds */
			#undef POLICY
			break;

		case adapt_pref_policy_mbp:
			#define POLICY mbp
			pref->th.POLICY.ratio = 0.15;
			pref->th.POLICY.a1 = 1;
			pref->th.POLICY.a2 = 2;
			pref->th.POLICY.a3 = 4;
			#undef POLICY
			break;

		default:
			break;
	}
}
