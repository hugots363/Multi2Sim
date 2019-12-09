/*
 *  Multi2Sim
 *  Copyright (C) 2014  Vicent Selfa (viselol@disca.upv.es)
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

#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/misc.h>

#include "atd.h"
#include "cache.h"
#include "module.h"


/*
 * Private Structures
 */


enum atd_waylist_enum
{
	atd_waylist_head,
	atd_waylist_tail
};


/*
 * Private Functions
 */


void atd_update_waylist(struct atd_set_t *set, struct atd_block_t *blk, enum atd_waylist_enum where)
{
	if (!blk->way_prev && !blk->way_next)
	{
		assert(set->way_head == blk && set->way_tail == blk);
		return;

	}
	else if (!blk->way_prev)
	{
		assert(set->way_head == blk && set->way_tail != blk);
		if (where == atd_waylist_head)
			return;
		set->way_head = blk->way_next;
		blk->way_next->way_prev = NULL;

	}
	else if (!blk->way_next)
	{
		assert(set->way_head != blk && set->way_tail == blk);
		if (where == atd_waylist_tail)
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

	if (where == atd_waylist_head)
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


/* Return the way of the block to be replaced in a specific set,
 * depending on the replacement policy */
int atd_replace_block(struct atd_t *atd, int set)
{
	struct cache_t *cache = atd->mod->cache;

	assert(set >= 0 && set < cache->num_sets);

	/* The set is not in the ATD */
	if (set >= atd->num_sets)
		return -1;

	/* LRU and FIFO replacement: return block at the
	 * tail of the linked list */
	if (cache->policy == cache_policy_lru ||
		cache->policy == cache_policy_fifo ||
		cache->policy == cache_policy_partitioned_lru)
	{
		int way = atd->sets[set].way_tail->way;
		atd_update_waylist(&atd->sets[set], atd->sets[set].way_tail, atd_waylist_head);
		return way;
	}

	/* Random replacement */
	assert(cache->policy == cache_policy_random);
	return random() % cache->assoc;
}


/*
 * Public Functions
 */


struct atd_t *atd_create(struct mod_t *mod, int num_sets)
{
	struct atd_t *atd;
	int assoc = mod->cache->assoc;

	/* Initialize */
	atd = xcalloc(1, sizeof(struct atd_t));
	atd->mod = mod;
	atd->num_sets = num_sets;

	/* Initialize array of sets */
	atd->sets = xcalloc(num_sets, sizeof(struct atd_set_t));
	for (int set = 0; set < num_sets; set++)
	{
		/* Initialize array of blocks */
		atd->sets[set].blocks = xcalloc(assoc, sizeof(struct atd_block_t));
		atd->sets[set].way_head = &atd->sets[set].blocks[0];
		atd->sets[set].way_tail = &atd->sets[set].blocks[assoc - 1];
		for (int way = 0; way < assoc; way++)
		{
			struct atd_block_t *block;
			block = &atd->sets[set].blocks[way];
			block->way = way;
			block->way_prev = way ? &atd->sets[set].blocks[way - 1] : NULL;
			block->way_next = way < assoc - 1 ? &atd->sets[set].blocks[way + 1] : NULL;
		}
	}

	/* Create umon counters */
	atd->stack_distance_counters = xcalloc(assoc, sizeof(long long));

	return atd;
}


void atd_free(struct atd_t *atd)
{
	if (!atd)
		return;
	for (int set = 0; set < atd->num_sets; set++)
		free(atd->sets[set].blocks);
	free(atd->sets);
	free(atd->stack_distance_counters);
	free(atd);
}


int atd_set_block(struct atd_t *atd, int addr, int state)
{
	struct cache_t *cache = atd->mod->cache;
	int set;
	int way;
	int tag;

	atd_find_block(atd, addr, &set, &way, &tag, NULL);
	if (way < 0)
		way = atd_replace_block(atd, set);

	assert(set >= 0 && set < cache->num_sets);
	assert(way >= 0 && way < cache->assoc);

	/* The set is not in the ATD */
	if (set >= atd->num_sets)
		return -1;

	if (cache->policy == cache_policy_fifo && atd->sets[set].blocks[way].tag != tag)
		atd_update_waylist(&atd->sets[set], &atd->sets[set].blocks[way], atd_waylist_head);

	atd->sets[set].blocks[way].tag = tag;
	atd->sets[set].blocks[way].state = state;

	return 1;
}


int atd_find_block(struct atd_t *atd, unsigned int addr, int *set_ptr, int *way_ptr, int *tag_ptr, int *state_ptr)
{
	struct cache_t *cache = atd->mod->cache;
	struct mod_t *mod = atd->mod;
	int set, tag, way;

	tag = addr & ~cache->block_mask;
	if (mod->range_kind == mod_range_interleaved)
	{
		unsigned int num_mods = mod->range.interleaved.mod;
		set = ((tag >> cache->log_block_size) / num_mods) % cache->num_sets;
	}
	else if (mod->range_kind == mod_range_bounds)
		set = (tag >> cache->log_block_size) % cache->num_sets;
	else
		panic("%s: invalid range kind (%d)", __FUNCTION__, mod->range_kind);

	assert(set >= 0 && set < cache->num_sets);

	PTR_ASSIGN(set_ptr, set);
	PTR_ASSIGN(tag_ptr, tag);
	PTR_ASSIGN(way_ptr, -2); /* Invalid state */
	PTR_ASSIGN(state_ptr, -2); /* Invalid state */

	/* The set is not in the ATD */
	if (set >= atd->num_sets)
		return -1;

	/* Locate block */
	for (way = 0; way < cache->assoc; way++)
	{
		struct atd_block_t *blk = &atd->sets[set].blocks[way];
		if (blk->tag == tag && blk->state)
			break;
	}

	/* Block not found */
	if (way == cache->assoc)
		return 0;

	/* Block found */
	PTR_ASSIGN(way_ptr, way);
	return 1;
}


/* Update LRU counters, i.e., rearrange linked list in case
 * replacement policy is LRU. */
int atd_access_block(struct atd_t *atd, unsigned int addr)
{
	struct cache_t *cache = atd->mod->cache;
	int move_to_head;
	int set;
	int way;
	int ret_value;

	ret_value = atd_find_block(atd, addr, &set, &way, NULL, NULL);

	if (ret_value != 1)
		return ret_value;

	assert(set >= 0 && set < cache->num_sets);
	assert(way >= 0 && way < cache->assoc);

	/* A block is moved to the head of the list for LRU policy.
	 * It will also be moved if it is its first access for FIFO policy, i.e., if the
	 * state of the block was invalid. */
	move_to_head = cache->policy == cache_policy_lru ||
			cache->policy == cache_policy_partitioned_lru ||
			(cache->policy == cache_policy_fifo && !atd->sets[set].blocks[way].state);
	if (move_to_head && atd->sets[set].blocks[way].way_prev)
		atd_update_waylist(&atd->sets[set], &atd->sets[set].blocks[way], atd_waylist_head);
	return 1;
}


int atd_get_stack_distance(struct atd_t *atd, int set, int way)
{
	int distance = 0;

	assert(set >= 0 && set < atd->mod->cache->num_sets);
	assert(way >= 0 && way < atd->mod->cache->assoc);

	for (struct atd_block_t *block = atd->sets[set].blocks[way].way_prev; block; block = block->way_prev)
		distance++;

	return distance;
}


/* Count hits in each step of the LRU stack */
void atd_update_stack_distance_counters(struct atd_t *atd, int set, int way)
{
	int distance = atd_get_stack_distance(atd, set, way);
	atd->stack_distance_counters[distance]++;
}
