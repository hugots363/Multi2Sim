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
 *  You should have received stack copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>

#include <arch/x86/emu/context.h>
#include <arch/x86/timing/cpu.h>
#include <arch/x86/timing/uop.h>

#include <dramsim/bindings-c.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/bloom.h>
#include <lib/util/debug.h>
#include <lib/util/hash-table-gen.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>
#include <lib/util/misc.h>
#include <lib/util/string.h>
#include <network/network.h>
#include <network/node.h>

#include "atd.h"
#include "cache.h"
#include "directory.h"
#include "mem-system.h"
#include "mmu.h"
#include "mod-stack.h"
#include "prefetcher.h"
#include "stream-prefetcher.h"

/* Events */

int EV_MOD_NMOESI_LOAD;
int EV_MOD_NMOESI_LOAD_LOCK;
int EV_MOD_NMOESI_LOAD_ACTION;
int EV_MOD_NMOESI_LOAD_MISS;
int EV_MOD_NMOESI_LOAD_UNLOCK;
int EV_MOD_NMOESI_LOAD_FINISH;

int EV_MOD_NMOESI_STORE;
int EV_MOD_NMOESI_STORE_LOCK;
int EV_MOD_NMOESI_STORE_ACTION;
int EV_MOD_NMOESI_STORE_UNLOCK;
int EV_MOD_NMOESI_STORE_FINISH;

int EV_MOD_NMOESI_PREFETCH;
int EV_MOD_NMOESI_PREFETCH_LOCK;
int EV_MOD_NMOESI_PREFETCH_ACTION;
int EV_MOD_NMOESI_PREFETCH_MISS;
int EV_MOD_NMOESI_PREFETCH_UNLOCK;
int EV_MOD_NMOESI_PREFETCH_FINISH;

int EV_MOD_NMOESI_NC_STORE;
int EV_MOD_NMOESI_NC_STORE_LOCK;
int EV_MOD_NMOESI_NC_STORE_WRITEBACK;
int EV_MOD_NMOESI_NC_STORE_ACTION;
int EV_MOD_NMOESI_NC_STORE_MISS;
int EV_MOD_NMOESI_NC_STORE_UNLOCK;
int EV_MOD_NMOESI_NC_STORE_FINISH;

int EV_MOD_NMOESI_FIND_AND_LOCK;
int EV_MOD_NMOESI_FIND_AND_LOCK_PORT;
int EV_MOD_NMOESI_FIND_AND_LOCK_PREF_STREAM;
int EV_MOD_NMOESI_FIND_AND_LOCK_ACTION;
int EV_MOD_NMOESI_FIND_AND_LOCK_FINISH;

int EV_MOD_NMOESI_EVICT;
int EV_MOD_NMOESI_EVICT_INVALID;
int EV_MOD_NMOESI_EVICT_ACTION;
int EV_MOD_NMOESI_EVICT_RECEIVE;
int EV_MOD_NMOESI_EVICT_PROCESS;
int EV_MOD_NMOESI_EVICT_PROCESS_NONCOHERENT;
int EV_MOD_NMOESI_EVICT_REPLY;
int EV_MOD_NMOESI_EVICT_REPLY_RECEIVE;
int EV_MOD_NMOESI_EVICT_FINISH;

int EV_MOD_NMOESI_WRITE_REQUEST;
int EV_MOD_NMOESI_WRITE_REQUEST_RECEIVE;
int EV_MOD_NMOESI_WRITE_REQUEST_LOCK;
int EV_MOD_NMOESI_WRITE_REQUEST_ACTION;
int EV_MOD_NMOESI_WRITE_REQUEST_EXCLUSIVE;
int EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN;
int EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_FINISH;
int EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_LATENCY;
int EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP;
int EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP_FINISH;
int EV_MOD_NMOESI_WRITE_REQUEST_REPLY;
int EV_MOD_NMOESI_WRITE_REQUEST_FINISH;

int EV_MOD_NMOESI_READ_REQUEST;
int EV_MOD_NMOESI_READ_REQUEST_RECEIVE;
int EV_MOD_NMOESI_READ_REQUEST_LOCK;
int EV_MOD_NMOESI_READ_REQUEST_ACTION;
int EV_MOD_NMOESI_READ_REQUEST_UPDOWN;
int EV_MOD_NMOESI_READ_REQUEST_UPDOWN_MISS;
int EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH;
int EV_MOD_NMOESI_READ_REQUEST_UPDOWN_LATENCY;
int EV_MOD_NMOESI_READ_REQUEST_DOWNUP;
int EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS;
int EV_MOD_NMOESI_READ_REQUEST_DOWNUP_FINISH;
int EV_MOD_NMOESI_READ_REQUEST_REPLY;
int EV_MOD_NMOESI_READ_REQUEST_FINISH;

int EV_MOD_NMOESI_INVALIDATE;
int EV_MOD_NMOESI_INVALIDATE_FINISH;

int EV_MOD_NMOESI_PEER_SEND;
int EV_MOD_NMOESI_PEER_RECEIVE;
int EV_MOD_NMOESI_PEER_REPLY;
int EV_MOD_NMOESI_PEER_FINISH;

int EV_MOD_NMOESI_MESSAGE;
int EV_MOD_NMOESI_MESSAGE_RECEIVE;
int EV_MOD_NMOESI_MESSAGE_ACTION;
int EV_MOD_NMOESI_MESSAGE_REPLY;
int EV_MOD_NMOESI_MESSAGE_FINISH;

int EV_MOD_PREF;
int EV_MOD_PREF_LOCK;
int EV_MOD_PREF_ACTION;
int EV_MOD_PREF_MISS;
int EV_MOD_PREF_UNLOCK;
int EV_MOD_PREF_FINISH;

int EV_MOD_NMOESI_PREF_FIND_AND_LOCK;
int EV_MOD_NMOESI_PREF_FIND_AND_LOCK_PORT;
int EV_MOD_NMOESI_PREF_FIND_AND_LOCK_ACTION;
int EV_MOD_NMOESI_PREF_FIND_AND_LOCK_FINISH;

int EV_MOD_NMOESI_INVALIDATE_SLOT;
int EV_MOD_NMOESI_INVALIDATE_SLOT_LOCK;
int EV_MOD_NMOESI_INVALIDATE_SLOT_ACTION;
int EV_MOD_NMOESI_INVALIDATE_SLOT_UNLOCK;
int EV_MOD_NMOESI_INVALIDATE_SLOT_FINISH;


int EV_MOD_NMOESI_EXAMINE_ONLY_ONE_QUEUE_REQUEST;
int EV_MOD_NMOESI_EXAMINE_QUEUE_REQUEST;
int EV_MOD_NMOESI_ACCES_BANK;
int EV_MOD_NMOESI_TRANSFER_FROM_BANK;
int EV_MOD_NMOESI_ACCES_TABLE;
int EV_MOD_NMOESI_TRANSFER_FROM_TABLE;
int EV_MOD_NMOESI_REMOVE_MEMORY_CONTROLLER;
int EV_MOD_NMOESI_INSERT_MEMORY_CONTROLLER;

int EV_MOD_NMOESI_FIND_AND_LOCK_MEM_CONTROLLER;
int EV_MOD_NMOESI_FIND_AND_LOCK_MEM_CONTROLLER_PORT;
int EV_MOD_NMOESI_FIND_AND_LOCK_MEM_CONTROLLER_ACTION;
int EV_MOD_NMOESI_FIND_AND_LOCK_MEM_CONTROLLER_FINISH;


int EV_MOD_ATD_DELAYED_SET;



int should_count_stats(struct mod_stack_t *stack)
{
	struct mod_t *mod = stack->target_mod ? stack->target_mod : stack->mod;
	return stack->request_dir == mod_request_up_down &&
		!stack->prefetch &&
		mod->kind == mod_kind_cache &&
		(stack->access_kind == mod_access_load ||
		stack->access_kind == mod_access_store ||
		stack->access_kind == mod_access_read_request ||
		stack->access_kind == mod_access_write_request) &&
		!stack->background;
}


/* NMOESI Protocol */

void mod_handler_pref(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;
	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;
	struct x86_ctx_t *ctx = stack->client_info->ctx;

	assert(ctx);

	stack->prefetch = mod->level;

	if (event == EV_MOD_PREF)
	{
		mem_debug("%lld %lld 0x%x %s stream=%d slot=%d %s pref\n", esim_time, stack->id,
			stack->addr, mod->name, stack->client_info->stream, stack->client_info->slot,
			str_map_value(&stream_request_kind_map, stack->client_info->stream_request_kind));
		mem_trace("mem.new_access name=\"A-%lld\" type=\"pref\" state=\"%s:pref\" addr=0x%x\n",
			stack->id, mod->name, stack->addr);

		/* Record access */
		mod_access_start(mod, stack, mod_access_prefetch);

		/* Measure end-to-end latency */
		stack->start_time = esim_time;

		/* Statistics */
		mod->programmed_prefetches++;

		/* Set pref stream and slot */
		stack->pref_stream = stack->client_info->stream;
		stack->pref_slot = stack->client_info->slot;

		/* Next event */
		esim_schedule_event(EV_MOD_PREF_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_PREF_LOCK)
	{
		struct mod_stack_t *master_stack;
		struct mod_stack_t *older_stack;

		mem_debug("  %lld %lld 0x%x %s pref lock\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_lock\"\n", stack->id, mod->name);

		/* Coalesce access, if so die or invalidate slot */
		master_stack = mod_can_coalesce(mod, mod_access_prefetch, stack->addr, stack);
		if (master_stack)
		{
			mem_debug("    %lld will finish due to %lld\n", stack->id, master_stack->id);
			mod->canceled_prefetches++; /* Statistics */
			mod->canceled_prefetches_coalesce++; /* Statistics */
			new_stack = mod_stack_create(stack->id, mod, stack->addr, EV_MOD_PREF_FINISH, stack, stack->prefetch);
			new_stack->client_info = mod_client_info_clone(stack->mod, stack->client_info);
			new_stack->retry = 0;
			esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_SLOT, new_stack, 0);
			return;
		}

		/* If there is any older write, wait for it */
		older_stack = mod_in_flight_write(mod, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for write %lld\n",
				stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_PREF_LOCK);
			return;
		}

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr, EV_MOD_PREF_ACTION, stack, stack->prefetch);
		new_stack->blocking = 1;
		new_stack->read = 1;
		new_stack->retry = stack->retry;
		new_stack->access_kind = mod_access_prefetch;
		new_stack->request_dir = mod_request_up_down;
		new_stack->pref_stream = stack->pref_stream;
		new_stack->pref_slot = stack->pref_slot;
		esim_schedule_event(EV_MOD_NMOESI_PREF_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_PREF_ACTION)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s pref action\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_action\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->prefetch_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_PREF_LOCK, stack, retry_lat);
			return;
		}

		/* Hit */
		if (stack->hit)
		{
			mem_debug("    will finish due to block already in cache (or write buffer)\n");
			mod->canceled_prefetches++; /* Statistics */
			mod->canceled_prefetches_cache_hit++;
			esim_schedule_event(EV_MOD_PREF_UNLOCK, stack, 0);
			return;
		}

		/* Stream hit -- Block already in the stream */
		if (stack->stream_hit)
		{
			mem_debug("     block is already in the stream ");
			if (stack->pref_slot == stack->src_pref_slot)
			{
				mem_debug("and in the correct slot\n");
				mod->canceled_prefetches++; /* Statistics */
				mod->canceled_prefetches_stream_hit++; /* Statistics */
				esim_schedule_event(EV_MOD_PREF_FINISH, stack, 0);
			}
			else
			{
				mem_debug("but not in the correct slot\n");
				esim_schedule_event(EV_MOD_PREF_MISS, stack, 0);
			}
			return;
		}

		/* Miss */
		new_stack = mod_stack_create(stack->id, mod, stack->tag,
			EV_MOD_PREF_MISS, stack, stack->prefetch);
		new_stack->peer = mod;
		new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
		new_stack->request_dir = mod_request_up_down;
		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
		return;
	}

	if (event == EV_MOD_PREF_MISS)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s pref miss\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_miss\"\n",
			stack->id, mod->name);

		/* Error on read request. Unlock block and retry prefetch */
		if (stack->err)
		{
			mod->prefetch_retries++;
			retry_lat = mod_get_retry_latency(mod);
			dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);
			pref->streams[stack->pref_stream].count--; //COUNT
			mem_debug("    pref lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_PREF_LOCK, stack, retry_lat);
			return;
		}

		cache_get_pref_block_data(cache, stack->pref_stream, stack->pref_slot, NULL, &stack->state);
		assert(!stack->state);

		/* Store block */
		cache_set_pref_block(cache, stack->pref_stream, stack->pref_slot, stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive);

		/* If block comes from another slot in the same stream invalidate the original one */
		if (stack->stream_hit)
			cache_set_pref_block(cache, stack->src_pref_stream, stack->src_pref_slot, 0, cache_block_invalid);

		/* Continue */
		esim_schedule_event(EV_MOD_PREF_UNLOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_PREF_UNLOCK)
	{
		mem_debug("  %lld %lld 0x%x %s pref unlock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_unlock\"\n",
			stack->id, mod->name);

		/* Unlock directory entry */
		dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);

		/* Block was in the stream */
		if (stack->stream_hit)
			dir_pref_entry_unlock(mod->dir, stack->src_pref_stream, stack->src_pref_slot);

		/* Statistics. They are collected here to avoid cancelled prefetches, that go directly to EV_MOD_NMOESI_PREFETCH_FINISH. */
		if(!stack->hit && !stack->stream_hit)
		{
			mod->completed_prefetches++;

			if (stack->client_info->late_prefetch)
			{
				mod->late_prefetches++;
				ctx->report_stack->late_prefs_per_level_int[mod->level]++;
			}

			/* Measure end-to-end latency */
			assert(stack->start_time != -1);
			ctx->report_stack->aggregate_pref_lat_per_level_int[mod->level] += esim_time - stack->start_time;
			ctx->report_stack->prefs_per_level_int[mod->level]++;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_PREF_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_PREF_FINISH)
	{
		mem_debug("%lld %lld 0x%x %s pref finish\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_finish\"\n",
			stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n",
			stack->id);

		/* Increment witness variable */
		if (stack->witness_ptr)
			(*stack->witness_ptr)++;

		/* Return event queue element into event queue */
		if (stack->event_queue && stack->event_queue_item)
			linked_list_add(stack->event_queue, stack->event_queue_item);

		/* Finish access */
		mod_access_finish(mod, stack);

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* When only remains one pending prefetch the stream tag is set */
		sb = &pref->streams[stack->pref_stream];
		assert(stack->pref_stream >= 0 && stack->pref_stream < pref->max_num_streams);
		assert(stack->pref_slot >= 0 && stack->pref_slot < pref->max_num_slots);
		assert(sb->pending_prefetches > 0);
		if (sb->pending_prefetches == 1)
		{
			sb->stream_tag = stack->addr & pref->stream_tag_mask;
			assert(sb->stream_tag == sb->stream_transcient_tag);
		}
		sb->pending_prefetches--;

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}

/* NMOESI Protocol */

void mod_handler_nmoesi_load(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct x86_ctx_t *ctx = stack->client_info->ctx;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(ctx);
	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_LOAD)
	{
		struct mod_stack_t *master_stack;

		mem_debug("%lld %lld 0x%x %s load\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.new_access name=\"A-%lld\" type=\"load\" "
			"state=\"%s:load\" addr=0x%x\n",
			stack->id, mod->name, stack->addr);

		/* Record access */
		mod_access_start(mod, stack, mod_access_load);

		/* Measure end-to-end latency */
		stack->start_time = esim_time;

		/* Add access to stride detector and record if there is a stride */
		if (pref && pref->type)
			stack->stride = cache_detect_stride(mod->cache, stack->addr);

		/* Coalesce access */
		master_stack = mod_can_coalesce(mod, mod_access_load, stack->addr, stack);
		if (master_stack)
		{
			mod->reads++;
			mod_coalesce(mod, master_stack, stack);

			if (master_stack->access_kind == mod_access_prefetch)
			{
				/* Statistics */
				mod->delayed_hits++;
				/* Prefetch stream stacks leave block in prefetch buffer, not in cache, so first load to coalesce must bring block from stream to cache.
				 * Next stacks must coalesce with the load, not with the prefetch. This is done in mod_coalesce(...). */
				if (prefetcher_uses_stream_buffers(pref))
				{
					mod_stack_wait_in_stack(stack, master_stack, EV_MOD_NMOESI_LOAD_LOCK);
					return;
				}
			}

			/* Si la master stack te el bit de coalesced pero no master stack, vol dir que ha fet coalesce amb un stream prefetch, per tant açò és un delayed hit */
			if(master_stack->coalesced)
				mod->delayed_hits++;

			mod_stack_wait_in_stack(stack, master_stack, EV_MOD_NMOESI_LOAD_FINISH);
			return;
		}

		/* Next event */
		esim_schedule_event(EV_MOD_NMOESI_LOAD_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_LOAD_LOCK)
	{
		struct mod_stack_t *older_stack;

		mem_debug("  %lld %lld 0x%x %s load lock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_lock\"\n",
			stack->id, mod->name);

		/* If there is any older write, wait for it */
		older_stack = mod_in_flight_write(mod, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for write %lld\n",
				stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_LOAD_LOCK);
			return;
		}

		/* If there is any older access to the same address that this access could not
		 * be coalesced with, wait for it. */
		older_stack = mod_in_flight_address(mod, stack->addr, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for access %lld\n",
				stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_LOAD_LOCK);
			return;
		}

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr,
			EV_MOD_NMOESI_LOAD_ACTION, stack, stack->prefetch);
		new_stack->blocking = 1;
		new_stack->read = 1;
		new_stack->retry = stack->retry;
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->background = stack->background;
		new_stack->request_dir = mod_request_up_down;
		new_stack->access_kind = mod_access_load;
		new_stack->event_queue_item = stack->event_queue_item;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_LOAD_ACTION)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s load action\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_action\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->read_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry |= 1 << mod->level;
			esim_schedule_event(EV_MOD_NMOESI_LOAD_LOCK, stack, retry_lat);
			return;
		}

		/* Fast resumed or hit in write buffer */
		if (stack->wb_hit || stack->fast_resume)
		{
			esim_schedule_event(EV_MOD_NMOESI_LOAD_FINISH, stack, 0);
			return;
		}

		/* Hit in cache */
		if (stack->state)
		{
			esim_schedule_event(EV_MOD_NMOESI_LOAD_UNLOCK, stack, 0);

			/* The prefetcher may have prefetched this earlier and hence
			 * this is a hit now. Let the prefetcher know of this hit
			 * since without the prefetcher, this may have been a miss. */
			prefetcher_cache_hit(stack, mod);
			return;
		}

		/* Hit in stream */
		if (stack->stream_hit)
		{
			esim_schedule_event(EV_MOD_NMOESI_LOAD_MISS, stack, 0);
			return;
		}

		/* Background stack moving block from stream to cache */
		if (stack->background)
		{
			esim_schedule_event(EV_MOD_NMOESI_LOAD_MISS, stack, 0);
			return;
		}

		/* Miss */
		new_stack = mod_stack_create(stack->id, mod, stack->tag,
			EV_MOD_NMOESI_LOAD_MISS, stack, stack->prefetch);
		new_stack->peer = mod_stack_set_peer(mod, stack->state);
		new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
		new_stack->retry = stack->retry; //VVV
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->request_dir = mod_request_up_down;
		new_stack->event_queue_item = stack->event_queue_item;
		new_stack->src_atd_hit = stack->atd_hit; /* An L1 ATD hit should not be an ATD hit in L2 */
		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);

		/* The prefetcher may be interested in this miss */
		prefetcher_cache_miss(stack, mod);

		return;
	}

	if (event == EV_MOD_NMOESI_LOAD_MISS)
	{
		int retry_lat;
		struct stream_buffer_t *sb;

		mem_debug("  %lld %lld 0x%x %s load miss\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_miss\"\n",
			stack->id, mod->name);

		/* Error on read request. Unlock block and retry load. */
		if (stack->err)
		{
			mod->read_retries++;
			retry_lat = mod_get_retry_latency(mod);
			dir_entry_unlock(mod->dir, stack->set, stack->way);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry |= 1 << mod->level;
			esim_schedule_event(EV_MOD_NMOESI_LOAD_LOCK, stack, retry_lat);
			return;
		}

		/* If fast resumed because prefetch hit, there are some pending memory operations, so moving the block from prefetch buffer to cache and unlocking directories must wait for them to complete. The background stack created when fast resumed this access will take care of moving the blocks. */
		if (stack->fast_resume)
		{
			esim_schedule_event(EV_MOD_NMOESI_LOAD_FINISH, stack, 0);
			return;
		}
		else if (stack->background)
		{
			struct write_buffer_block_t *block = (struct write_buffer_block_t *) -1;
			assert(linked_list_count(cache->wb.blocks));
			LINKED_LIST_FOR_EACH(cache->wb.blocks)
			{
				block = linked_list_get(cache->wb.blocks);
				if (block->tag == stack->tag)
				{
					cache_set_block(cache, stack->set, stack->way, stack->tag, block->state, stack->client_info);
					mod_stack_wake_up_write_buffer(block);
					linked_list_remove(cache->wb.blocks);
					free(block);
					block = NULL;
					break;
				}
			}
			assert(!block);
		}
		else if (stack->stream_hit)
		{
			int tag;
			cache_get_pref_block_data(cache, stack->pref_stream, stack->pref_slot, &tag, &stack->state);
			assert(stack->tag == tag);
			assert(stack->state);

			/* Write block in cache */
			cache_set_block(cache, stack->set, stack->way, tag, stack->state, stack->client_info);

			/* Free buffer entry */
			cache_set_pref_block(cache, stack->pref_stream, stack->pref_slot, -1, cache_block_invalid);
			sb = &pref->streams[stack->pref_stream];
			sb->count--; //COUNT
		}
		else
		{
			/* Set block state to excl/shared depending on return var 'shared'.
			* Also set the tag of the block. */
			cache_set_block(cache, stack->set, stack->way, stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive, stack->client_info);
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_LOAD_UNLOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_LOAD_UNLOCK)
	{
		mem_debug("  %lld %lld 0x%x %s load unlock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_unlock\"\n",
			stack->id, mod->name);

		/* For the ATD, the possibilities are:
		 * Cache hit - ATD hit ---> No problem
		 * Cache hit - ATD miss --> Problematic, set block in ATD after a delay?
		 * Cache miss - ATD hit --> Propagate hit to lower levels, do nothing
		 * Cache miss - ATD miss -> Just follow what the cache does */
		if (stack->hit && stack->atd_hit == 1)
		{
			/* Do nothing */
		}
		else if (stack->hit && stack->atd_hit == 0)
		{
			struct mod_t *low_mod = mod_get_low_mod(mod, stack->tag);
			assert(low_mod);
			new_stack = mod_stack_create(stack->id, mod, stack->tag,
				ESIM_EV_NONE, stack, 0);
			new_stack->target_mod = low_mod;
			esim_schedule_event(EV_MOD_ATD_DELAYED_SET, new_stack, low_mod->latency);
		}
		else if (stack->hit == 0 && stack->atd_hit == 1)
		{
			/* Do nothing */
		}
		else if (stack->hit == 0 && stack->atd_hit == 0)
		{
			atd_set_block(mod->atd_per_thread[thread_id], stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive);
		}

		/* Unlock directory entry */
		dir_entry_unlock(mod->dir, stack->set, stack->way);
		if (stack->stream_hit && !stack->background)
			dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);

		/* Impose the access latency before continuing */
		esim_schedule_event(EV_MOD_NMOESI_LOAD_FINISH, stack,
			mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_LOAD_FINISH)
	{
		mem_debug("%lld %lld 0x%x %s load finish\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_finish\"\n",
			stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n",
			stack->id);

		/* Background stacks don't have to do this */
		if (!stack->background)
		{
			/* Increment witness variable */
			if (stack->witness_ptr)
				(*stack->witness_ptr)++;

			/* Increment head */
			if(stack->stream_hit && stack->stream_head_hit)
			{
				struct stream_buffer_t *sb = &pref->streams[stack->pref_stream];
				sb->head = (stack->pref_slot + 1) % pref->max_num_slots; //HEAD
			}

			/* Return event queue element into event queue */
			if (stack->event_queue && stack->event_queue_item)
				linked_list_add(stack->event_queue, stack->event_queue_item);

			/* Measure end-to-end latency */
			assert(stack->start_time != -1);
			ctx->report_stack->aggregate_load_lat_int += esim_time - stack->start_time;
			ctx->report_stack->loads_int++;

			/* Finish access */
			mod_access_finish(mod, stack);
		}

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_store(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct x86_ctx_t *ctx = stack->client_info->ctx;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(ctx);
	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_STORE)
	{
		struct mod_stack_t *master_stack;

		mem_debug("%lld %lld 0x%x %s store\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.new_access name=\"A-%lld\" type=\"store\" "
			"state=\"%s:store\" addr=0x%x\n",
			stack->id, mod->name, stack->addr);

		/* Record access */
		mod_access_start(mod, stack, mod_access_store);

		/* Measure end-to-end latency */
		stack->start_time = esim_time;

		/* Add access to stride detector and record if there is a stride */
		if (pref && pref->type)
			stack->stride = cache_detect_stride(mod->cache, stack->addr);

		/* Coalesce access */
		master_stack = mod_can_coalesce(mod, mod_access_store, stack->addr, stack);
		if (master_stack)
		{
			mod->writes++;
			mod_coalesce(mod, master_stack, stack);
			mod_stack_wait_in_stack(stack, master_stack, EV_MOD_NMOESI_STORE_FINISH);

			/* Increment witness variable */
			if (stack->witness_ptr)
				(*stack->witness_ptr)++;

			return;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_STORE_LOCK, stack, 0);
		return;
	}


	if (event == EV_MOD_NMOESI_STORE_LOCK)
	{
		struct mod_stack_t *older_stack;

		mem_debug("  %lld %lld 0x%x %s store lock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:store_lock\"\n",
			stack->id, mod->name);

		/* If there is any older access, wait for it */
		older_stack = stack->access_list_prev;
		if (older_stack)
		{
			mem_debug("    %lld wait for access %lld\n",
				stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_STORE_LOCK);
			return;
		}

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr,
			EV_MOD_NMOESI_STORE_ACTION, stack, stack->prefetch);
		new_stack->blocking = 1;
		new_stack->write = 1;
		new_stack->retry = stack->retry;
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->witness_ptr = stack->witness_ptr;
		new_stack->request_dir = mod_request_up_down;
		new_stack->access_kind = mod_access_store;
		new_stack->event_queue_item = stack->event_queue_item;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);

		/* Set witness variable to NULL so that retries from the same
		 * stack do not increment it multiple times */
		stack->witness_ptr = NULL;

		return;
	}

	if (event == EV_MOD_NMOESI_STORE_ACTION)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s store action\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:store_action\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->write_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry |= 1 << mod->level;
			esim_schedule_event(EV_MOD_NMOESI_STORE_LOCK, stack, retry_lat);
			return;
		}

		/* ATD */
		if (stack->hit == 0 && stack->atd_hit == 1)
		{
			atd_set_block(mod->atd_per_thread[thread_id], stack->tag, cache_block_modified);
		}

		/* Fast resumed stack */
		if (stack->fast_resume)
		{
			/* Impose the access latency before continuing */
			esim_schedule_event(EV_MOD_NMOESI_STORE_FINISH, stack,
				mod->latency);
			return;
		}

		/* Background stack for a fast resumed one */
		else if (stack->background)
		{
			/* Write block from write buffer to cache */
			struct write_buffer_block_t *wb_block = (struct write_buffer_block_t *) -1; /* Used as canary */
			assert(linked_list_count(cache->wb.blocks));
			LINKED_LIST_FOR_EACH(cache->wb.blocks)
			{
				wb_block = linked_list_get(cache->wb.blocks);
				if (wb_block->tag == stack->tag)
				{
					cache_set_block(cache, stack->set, stack->way, stack->tag, wb_block->state, stack->client_info);
					stack->state = wb_block->state;
					mod_stack_wake_up_write_buffer(wb_block);
					linked_list_remove(cache->wb.blocks);
					free(wb_block);
					wb_block = NULL;
					break;
				}
			}
			assert(!wb_block);
		}

		/* Normal stack with a prefetch hit */
		else if (stack->stream_hit)
			cache_get_pref_block_data(mod->cache, stack->pref_stream, stack->pref_slot, NULL, &stack->state);

		/* Hit - state=M/E */
		if (stack->state == cache_block_modified ||
			stack->state == cache_block_exclusive)
		{
			esim_schedule_event(EV_MOD_NMOESI_STORE_UNLOCK, stack, 0);

			/* The prefetcher may have prefetched this earlier and hence
			 * this is a hit now. Let the prefetcher know of this hit
			 * since without the prefetcher, this may have been a miss. */
			prefetcher_cache_hit(stack, mod);

			return;
		}

		/* Miss - state=O/S/I/N */
		new_stack = mod_stack_create(stack->id, mod, stack->tag,
			EV_MOD_NMOESI_STORE_UNLOCK, stack, stack->prefetch);
		new_stack->peer = mod_stack_set_peer(mod, stack->state);
		new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
		new_stack->retry = stack->err; //VVV
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->request_dir = mod_request_up_down;
		new_stack->event_queue_item = stack->event_queue_item;
		new_stack->src_atd_hit = stack->atd_hit; /* An L1 ATD hit should not be an ATD hit in L2 */
		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST, new_stack, 0);

		/* The prefetcher may be interested in this miss */
		prefetcher_cache_miss(stack, mod);

		return;
	}

	if (event == EV_MOD_NMOESI_STORE_UNLOCK)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s store unlock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:store_unlock\"\n",
			stack->id, mod->name);

		/* Error in write request, unlock block and retry store. */
		if (stack->err)
		{
			mod->write_retries++;
			retry_lat = mod_get_retry_latency(mod);
			dir_entry_unlock(mod->dir, stack->set, stack->way);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry |= 1 << mod->level;
			esim_schedule_event(EV_MOD_NMOESI_STORE_LOCK, stack, retry_lat);
			return;
		}

		/* Update tag/state and unlock cache block */
		cache_set_block(mod->cache, stack->set, stack->way, stack->tag, cache_block_modified, stack->client_info);
		dir_entry_unlock(mod->dir, stack->set, stack->way);

		/* For the ATD, the possibilities are:
		 * Cache hit - ATD hit ---> No problem
		 * Cache hit - ATD miss --> Problematic, set block in ATD after a delay?
		 * Cache miss - ATD hit --> Propagate hit to lower levels, do nothing
		 * Cache miss - ATD miss -> Just follow what the cache does */
		if (stack->hit && stack->atd_hit == 1)
		{
			atd_set_block(mod->atd_per_thread[thread_id], stack->tag, cache_block_modified);
		}
		else if (stack->hit && stack->atd_hit == 0)
		{
			struct mod_t *low_mod = mod_get_low_mod(mod, stack->tag);
			assert(low_mod);
			new_stack = mod_stack_create(stack->id, mod, stack->tag,
				ESIM_EV_NONE, stack, 0);
			new_stack->target_mod = low_mod;
			esim_schedule_event(EV_MOD_ATD_DELAYED_SET, new_stack, low_mod->latency);
		}
		else if (stack->hit == 0 && stack->atd_hit == 1)
		{
			/* Already done */
		}
		else if (stack->hit == 0 && stack->atd_hit == 0)
		{
			atd_set_block(mod->atd_per_thread[thread_id], stack->tag, cache_block_modified);
		}

		/* Block comes from stream buffer */
		if (stack->stream_hit)
		{
			struct stream_buffer_t *sb;

			/* Free stream buffer entry */
			cache_set_pref_block(cache, stack->pref_stream, stack->pref_slot, block_invalid_tag, cache_block_invalid);
			sb = &pref->streams[stack->pref_stream];
			sb->count--; //COUNT

			/* Unlock stream buffer entry */
			assert(!stack->background);
			assert(!stack->fast_resume);
			dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);
		}

		/* Impose the access latency before continuing */
		esim_schedule_event(EV_MOD_NMOESI_STORE_FINISH, stack,
			mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_STORE_FINISH)
	{
		mem_debug("%lld %lld 0x%x %s store finish\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:store_finish\"\n",
			stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n",
			stack->id);

		/* Return event queue element into event queue */
		if (stack->event_queue && stack->event_queue_item)
			linked_list_add(stack->event_queue, stack->event_queue_item);

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* Measure end-to-end latency */
		assert(stack->start_time != -1);
		ctx->report_stack->aggregate_store_lat_int += esim_time - stack->start_time;
		ctx->report_stack->stores_int++;

		/* Finish access */
		mod_access_finish(mod, stack);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}

void mod_handler_nmoesi_nc_store(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;

	struct x86_ctx_t *ctx = stack->client_info->ctx;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(ctx);
	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_NC_STORE)
	{
		struct mod_stack_t *master_stack;

		mem_debug("%lld %lld 0x%x %s nc store\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.new_access name=\"A-%lld\" type=\"nc_store\" "
			"state=\"%s:nc store\" addr=0x%x\n", stack->id, mod->name, stack->addr);

		/* Record access */
		mod_access_start(mod, stack, mod_access_nc_store);

		/* Coalesce access */
		master_stack = mod_can_coalesce(mod, mod_access_nc_store, stack->addr, stack);
		if (master_stack)
		{
			mod->nc_writes++;
			mod_coalesce(mod, master_stack, stack);
			mod_stack_wait_in_stack(stack, master_stack, EV_MOD_NMOESI_NC_STORE_FINISH);
			return;
		}

		/* Next event */
		esim_schedule_event(EV_MOD_NMOESI_NC_STORE_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_LOCK)
	{
		struct mod_stack_t *older_stack;

		mem_debug("  %lld %lld 0x%x %s nc store lock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_lock\"\n",
			stack->id, mod->name);

		/* If there is any older write, wait for it */
		older_stack = mod_in_flight_write(mod, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for write %lld\n", stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_NC_STORE_LOCK);
			return;
		}

		/* If there is any older access to the same address that this access could not
		 * be coalesced with, wait for it. */
		older_stack = mod_in_flight_address(mod, stack->addr, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for write %lld\n", stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_NC_STORE_LOCK);
			return;
		}

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr,
			EV_MOD_NMOESI_NC_STORE_WRITEBACK, stack, stack->prefetch);
		new_stack->blocking = 1;
		new_stack->nc_write = 1;
		new_stack->retry = stack->retry;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_WRITEBACK)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s nc store writeback\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_writeback\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->nc_write_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_NMOESI_NC_STORE_LOCK, stack, retry_lat);
			return;
		}

		/* If the block has modified data, evict it so that the lower-level cache will
		 * have the latest copy */
		if (stack->state == cache_block_modified || stack->state == cache_block_owned)
		{
			stack->eviction = 1;
			new_stack = mod_stack_create(stack->id, mod, 0,
				EV_MOD_NMOESI_NC_STORE_ACTION, stack, stack->prefetch);
			new_stack->set = stack->set;
			new_stack->way = stack->way;
			esim_schedule_event(EV_MOD_NMOESI_EVICT, new_stack, 0);
			return;
		}

		esim_schedule_event(EV_MOD_NMOESI_NC_STORE_ACTION, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_ACTION)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s nc store action\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_action\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->nc_write_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_NMOESI_NC_STORE_LOCK, stack, retry_lat);
			return;
		}

		/* Main memory modules are a special case */
		if (mod->kind == mod_kind_main_memory)
		{
			/* For non-coherent stores, finding an E or M for the state of
			 * a cache block in the directory still requires a message to
			 * the lower-level module so it can update its owner field.
			 * These messages should not be sent if the module is a main
			 * memory module. */
			esim_schedule_event(EV_MOD_NMOESI_NC_STORE_UNLOCK, stack, 0);
			return;
		}

		atd_set_block(mod->atd_per_thread[thread_id], stack->tag, cache_block_noncoherent);

		/* N/S are hit */
		if (stack->state == cache_block_shared || stack->state == cache_block_noncoherent)
		{
			esim_schedule_event(EV_MOD_NMOESI_NC_STORE_UNLOCK, stack, 0);
		}
		/* E state must tell the lower-level module to remove this module as an owner */
		else if (stack->state == cache_block_exclusive)
		{
			new_stack = mod_stack_create(stack->id, mod, stack->tag,
				EV_MOD_NMOESI_NC_STORE_MISS, stack, stack->prefetch);
			new_stack->message = message_clear_owner;
			new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
			esim_schedule_event(EV_MOD_NMOESI_MESSAGE, new_stack, 0);
		}
		/* Modified and Owned states need to call read request because we've already
		 * evicted the block so that the lower-level cache will have the latest value
		 * before it becomes non-coherent */
		else
		{
			new_stack = mod_stack_create(stack->id, mod, stack->tag,
				EV_MOD_NMOESI_NC_STORE_MISS, stack, stack->prefetch);
			new_stack->peer = mod_stack_set_peer(mod, stack->state);
			new_stack->nc_write = 1;
			new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
			new_stack->request_dir = mod_request_up_down;
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
		}

		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_MISS)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s nc store miss\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_miss\"\n",
			stack->id, mod->name);

		/* Error on read request. Unlock block and retry nc store. */
		if (stack->err)
		{
			mod->nc_write_retries++;
			retry_lat = mod_get_retry_latency(mod);
			dir_entry_unlock(mod->dir, stack->set, stack->way);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_NMOESI_NC_STORE_LOCK, stack, retry_lat);
			return;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_NC_STORE_UNLOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_UNLOCK)
	{
		mem_debug("  %lld %lld 0x%x %s nc store unlock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_unlock\"\n",
			stack->id, mod->name);

		/* Set block state to excl/shared depending on return var 'shared'.
		 * Also set the tag of the block. */
		cache_set_block(mod->cache, stack->set, stack->way, stack->tag, cache_block_noncoherent, stack->client_info);

		/* Unlock directory entry */
		dir_entry_unlock(mod->dir, stack->set, stack->way);

		/* Impose the access latency before continuing */
		esim_schedule_event(EV_MOD_NMOESI_NC_STORE_FINISH, stack,
			mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_NC_STORE_FINISH)
	{
		mem_debug("%lld %lld 0x%x %s nc store finish\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:nc_store_finish\"\n",
			stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n",
			stack->id);

		/* Increment witness variable */
		if (stack->witness_ptr)
			(*stack->witness_ptr)++;

		/* Return event queue element into event queue */
		if (stack->event_queue && stack->event_queue_item)
			linked_list_add(stack->event_queue, stack->event_queue_item);

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* Finish access */
		mod_access_finish(mod, stack);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_pref_find_and_lock(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;

	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb;

	assert(stack->request_dir);
	assert(stack->access_kind);

	if (event == EV_MOD_NMOESI_PREF_FIND_AND_LOCK)
	{
		mem_debug("  %lld %lld 0x%x %s pref find and lock (blocking=%d)\n", esim_time, stack->id, stack->addr, mod->name, stack->blocking);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_find_and_lock\"\n", stack->id, mod->name);

		/* Look for block in write buffer */
		struct write_buffer_block_t *block;
		stack->tag = stack->addr & ~cache->block_mask;
		LINKED_LIST_FOR_EACH(cache->wb.blocks)
		{
			block = linked_list_get(cache->wb.blocks);
			assert(block->state == cache_block_exclusive || block->state == cache_block_shared);
			if (block->tag == stack->tag)
			{
				stack->state = block->state;
				mem_debug("    %lld 0x%x %s write buffer hit: pos=%d, state=%s\n",
					stack->id, stack->tag, mod->name, linked_list_current(cache->wb.blocks),
					str_map_value(&cache_block_state_map, stack->state));
				stack->hit = 1;
				stack->wb_hit = 1;
				mod->write_buffer_prefetch_hits++; /* Statistics */
			}
		}

		/* Default return values */
		ret->err = 0;

		/* Get a port */
		mod_lock_port(mod, stack, EV_MOD_NMOESI_PREF_FIND_AND_LOCK_PORT);
		return;
	}

	if (event == EV_MOD_NMOESI_PREF_FIND_AND_LOCK_PORT)
	{
		struct mod_port_t *port = stack->port;
		struct dir_lock_t *dir_lock;
		struct stream_block_t *sblock;
		int slot;

		assert(stack->port);
		mem_debug("  %lld %lld 0x%x %s pref find and lock port\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_find_and_lock_port\"\n", stack->id, mod->name);

		/* Set parent stack flag expressing that port has already been locked.
		 * This flag is checked by new writes to find out if it is already too
		 * late to coalesce. */
		ret->port_locked = 1;

		/* Search block in cache and stream */
		if (stack->access_kind != mod_access_invalidate_slot && !stack->wb_hit)
		{
			sb = &pref->streams[stack->pref_stream];

			/* Search in cache */
			stack->hit = mod_find_block(mod, stack->addr, &stack->set, &stack->way, &stack->tag, &stack->state);

			/* Block in cache */
			if (stack->hit)
				mem_debug("    %lld 0x%x %s hit: set=%d, way=%d, state=%s\n", stack->id, stack->tag, mod->name, stack->set, stack->way, str_map_value(&cache_block_state_map, stack->state));

			/* Block is not in cache */
			else
			{
				/* Block may be in this stream */
				if (sb->stream_tag == sb->stream_transcient_tag)
				{
					slot = mod_find_block_in_stream(mod, stack->addr, stack->pref_stream);

					/* Block found */
					if (slot > -1)
					{
						/* Block already in the correct slot. Do nothing. */
						if (slot == stack->pref_slot)
						{
							ret->stream_hit = 1;
							ret->src_pref_slot = stack->pref_slot;
							ret->src_pref_stream = stack->pref_stream;
							mod_unlock_port(mod, port, stack);
							mod_stack_return(stack);
							return;
						}

						/* Block is not in the correct slot */ //TODO: Posible condició de carrera
						else
						{
							/* Lock it if is unlocked */
							dir_lock = dir_pref_lock_get(mod->dir, sb->stream, slot);
							if (!dir_lock->lock)
							{
								dir_pref_entry_lock(mod->dir, sb->stream, slot, EV_MOD_NMOESI_PREF_FIND_AND_LOCK, stack);
								sblock = cache_get_pref_block(cache, sb->stream, slot);
								sblock->transient_tag = -1;
								ret->stream_hit = 1;
								ret->src_pref_slot = slot;
								ret->src_pref_stream = stack->pref_stream;
							}
						}
					}
				}
			}
		}

		/* Assertions */
		assert(stack->pref_stream >= 0 && stack->pref_stream < pref->max_num_streams);
		assert(stack->pref_slot >= 0 && stack->pref_slot < pref->max_num_slots);
		sb = &pref->streams[stack->pref_stream];
	
		/* Lock prefetch entry */
		dir_lock = dir_pref_lock_get(mod->dir, stack->pref_stream, stack->pref_slot);
		if (dir_lock->lock && !stack->blocking)
		{
			struct stream_block_t * block = cache_get_pref_block(cache, stack->pref_stream, stack->pref_slot);
			mem_debug("    %lld 0x%x %s pref_stream=%d pref_slot=%d containing 0x%x (0x%x) already locked by stack %lld, retrying...\n", stack->id, stack->tag, mod->name, stack->pref_stream, stack->pref_slot, block->tag, block->transient_tag, dir_lock->locking_stack->id);
			ret->err = 1;
			mod_unlock_port(mod, port, stack);
			mod_stack_return(stack);
			return;
		}
		if (!dir_pref_entry_lock(mod->dir, stack->pref_stream, stack->pref_slot, EV_MOD_NMOESI_PREF_FIND_AND_LOCK, stack))
		{
			mem_debug("    %lld 0x%x %s pref_stream=%d pref_slot=%d already locked by stack %lld, waiting...\n", stack->id, stack->tag, mod->name, stack->pref_stream, stack->pref_slot, dir_lock->locking_stack->id);
			mod_unlock_port(mod, port, stack);
			return;
		}

		/* Entry is locked. Record the transient tag so that a subsequent lookup
		 * detects that the block is being brought. */
		struct stream_block_t *block = cache_get_pref_block(cache, stack->pref_stream, stack->pref_slot);
		block->transient_tag = !stack->hit && stack->access_kind != mod_access_invalidate_slot ?
			stack->tag : -1; /* If cache hit, block will be removed */

		mem_debug("    %lld 0x%x %s stream=%d, slot=%d, state=%s\n", stack->id, stack->tag, mod->name, stack->pref_stream, stack->pref_slot, str_map_value(&cache_block_state_map, block->state));

		/* Update count */
		if (stack->access_kind != mod_access_invalidate_slot)
		{
			assert(sb->stream == stack->pref_stream);
			if (!block->state && !stack->hit && !ret->stream_hit)
				sb->count++; //COUNT
		}

		/* Update LRU stream */
		cache_access_stream(mod->cache, stack->pref_stream);

		/* Access latency */
		esim_schedule_event(EV_MOD_NMOESI_PREF_FIND_AND_LOCK_ACTION, stack, mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_PREF_FIND_AND_LOCK_ACTION)
	{
		struct mod_port_t *port = stack->port;

		assert(port);
		mem_debug("  %lld %lld 0x%x %s pref find and lock action\n", esim_time, stack->id, stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_find_and_lock_action\"\n", stack->id, mod->name);

		/* Release port */
		mod_unlock_port(mod, port, stack);

		/* Prefetch buffer slot eviction */
		struct stream_block_t *block = cache_get_pref_block(cache, stack->pref_stream, stack->pref_slot);
		if (block->state)
		{
			/* Silent eviction */
			block->state = cache_block_invalid;
			stack->pref_eviction = 1;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_PREF_FIND_AND_LOCK_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_PREF_FIND_AND_LOCK_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s pref find and lock finish (err=%d)\n", esim_time, stack->id, stack->tag, mod->name, stack->err);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:pref_find_and_lock_finish\"\n", stack->id, mod->name);

		/* If evict produced err, return err */ //TODO: Deprecated
		if (stack->err)
		{
			assert(stack->pref_eviction);
			cache_get_pref_block_data(mod->cache, stack->pref_stream, stack->pref_slot, NULL, &stack->state);
			assert(stack->state);
			ret->err = 1;
			dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);
			mod_stack_return(stack);
			return;
		}

		/* Eviction */
		if (stack->pref_eviction)
		{
			mod->stream_evictions++;
			/* Block is removed, not replaced */
			if (stack->hit || stack->access_kind == mod_access_invalidate_slot)
				pref->streams[stack->src_pref_stream].count--; //COUNT
			cache_get_pref_block_data(mod->cache, stack->pref_stream, stack->pref_slot, NULL, &stack->state);
			assert(!stack->state);
		}

		/* Return */
		ret->err = 0;
		ret->state = stack->state;
		ret->tag = stack->tag;
		ret->hit = stack->hit;
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_prefetch(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct x86_ctx_t *ctx = stack->client_info->ctx;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(ctx);
	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_PREFETCH)
	{
		struct mod_stack_t *master_stack;

		mem_debug("%lld %lld 0x%x %s prefetch\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.new_access name=\"A-%lld\" type=\"store\" "
			"state=\"%s:prefetch\" addr=0x%x\n",
			stack->id, mod->name, stack->addr);

		/* Record access */
		mod_access_start(mod, stack, mod_access_prefetch);

		/* Measure end-to-end latency */
		stack->start_time = esim_time;

		/* Statistics */
		mod->programmed_prefetches++;

		/* Coalesce access */
		master_stack = mod_can_coalesce(mod, mod_access_prefetch, stack->addr, stack);
		if (master_stack)
		{
			/* Doesn't make sense to prefetch as the block is already being fetched */
			mem_debug("  %lld %lld 0x%x %s useless prefetch - already being fetched\n",
					esim_time, stack->id, stack->addr, mod->name);

			mod->canceled_prefetches++;
			mod->canceled_prefetches_coalesce++;

			esim_schedule_event(EV_MOD_NMOESI_PREFETCH_FINISH, stack, 0);

			/* Increment witness variable */
			if (stack->witness_ptr)
				(*stack->witness_ptr)++;

			return;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_PREFETCH_LOCK, stack, 0);
		return;
	}


	if (event == EV_MOD_NMOESI_PREFETCH_LOCK)
	{
		struct mod_stack_t *older_stack;

		mem_debug("  %lld %lld 0x%x %s prefetch lock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:prefetch_lock\"\n",
			stack->id, mod->name);

		/* If there is any older write, wait for it */
		older_stack = mod_in_flight_write(mod, stack);
		if (older_stack)
		{
			mem_debug("    %lld wait for write %lld\n",
				stack->id, older_stack->id);
			mod_stack_wait_in_stack(stack, older_stack, EV_MOD_NMOESI_PREFETCH_LOCK);
			return;
		}

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr,
			EV_MOD_NMOESI_PREFETCH_ACTION, stack, stack->prefetch);
		new_stack->blocking = 0;
		new_stack->prefetch = 1;
		new_stack->retry = 0;
		new_stack->witness_ptr = stack->witness_ptr;
		new_stack->request_dir = mod_request_up_down;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);

		/* Set witness variable to NULL so that retries from the same
		 * stack do not increment it multiple times */
		stack->witness_ptr = NULL;

		return;
	}

	if (event == EV_MOD_NMOESI_PREFETCH_ACTION)
	{
		mem_debug("  %lld %lld 0x%x %s prefetch action\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:prefetch_action\"\n",
			stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			/* Don't want to ever retry prefetches if getting a lock failed.
			Effectively this means that prefetches are of low priority.
			This can be improved to not retry only when the current lock
			holder is writing to the block. */
			mod->canceled_prefetches++;
			mod->canceled_prefetches_retry++;
			mem_debug("    lock error, aborting prefetch\n");
			esim_schedule_event(EV_MOD_NMOESI_PREFETCH_FINISH, stack, 0);
			return;
		}

		atd_set_block(mod->atd_per_thread[thread_id], stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive);

		/* Hit */
		if (stack->state)
		{
			/* Block already in cache */
			mem_debug("  %lld %lld 0x%x %s useless prefetch - cache hit\n",
					esim_time, stack->id, stack->addr, mod->name);

			/* Statistics */
			mod->canceled_prefetches++;
			mod->canceled_prefetches_cache_hit++;

			esim_schedule_event(EV_MOD_NMOESI_PREFETCH_UNLOCK, stack, 0);
			return;
		}

		/* Miss */
		new_stack = mod_stack_create(stack->id, mod, stack->tag,
			EV_MOD_NMOESI_PREFETCH_MISS, stack, stack->prefetch);
		new_stack->peer = mod_stack_set_peer(mod, stack->state);
		new_stack->target_mod = mod_get_low_mod(mod, stack->tag);
		new_stack->request_dir = mod_request_up_down;
		new_stack->prefetch = 1;
		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_PREFETCH_MISS)
	{
		mem_debug("  %lld %lld 0x%x %s prefetch miss\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:prefetch_miss\"\n",
			stack->id, mod->name);

		/* Error on read request. Unlock block and abort. */
		if (stack->err)
		{
			/* Don't want to ever retry prefetches if read request failed.
			 * Effectively this means that prefetches are of low priority.
			 * This can be improved depending on the reason for read request fail */
			mod->canceled_prefetches++;
			mod->canceled_prefetches_retry++;
			dir_entry_unlock(mod->dir, stack->set, stack->way);
			mem_debug("    lock error, aborting prefetch\n");
			esim_schedule_event(EV_MOD_NMOESI_PREFETCH_FINISH, stack, 0);
			return;
		}

		/* Set block state to excl/shared depending on return var 'shared'.
		 * Also set the tag of the block. */
		cache_set_block(mod->cache, stack->set, stack->way, stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive, stack->client_info);
	/* Mark the prefetched block as prefetched. This is needed to let the
		 * prefetcher know about an actual access to this block so that it
		 * is aware of all misses as they would be without the prefetcher.
		 * TODO: The lower caches that will be filled because of this prefetch
		 * do not know if it was a prefetch or not. Need to have a way to mark
		 * them as prefetched too. */
		mod_set_prefetched_bit(mod, stack->addr, 1);

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_PREFETCH_UNLOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_PREFETCH_UNLOCK)
	{
		mem_debug("  %lld %lld 0x%x %s prefetch unlock\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:prefetch_unlock\"\n",
			stack->id, mod->name);

		/* Unlock directory entry */
		dir_entry_unlock(mod->dir, stack->set, stack->way);

		/* Statistics. They are collected here to avoid cancelled prefetches, that go directly to EV_MOD_NMOESI_PREFETCH_FINISH. */
		if(!stack->hit && !stack->stream_hit)
		{
			mod->completed_prefetches++;

			if (stack->client_info->late_prefetch)
			{
				mod->late_prefetches++;
				ctx->report_stack->late_prefs_per_level_int[mod->level]++;
			}

			/* Measure end-to-end latency */
			assert(stack->start_time != -1);
			ctx->report_stack->aggregate_pref_lat_per_level_int[mod->level] += esim_time - stack->start_time;
			ctx->report_stack->prefs_per_level_int[mod->level]++;
		}

		esim_schedule_event(EV_MOD_NMOESI_PREFETCH_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_PREFETCH_FINISH)
	{
		mem_debug("%lld %lld 0x%x %s prefetch\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:prefetch_finish\"\n",
			stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n",
			stack->id);

		/* Increment witness variable */
		if (stack->witness_ptr)
			(*stack->witness_ptr)++;

		/* Return event queue element into event queue */
		if (stack->event_queue && stack->event_queue_item)
			linked_list_add(stack->event_queue, stack->event_queue_item);

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* Finish access */
		mod_access_finish(mod, stack);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}

void mod_handler_nmoesi_find_and_lock(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;
	struct mod_stack_t *new_stack;
	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct x86_ctx_t *ctx = stack->client_info->ctx;
	/*RTM Hugo*/
	int hit = 1;
	int hit_set = 0;
	int miss_set = 0;


	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(ctx);
	assert(stack->request_dir);
	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_FIND_AND_LOCK)
	{
		mem_debug("  %lld %lld 0x%x %s find and lock (blocking=%d)\n",
			esim_time, stack->id, stack->addr, mod->name, stack->blocking);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:find_and_lock\"\n",
			stack->id, mod->name);

		/* Default return values */
		ret->err = 0;

		/* If block is in write buffer and request dir is up down, retry. Else, wait. */
		if (!stack->background &&!(mod->kind==mod_kind_main_memory && stack->request_dir == mod_request_up_down))
		{
			struct write_buffer_block_t *block;
			stack->tag = stack->addr & ~cache->block_mask;
			LINKED_LIST_FOR_EACH(cache->wb.blocks)
			{
				block = linked_list_get(cache->wb.blocks);
				assert(block->state == cache_block_exclusive || block->state == cache_block_shared);
				if (block->tag == stack->tag)
				{
					stack->state = block->state;
					if (stack->read)
					{
						mem_debug("    %lld 0x%x %s write buffer hit: pos=%d, state=%s\n",
							stack->id, stack->tag, mod->name, linked_list_current(cache->wb.blocks),
							str_map_value(&cache_block_state_map, stack->state));
						mod->write_buffer_read_hits++; /* Statistics */
					}
					else if (stack->write)
					{
						mem_debug("    %lld 0x%x %s write buffer hit: pos=%d, state=%s\n",
							stack->id, stack->tag, mod->name, linked_list_current(cache->wb.blocks),
							str_map_value(&cache_block_state_map, stack->state));
						mod->write_buffer_write_hits++; /* Statistics */
					}
					else
						fatal("Unknown memory operation type");

					if(stack->request_dir == mod_request_up_down)
					{
						mem_debug("    %lld retry in wb due to stack %lld\n", stack->id, block->stack_id);
						ret->err = 1;
						mod_stack_return(stack);
					}
					else
					{
						mem_debug("    %lld wait in wb for stack %lld\n",
							stack->id, block->stack_id);
						mod_stack_wait_in_write_buffer(stack, block, EV_MOD_NMOESI_FIND_AND_LOCK);

					}

					return;
				}
			}
		}

		/* If this stack has already been assigned a way, keep using it */
		stack->way = ret->way;
		stack->atd_way = ret->atd_way;

		/* Get a port */
		mod_lock_port(mod, stack, EV_MOD_NMOESI_FIND_AND_LOCK_PORT);

		stack->hit = stack->stream_hit = 0;
		return;
	}

	if (event == EV_MOD_NMOESI_FIND_AND_LOCK_PORT)
	{
		struct mod_port_t *port = stack->port;
		struct dir_lock_t *dir_lock;
		//struct cache_block_t *block_lru;
		int aux = 0;

		assert(stack->port);
		mem_debug("  %lld %lld 0x%x %s find and lock port\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:find_and_lock_port\"\n",
			stack->id, mod->name);

		/* Set parent stack flag expressing that port has already been locked.
		 * This flag is checked by new writes to find out if it is already too
		 * late to coalesce. */
		ret->port_locked = 1;

		/* Look for block */
		stack->hit = mod_find_block(mod, stack->addr, &stack->set, &stack->way, &stack->tag, &stack->state);
		
		/* Si es hit y mod->level == 1 */
		/* Comprobar si lru */
		/* ctx->l1_lru_hits++ */
		/* cache_get_block  2*/
		/* blk = mod->cache->sets[stack->set].ways[stack->way] */ 
		if (stack->hit)
		{	
			//Adding the set to my var Hugo
			hit_set = stack->set;
		}

		if (stack->request_dir == mod_request_up_down)
		{
			stack->atd_hit = atd_find_block(mod->atd_per_thread[thread_id], stack->addr, NULL, NULL, NULL, NULL);
			if (stack->src_atd_hit > 0)
				atd_access_block(mod->atd_per_thread[thread_id], stack->tag);
		}

		/* Debug */
		if (stack->hit)
			mem_debug("    %lld 0x%x %s hit: set=%d, way=%d, state=%s\n", stack->id,
				stack->tag, mod->name, stack->set, stack->way,
				str_map_value(&cache_block_state_map, stack->state));

		/* If there is a background stack, no other stack can be retrieving the same block */
		assert(!(stack->background && stack->hit));

		/* Statistics */
		mod->accesses++;

		if (stack->read)
		{
			mod->reads++;
			mod->effective_reads++;
			stack->blocking ? mod->blocking_reads++ : mod->non_blocking_reads++;
			if (stack->hit)
				mod->read_hits++;
		}
		else if (stack->prefetch)
		{
			mod->prefetches++;
		}
		else if (stack->nc_write)  /* Must go after read */
		{
			mod->nc_writes++;
			mod->effective_nc_writes++;
			stack->blocking ? mod->blocking_nc_writes++ : mod->non_blocking_nc_writes++;
			if (stack->hit)
				mod->nc_write_hits++;
		}
		else if (stack->write)
		{
			mod->writes++;
			mod->effective_writes++;
			stack->blocking ? mod->blocking_writes++ : mod->non_blocking_writes++;

			/* Increment witness variable when port is locked */
			if (stack->witness_ptr)
			{
				(*stack->witness_ptr)++;
				stack->witness_ptr = NULL;
			}

			if (stack->hit)
				mod->write_hits++;
		}
		else if (stack->message)
		{
			/* FIXME */
		}
		else
		{
			fatal("Unknown memory operation type");
		}

		if (!(stack->retry & (1 << mod->level)))
		{
			mod->no_retry_accesses++;
			if (stack->hit)
				mod->no_retry_hits++;

			if (stack->read)
			{
				mod->no_retry_reads++;
				if (stack->hit)
					mod->no_retry_read_hits++;
			}
			else if (stack->nc_write)  /* Must go after read */
			{
				mod->no_retry_nc_writes++;
				if (stack->hit)
					mod->no_retry_nc_write_hits++;
			}
			else if (stack->write)
			{
				mod->no_retry_writes++;
				if (stack->hit)
					mod->no_retry_write_hits++;
			}
			else if (stack->prefetch)
			{
				/* No retries currently for prefetches */
			}
			else if (stack->message)
			{
				/* FIXME */
			}
			else
			{
				fatal("Unknown memory operation type");
			}
		}

		if (!stack->hit)
		{
			/* Find victim */
			if (stack->way < 0)
				stack->way = cache_replace_block(cache, stack->set, stack->client_info);
			//RTM Hugo
			miss_set = stack->set;
			cache_get_block(mod->cache, stack->set, stack->way, NULL, &stack->state);
			assert(stack->state || !dir_entry_group_shared_or_owned(mod->dir, stack->set, stack->way));
			mem_debug("    %lld 0x%x %s lru: set=%d, way=%d, state=%s\n", stack->id, stack->tag, mod->name, stack->set, stack->way, str_map_value(&cache_block_state_map, stack->state));
		}
		assert(stack->way >= 0);
		if (stack->hit || stack->request_dir == mod_request_up_down )
		{
			/* If directory entry is locked and the call to FIND_AND_LOCK is not
			 * blocking, release port and return error. */
			dir_lock = dir_lock_get(mod->dir, stack->set, stack->way);
			if (dir_lock->lock && !stack->blocking)
			{
				mem_debug("    %lld 0x%x %s block already locked at set=%d, way=%d by A-%lld - aborting\n",
					stack->id, stack->tag, mod->name, stack->set, stack->way, dir_lock->locking_stack->id);

				mod_unlock_port(mod, port, stack);

				/* Interval statistics */
				if (should_count_stats(stack))
				{
					int core = stack->client_info->core;
					int thread = stack->client_info->thread;
					int pos = core * x86_cpu_num_threads + thread;
					mod->retries++;
					mod->report_stack->retries_per_thread_int[pos]++;
					ctx->report_stack->retries_per_level_int[mod->level]++;
					X86_THREAD.report_stack->retries_per_level_int[mod->level]++;
					/* If the stack locking the block is a prefetch that is bringing the block, then count that prefetch as a late prefetch */
					if (dir_lock->locking_stack->prefetch)
					{
						struct mod_stack_t *locking_stack = dir_lock->locking_stack;
						assert(locking_stack);
						assert(locking_stack->client_info);
						assert(dir_lock->locking_stack_id == locking_stack->id);
						locking_stack->client_info->late_prefetch = 1;
					}
				}

				ret->err = 1;
				ret->port_locked = 0;
				mod_stack_return(stack);
				return;
			}

			/* Lock directory entry. If lock fails, port needs to be released to prevent
			 * deadlock.  When the directory entry is released, locking port and
			 * directory entry will be retried. */
			if (!dir_entry_lock(mod->dir, stack->set, stack->way, EV_MOD_NMOESI_FIND_AND_LOCK,
				stack))
			{
				mem_debug("    %lld 0x%x %s block already locked at set=%d, way=%d by A-%lld - waiting\n",
					stack->id, stack->tag, mod->name, stack->set, stack->way, dir_lock->locking_stack->id);
				mod_unlock_port(mod, port, stack);
				ret->port_locked = 0;
				return;
			}

			/* This stack has been retried because the block it was looking for was locked in the
			 * stream and now has found the block in cache. Delayed hit statistics must be updated. */
			if(stack->hit && stack->stream_retried)
			{
				assert(stack->stream_retried_cycle);
				mod->delayed_hit_cycles += esim_cycle() - stack->stream_retried_cycle;
				mod->delayed_hits_cycles_counted++;
				ret->stream_retried = 0;
			}

			/* Cache entry is locked. Record the transient tag so that a subsequent lookup
			* detects that the block is being brought.
			* Also, update LRU counters here. */
			cache_set_transient_tag(mod->cache, stack->set, stack->way, stack->tag, stack->client_info);
			cache_access_block(mod->cache, stack->set, stack->way);
		}

		/* Access latency */
		if (!stack->hit && !stack->background && prefetcher_uses_stream_buffers(pref))
			esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK_PREF_STREAM, stack, 0); /* TODO: Zero? */
		else
			esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK_ACTION, stack, mod->dir_latency); /* Access latency */
		/* Calculating header penalty, Hugo */
                if(mod->level == 1 && mod->RTM  ){
                                if(hit){
					
                                        aux = hit_set - mod->mod_last_used_set->last_used_set[stack->way];
                                        aux = (aux>0)?(aux):(-aux);
					mod->mod_last_used_set->last_used_set[stack->way] = hit_set;
					mod->mod_last_used_set->added_cycles[stack->way][aux]++;
					
                                        //mod->mod_last_used_set->last_used_set  = hit_set;
                                       // mod->mod_last_used_set->added_cycles[aux]++; 
                                }
                                else{
                                        aux = miss_set - mod->mod_last_used_set->last_used_set[stack->way];
                                        aux = (aux>0)?(aux):(-aux);
                                        mod->mod_last_used_set->last_used_set[stack->way] = miss_set;
                                        mod->mod_last_used_set->added_cycles[stack->way][aux]++;

                                }
                     
                        }
		return;
	}
	                      
		 		



	if (event == EV_MOD_NMOESI_FIND_AND_LOCK_PREF_STREAM)
	{
		int result;
		struct mod_port_t *port = stack->port;
		struct stream_block_t *block;
		struct stream_buffer_t *sb;
		struct dir_lock_t *dir_lock;

		mem_debug("  %lld %lld 0x%x %s find and lock pref stream\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:find_and_lock_pref_stream\"\n", stack->id, mod->name);

		/* Look for block in prefetch stream buffers */
		stack->stream_hit = 0;
		result = mod_find_pref_block(mod, stack->addr, &stack->pref_stream, &stack->pref_slot);
		if (result)
		{
			sb = &pref->streams[stack->pref_stream];
			stack->stream_hit = 1;
			if (result == 1) /* {0 miss, 1 head hit, 2 middle stream hit} */
				stack->stream_head_hit = 1;
			mem_debug("    %lld 0x%x %s stream_hit=%d stream=%d slot=%d in head=%d up_down=%d pending_prefetches=%d\n", stack->id, stack->tag, mod->name, stack->stream_hit, stack->pref_stream, stack->pref_slot, stack->stream_head_hit, stack->request_dir == mod_request_up_down, sb->pending_prefetches);

			/* Print debug info */
			int i, slot, count;
			count = sb->head + pref->max_num_slots;
			for (i = sb->head; i < count; i++)
			{
				slot = i % pref->max_num_slots;
				block = &pref->streams[sb->stream].blocks[slot];
				dir_lock = dir_pref_lock_get(mod->dir, sb->stream, slot);
				mem_debug("\t\t{slot=%d, tag=0x%x, transient_tag=0x%x, state=%s, locked=%d}\n", slot, block->tag, block->transient_tag, str_map_value(&cache_block_state_map, block->state), dir_lock->lock);
			}

			/* Statistics */
			if (!(stack->retry & (1 << mod->level)))
			{
				mod->no_retry_stream_hits++;
				mod->no_retry_hits++;
			}

			if (stack->request_dir == mod_request_up_down)
			{
				if (stack->stream_head_hit)
					mod->up_down_head_hits++;
				mod->up_down_hits++;
			}
			else
			{
				if (stack->read)
					mod->down_up_read_hits++;
				else
					mod->down_up_write_hits++;
			}
		}

		/* If stream hit lock slot */
		if (stack->stream_hit)
		{
			block = cache_get_pref_block(cache, stack->pref_stream, stack->pref_slot);
			dir_lock = dir_pref_lock_get(mod->dir, stack->pref_stream, stack->pref_slot);
			if (dir_lock->lock && stack->request_dir == mod_request_up_down)
			{
				mem_debug("    %lld 0x%x %s pref_stream %d pref_slot %d containing 0x%x (0x%x) already locked by stack %lld, retrying...\n", stack->id, stack->tag, mod->name, stack->pref_stream, stack->pref_slot, block->tag, block->transient_tag, dir_lock->locking_stack->id);
				if(!stack->stream_retried)
				{
					assert(!stack->stream_retried_cycle);
					if(dir_lock->locking_stack->prefetch)
					{
						mod->delayed_hits++;
						ret->stream_retried_cycle = esim_cycle();
						ret->stream_retried = 1;
					}
				}
				mod_unlock_port(mod, port, stack);

				dir_entry_unlock(mod->dir, stack->set, stack->way); /* Unlock cache entry */

				/* Interval statistics */
				if (should_count_stats(stack))
				{
					int core = stack->client_info->core;
					int thread = stack->client_info->thread;
					int pos = core * x86_cpu_num_threads + thread;
					mod->retries++;
					mod->report_stack->retries_per_thread_int[pos]++;
					ctx->report_stack->retries_per_level_int[mod->level]++;
					X86_THREAD.report_stack->retries_per_level_int[mod->level]++;
					/* If the stack locking the block is a prefetch that is bringing the block, then count that prefetch as a late prefetch */
					if(dir_lock->locking_stack->prefetch)
						dir_lock->locking_stack->client_info->late_prefetch = 1;
				}

				ret->port_locked = 0;
				ret->err = 1;
				mod_stack_return(stack);
				return;
			}
			if (!dir_pref_entry_lock(mod->dir, stack->pref_stream, stack->pref_slot, EV_MOD_NMOESI_FIND_AND_LOCK, stack))
			{
				mem_debug("    %lld 0x%x %s pref_stream %d pref_slot %d containing 0x%x (0x%x) already locked by stack %lld, waiting...\n", stack->id, stack->tag, mod->name, stack->pref_stream, stack->pref_slot, block->tag, block->transient_tag, dir_lock->locking_stack->id);
				mod_unlock_port(mod, port, stack);
				ret->port_locked = 0;
				return;
			}

			if(stack->stream_retried)
			{
				assert(stack->stream_retried_cycle);
				mod->delayed_hit_cycles += esim_cycle() - stack->stream_retried_cycle;
				mod->delayed_hits_cycles_counted++;
				ret->stream_retried = 0;
			}
		}

		/* Prefetch buffer entry is locked. Record 0xFFFFFFFF as transient tag so that a subsequent lookup
		 * detects that the block is being removed from buffer. TODO: Crec q no fa falta tocar la tag...
		 * Also, update LRU counters here. */
		if (stack->request_dir == mod_request_up_down && stack->stream_hit)
		{
			block = cache_get_pref_block(cache, stack->pref_stream, stack->pref_slot);
			block->transient_tag = -1;
			if (stack->stream_head_hit)
				cache_access_stream(cache, stack->pref_stream);
		}

		/* Access latency */
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK_ACTION, stack, mod->dir_latency);
		return;
	}

	if (event == EV_MOD_NMOESI_FIND_AND_LOCK_ACTION)
	{
		struct mod_port_t *port = stack->port;

		assert(port);

		/* Because of silent replacement used in streams, there may be misses in down up requests */
		if(!stack->hit && !stack->stream_hit && stack->request_dir == mod_request_down_up)
		{
			mem_debug("  %lld %lld 0x%x %s down up miss due a silent eviction in a stream\n", esim_time, stack->id,
				stack->tag, mod->name);
			stack->state = cache_block_invalid;
			if(stack->read)
				mod->down_up_read_misses++;
			else if(stack->write)
				mod->down_up_write_misses++;
			else
				fatal("Unknown memory operation type");
		}

		/* No request can't be a hit and a prefetch hit at same time */
		assert(!stack->hit || !stack->stream_hit);

		mem_debug("  %lld %lld 0x%x %s find and lock action\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:find_and_lock_action\"\n",
			stack->id, mod->name);

		/* Release port */
		mod_unlock_port(mod, port, stack);
		ret->port_locked = 0;

		/* On miss, evict if victim is a valid block. */
		if (!stack->hit && stack->state && stack->request_dir == mod_request_up_down)
		{
			/* If stream hit, leave a background stack do the tough job and continue bringing the block to cpu */
			if (stack->stream_hit)
			{
				struct mod_stack_t *background_stack;
				struct mod_stack_t *find_and_lock_stack;
				struct mod_stack_t *eviction_stack;
				struct write_buffer_block_t *block;
				struct stream_buffer_t *sb;
				struct dir_lock_t *dir_lock;

				mem_debug("  %lld %lld 0x%x %s fast resume access\n", esim_time, stack->id, stack->tag, mod->name);

				assert(!(mod->kind==mod_kind_main_memory && stack->request_dir == mod_request_up_down));// P
				assert(stack->pref_stream >= 0 && stack->pref_stream < pref->max_num_streams);
				assert(stack->pref_slot >= 0 && stack->pref_slot < pref->max_num_slots);

				sb = &pref->streams[stack->pref_stream];

				/* Stack that will write block from prefetch buffer to cache */
				background_stack = mod_stack_create(stack->id, ret->mod, stack->addr, ESIM_EV_NONE,
					NULL, stack->prefetch);
				background_stack->target_mod = ret->target_mod;
				background_stack->request_dir = ret->request_dir;
				background_stack->reply_size = ret->reply_size;
				background_stack->reply = ret->reply;
				background_stack->client_info = mod_client_info_clone(stack->mod, stack->client_info);

				/* Change the pointer to the owner of the lock */
				dir_lock = dir_lock_get(mod->dir, stack->set, stack->way);
				dir_lock->locking_stack = background_stack;

				/* Stack that will wait for eviction to complete */
				find_and_lock_stack = mod_stack_create(stack->id, mod, stack->addr,
					stack->ret_event, background_stack, stack->prefetch);
				find_and_lock_stack->blocking = stack->blocking; /* Not used */
				find_and_lock_stack->read = stack->read; /* Not used */
				find_and_lock_stack->write = stack->write; /* Not used */
				find_and_lock_stack->retry = stack->retry; /* Not used */
				find_and_lock_stack->set = stack->set;
				find_and_lock_stack->way = stack->way;
				find_and_lock_stack->atd_way = stack->atd_way;
				find_and_lock_stack->state = stack->state;
				find_and_lock_stack->tag = stack->tag;
				find_and_lock_stack->pref_stream = -1; /* For this stack there hasn't been a prefetch hit */
				find_and_lock_stack->pref_slot = -1; /* " */
				find_and_lock_stack->stream_hit = 0; /* " */
				find_and_lock_stack->stream_head_hit = 0; /* " */
				find_and_lock_stack->request_dir = stack->request_dir;
				find_and_lock_stack->eviction = 1;
				find_and_lock_stack->background = 1;

				/* Eviction stack */
				eviction_stack = mod_stack_create(stack->id, mod, 0,
					EV_MOD_NMOESI_FIND_AND_LOCK_FINISH, find_and_lock_stack, stack->prefetch);
				eviction_stack->set = stack->set;
				eviction_stack->way = stack->way;

				/* Copy block from stream to cache write buffer */
				block = xcalloc(1, sizeof(struct write_buffer_block_t));
				cache_get_pref_block_data(cache, stack->pref_stream, stack->pref_slot, &block->tag, (int *) &block->state);
				block->stack_id = background_stack->id;
				linked_list_add(cache->wb.blocks, block);

				/* This stack will be fast resumed */
				stack->fast_resume = 1;
				stack->state = block->state;

				/* Free stream entry */
				cache_set_pref_block(cache, stack->pref_stream, stack->pref_slot, block_invalid_tag, cache_block_invalid);
				sb->count--; //COUNT

				/* Update head */
				if (stack->stream_head_hit)
					sb->head = (sb->head + 1) % pref->max_num_slots; //HEAD

				/* Unlock stream entry */
				dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);

				/* Statistics */
				mod->fast_resumed_accesses++;

				esim_schedule_event(EV_MOD_NMOESI_EVICT, eviction_stack, 0);
			}
			else
			{
				stack->eviction = 1;
				new_stack = mod_stack_create(stack->id, mod, 0,
					EV_MOD_NMOESI_FIND_AND_LOCK_FINISH, stack, stack->prefetch);
				new_stack->set = stack->set;
				new_stack->way = stack->way;
				esim_schedule_event(EV_MOD_NMOESI_EVICT, new_stack, 0);
				return;
			}
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_FIND_AND_LOCK_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s find and lock finish (err=%d fr=%d bg=%d)\n", esim_time, stack->id,
			stack->tag, mod->name, stack->err, stack->fast_resume, stack->background);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:find_and_lock_finish\"\n",
			stack->id, mod->name);

		/* If evict produced err, return err */
		if (stack->err)
		{
			cache_get_block(mod->cache, stack->set, stack->way, NULL, &stack->state);
			assert(stack->state);
			assert(stack->eviction);

			dir_entry_unlock(mod->dir, stack->set, stack->way);

			if (stack->stream_hit)
				dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);

			/* Interval statistics */
			if (should_count_stats(stack))
			{
				int core = stack->client_info->core;
				int thread = stack->client_info->thread;
				int pos = core * x86_cpu_num_threads + thread;
				mod->retries++; /* Total number of retries for this module */
				mod->report_stack->retries_per_thread_int[pos]++;
				ctx->report_stack->retries_per_level_int[mod->level]++;  /* Number of retries for the context in this module for the current interval */
				X86_THREAD.report_stack->retries_per_level_int[mod->level]++;
			}

			ret->err = 1;
			ret->background = stack->background;
			mod_stack_return(stack);
			return;
		}

		/* Statistics */
		if (should_count_stats(stack))
		{
			int core = stack->client_info->core;
			int thread = stack->client_info->thread;

			assert(mod->reachable_threads[thread_id]);

			/* Accesses */
			ctx->report_stack->accesses_per_level_int[mod->level]++;

			/* Only count accesses that missed (or we are not sure if missed) in a previous cache level */
			if (stack->src_atd_hit <= 0)
			{
				/* ATD Accesses */
				ctx->report_stack->atd_accesses_per_level_int[mod->level]++;

				/* Alternate Tag Directory hits */
				if (stack->atd_hit > 0)
				{
					mod->atd_hits_per_thread[thread_id]++;
					atd_update_stack_distance_counters(mod->atd_per_thread[thread_id], stack->set, stack->way);
					mod->report_stack->atd_hits_per_thread_int[thread_id]++;
					ctx->report_stack->atd_hits_per_level_int[mod->level]++;
					/* Count divergences */
					if (!stack->hit)
						ctx->report_stack->atd_cm_ah_per_level_int[mod->level]++;
				}

				/* Alternate Tag Directory misses */
				else if (stack->atd_hit == 0)
				{
					mod->atd_misses_per_thread[thread_id]++;
					mod->report_stack->atd_misses_per_thread_int[thread_id]++;
					ctx->report_stack->atd_misses_per_level_int[mod->level]++;
					if (stack->hit)
						ctx->report_stack->atd_ch_am_per_level_int[mod->level]++;
				}

				/* Access not covered by ATD */
				else
				{
					assert(stack->atd_hit == -1);
					mod->atd_unknown_per_thread[thread_id]++;
					mod->report_stack->atd_unknown_per_thread_int[thread_id]++;
					ctx->report_stack->atd_unknown_per_level_int[mod->level]++;
				}
			}

			/* Cache hits, misses, retries... */
			if (stack->hit)
			{
				mod->hits++;
				mod->report_stack->hits_per_thread_int[thread_id]++;
				ctx->report_stack->hits_per_level_int[mod->level]++;
				X86_THREAD.report_stack->hits_per_level_int[mod->level]++;
			}
			else if (stack->stream_hit)
			{
				mod->stream_hits++;
				mod->report_stack->stream_hits_per_thread_int[thread_id]++;
				ctx->report_stack->stream_hits_per_level_int[mod->level]++;
				X86_THREAD.report_stack->stream_hits_per_level_int[mod->level]++;

				/* ATD hit in previous level */
				if (stack->src_atd_hit > 0)
				{
					struct x86_uop_t *uop = stack->event_queue_item;
					if (uop)
						uop->uinst->interthread_miss = 1; /* Mark this memory instruction as an interthread miss */
					if (X86_THREAD.ctx)
						X86_THREAD.ctx->interthread_cache_penalty_cycles += mod->latency + mod->dir_latency;
				}
			}
			else
			{
				mod->misses++;
				mod->report_stack->misses_per_thread_int[thread_id]++;
				ctx->report_stack->misses_per_level_int[mod->level]++;
				X86_THREAD.report_stack->misses_per_level_int[mod->level]++;

				/* Cache miss and ATD miss */
				if (stack->atd_hit == 0)
				{
					mod->atd_intramisses_per_thread[thread_id]++;
					mod->report_stack->atd_intramisses_per_thread_int[thread_id]++;
					ctx->report_stack->atd_intramisses_per_level_int[mod->level]++;
				}

				/* Cache miss and ATD hit */
				else if (stack->atd_hit > 0)
				{
					struct x86_uop_t *uop = stack->event_queue_item;
					if (uop)
						uop->uinst->interthread_miss = 1; /* Mark this memory instruction as an interthread miss */
					mod->atd_intermisses_per_thread[thread_id]++;
					mod->report_stack->atd_intermisses_per_thread_int[thread_id]++;
					ctx->report_stack->atd_intermisses_per_level_int[mod->level]++;
				}

				/* Cache miss and ATD status unknown */
				else
					assert(stack->atd_hit == -1);
			}

			/* Prefetch usefulness */
			if (mod_get_prefetched_bit(mod, stack->tag) || stack->stream_hit)
			{
				mod->useful_prefetches++;
				ctx->report_stack->useful_prefs_per_level_int[mod->level]++;
				mod_set_prefetched_bit(mod, stack->addr, 0);
			}

			/* If this is an on demand request, search the block in the pollution filters to find out if it
			 * has been previously evicted by a prefetch request or by another accessig thread */
			if (!stack->hit && !stack->stream_hit)
			{
				/* Prefetch - demand pollution for adaptive prefetch */
				if (prefetcher_uses_pollution_filters(pref) && BLOOM_FIND(mod->adapt_pref_stack->pref_pollution_filter, stack->tag))
					mod->adapt_pref_stack->pref_pollution_int++;

				/* Prefetch - demand pollution */
				if (hash_table_gen_get(mod->report_stack->pref_pollution_filter, (void*) &stack->tag, sizeof(stack->tag)))
					mod->report_stack->pref_pollution_int++;

				/* Thread - thread pollution */
				X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
				{
					int pos = core * x86_cpu_num_threads + thread;
					if (mod->reachable_threads[pos] && pos != thread_id)
					{
						/* Demand */
						if (hash_table_gen_get(mod->report_stack->dem_pollution_filter_per_thread[pos], (void*) &stack->tag, sizeof(stack->tag)))
						{
							mod->report_stack->dem_pollution_per_thread_int[thread_id]++;
							goto out; /* Exiting two nested loops */
						}
						/* Prefetch */
						if (hash_table_gen_get(mod->report_stack->pref_pollution_filter_per_thread[pos], (void*) &stack->tag, sizeof(stack->tag)))
						{
							mod->report_stack->pref_pollution_per_thread_int[thread_id]++;
							goto out;
						}
					}
				}
out:;
			}
		}

		/* Eviction */
		if (stack->eviction)
		{
			/* Interval statistics */
			if (should_count_stats(stack))
			{
				int core = stack->client_info->core;
				int thread = stack->client_info->thread;
				int pos = core * x86_cpu_num_threads + thread;
				mod->evictions++;
				mod->report_stack->evictions_per_thread_int[pos]++;
				ctx->report_stack->evictions_per_level_int[mod->level]++;
				X86_THREAD.report_stack->evictions_per_level_int[mod->level]++;
			}
			cache_get_block(mod->cache, stack->set, stack->way, NULL, &stack->state);
			assert(!stack->state);
		}

		if (mod->kind == mod_kind_main_memory && !stack->state)
		{
			stack->state = cache_block_exclusive;
			cache_set_block(mod->cache, stack->set, stack->way, stack->tag, stack->state, stack->client_info);
			atd_set_block(mod->atd_per_thread[thread_id], stack->tag, stack->state);
		}

		/* Return */
		ret->err = 0;
		ret->set = stack->set;
		ret->way = stack->way;
		ret->atd_way = stack->atd_way;
		ret->state = stack->state;
		ret->tag = stack->tag;
		ret->stream_hit = stack->stream_hit;
		ret->stream_head_hit = stack->stream_head_hit;
		ret->pref_stream = stack->pref_stream;
		ret->pref_slot = stack->pref_slot;
		ret->fast_resume = stack->fast_resume;
		ret->background = stack->background;
		ret->wb_hit = stack->wb_hit;
		ret->hit = stack->hit;
		ret->atd_hit = stack->atd_hit;
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_evict(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct mod_t *target_mod = stack->target_mod;

	struct dir_t *dir;
	struct dir_entry_t *dir_entry;

	unsigned int dir_entry_tag;

	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_EVICT)
	{
		/* Default return value */
		ret->err = 0;

		/* Get block info */
		cache_get_block(mod->cache, stack->set, stack->way, &stack->tag, &stack->state);
		assert(stack->state || !dir_entry_group_shared_or_owned(mod->dir,
			stack->set, stack->way));
		mem_debug("  %lld %lld 0x%x %s evict (set=%d, way=%d, state=%s)\n", esim_time, stack->id,
			stack->tag, mod->name, stack->set, stack->way,
			str_map_value(&cache_block_state_map, stack->state));
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict\"\n",
			stack->id, mod->name);

		/* Save some data */
		stack->src_set = stack->set;
		stack->src_way = stack->way;
		stack->src_tag = stack->tag;
		stack->target_mod = mod_get_low_mod(mod, stack->tag);

		/* Send write request to all sharers */
		new_stack = mod_stack_create(stack->id, mod, 0, EV_MOD_NMOESI_EVICT_INVALID, stack, stack->prefetch);
		new_stack->except_mod = NULL;
		new_stack->set = stack->set;
		new_stack->way = stack->way;
		assert(stack->client_info);
		assert(new_stack->client_info);
		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_INVALID)
	{
		mem_debug("  %lld %lld 0x%x %s evict invalid\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_invalid\"\n",
			stack->id, mod->name);

		/* If module is main memory, we just need to set the block as invalid,
		 * and finish. */
		if (mod->kind == mod_kind_main_memory)
		{
			cache_set_block(mod->cache, stack->src_set, stack->src_way,
				0, cache_block_invalid, stack->client_info);
			esim_schedule_event(EV_MOD_NMOESI_EVICT_FINISH, stack, 0);
			return;
		}

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_EVICT_ACTION, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_ACTION)
	{
		struct mod_t *low_mod;
		struct net_node_t *low_node;
		int msg_size;

		mem_debug("  %lld %lld 0x%x %s evict action\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_action\"\n",
			stack->id, mod->name);

		/* Get low node */
		low_mod = stack->target_mod;
		low_node = low_mod->high_net_node;
		assert(low_mod != mod);
		assert(low_mod == mod_get_low_mod(mod, stack->tag));
		assert(low_node && low_node->user_data == low_mod);

		/* Update the cache state since it may have changed after its
		 * higher-level modules were invalidated */
		cache_get_block(mod->cache, stack->set, stack->way, NULL, &stack->state);

		/* State = I */
		if (stack->state == cache_block_invalid)
		{
			esim_schedule_event(EV_MOD_NMOESI_EVICT_FINISH, stack, 0);
			return;
		}

		/* If state is M/O/N, data must be sent to lower level mod */
		if (stack->state == cache_block_modified || stack->state == cache_block_owned ||
			stack->state == cache_block_noncoherent)
		{
			/* Need to transmit data to low module */
			msg_size = 8 + mod->block_size;
			stack->reply = reply_ack_data;
		}

		/* If state is E/S, just an ack needs to be sent */
		else
		{
			msg_size = 8;
			stack->reply = reply_ack;
		}

		/* Send message */
		stack->msg = net_try_send_ev(mod->low_net, mod->low_net_node,
			low_node, msg_size, EV_MOD_NMOESI_EVICT_RECEIVE, stack, event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s evict receive\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_receive\"\n",
			stack->id, target_mod->name);

		/* Receive message */
		net_receive_msg(target_mod->high_net, target_mod->high_net_node, stack->msg);

		/* Find and lock */
		if (stack->state == cache_block_noncoherent)
		{
			new_stack = mod_stack_create(stack->id, target_mod, stack->src_tag,
				EV_MOD_NMOESI_EVICT_PROCESS_NONCOHERENT, stack, stack->prefetch);
		}
		else
		{
			new_stack = mod_stack_create(stack->id, target_mod, stack->src_tag,
				EV_MOD_NMOESI_EVICT_PROCESS, stack, stack->prefetch);
		}

		/* FIXME It's not guaranteed to be a write */
		new_stack->blocking = 0;
		new_stack->write = 1;
		new_stack->retry = 0;
		new_stack->request_dir = mod_request_up_down;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_PROCESS)
	{
		//mem_controller= target_mod->mem_controller;
		mem_debug("  %lld %lld 0x%x %s evict process\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_process\"\n",
			stack->id, target_mod->name);

		assert(!stack->stream_hit); //VVV
		assert(stack->state); //VVV

		/* Error locking block */
		if (stack->err)
		{
			ret->err = 1;
			esim_schedule_event(EV_MOD_NMOESI_EVICT_REPLY, stack, 0);
			return;
		}

		/* If data was received, set the block to modified */
		if (stack->reply == reply_ack)
		{
			/* Nothing to do */
		}
		else if (stack->reply == reply_ack_data)
		{
			if(target_mod->kind == mod_kind_main_memory &&
					target_mod->dram_system)
			{
				struct dram_system_t *ds = target_mod->dram_system;
				struct x86_ctx_t *ctx = stack->client_info->ctx;
				assert(ds);
				assert(ctx);

				/* Retry if memory controller cannot accept transaction */
				if (!dram_system_will_accept_trans(ds->handler, stack->tag))
				{
					stack->err = 1;
					ret->err = 1;
					ret->retry |= 1 << target_mod->level;

					dir = target_mod->dir;
					dir_entry_unlock(dir, stack->set, stack->way);

					mem_debug("    %lld 0x%x %s mc queue full, retrying write...\n", stack->id, stack->tag, target_mod->name);

					esim_schedule_event(EV_MOD_NMOESI_EVICT_REPLY, stack, 0);
					return;
				}

				/* Access main memory system */
				dram_system_add_write_trans(ds->handler, stack->tag, stack->client_info->core, stack->client_info->thread);

				/* Ctx main memory stats */
				ctx->mm_write_accesses++;
			}

			if (stack->state == cache_block_exclusive)
			{
				cache_set_block(target_mod->cache, stack->set, stack->way,
					stack->tag, cache_block_modified, stack->client_info);
			}
			else if (stack->state == cache_block_modified)
			{
				/* Nothing to do */
			}
			else
			{
				fatal("%s: Invalid cache block state: %d\n", __FUNCTION__,
					stack->state);
			}
		}
		else
		{
			fatal("%s: Invalid cache block state: %d\n", __FUNCTION__,
				stack->state);
		}

		/* Remove sharer and owner */
		dir = target_mod->dir;
		for (int z = 0; z < dir->zsize; z++)
		{
			/* Skip other subblocks */
			dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
			assert(dir_entry_tag < stack->tag + target_mod->block_size);
			if (dir_entry_tag < stack->src_tag ||
				dir_entry_tag >= stack->src_tag + mod->block_size)
			{
				continue;
			}

			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
			dir_entry_clear_sharer(dir, stack->set, stack->way, z,
				mod->low_net_node->index);
			if (dir_entry->owner == mod->low_net_node->index)
			{
				dir_entry_set_owner(dir, stack->set, stack->way, z,
					DIR_ENTRY_OWNER_NONE);
			}
		}

		/* Unlock the directory entry */
		dir = target_mod->dir;
		dir_entry_unlock(dir, stack->set, stack->way);

		/* If the access is to main memory then return inmediately, because the transaction is already
		 * inserted and will be processed in background if using a memory controller or ignored if using a fixed latency main memory */
		esim_schedule_event(EV_MOD_NMOESI_EVICT_REPLY, stack, target_mod->kind == mod_kind_main_memory ? 0 : target_mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_PROCESS_NONCOHERENT)
	{
		mem_debug("  %lld %lld 0x%x %s evict process noncoherent\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_process_noncoherent\"\n",
			stack->id, target_mod->name);

		/* Error locking block */
		if (stack->err)
		{
			ret->err = 1;
			esim_schedule_event(EV_MOD_NMOESI_EVICT_REPLY, stack, 0);
			return;
		}

		/* If data was received, set the block to modified */
		if (stack->reply == reply_ack_data)
		{
			if (stack->state == cache_block_exclusive)
			{
				cache_set_block(target_mod->cache, stack->set, stack->way,
					stack->tag, cache_block_modified, stack->client_info);
			}
			else if (stack->state == cache_block_owned ||
				stack->state == cache_block_modified)
			{
				/* Nothing to do */
			}
			else if (stack->state == cache_block_shared ||
				stack->state == cache_block_noncoherent)
			{
				cache_set_block(target_mod->cache, stack->set, stack->way,
					stack->tag, cache_block_noncoherent, stack->client_info);
			}
			else
			{
				fatal("%s: Invalid cache block state: %d\n", __FUNCTION__,
					stack->state);
			}
		}
		else
		{
			fatal("%s: Invalid cache block state: %d\n", __FUNCTION__,
				stack->state);
		}

		/* Remove sharer and owner */
		dir = target_mod->dir;
		for (int z = 0; z < dir->zsize; z++)
		{
			/* Skip other subblocks */
			dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
			assert(dir_entry_tag < stack->tag + target_mod->block_size);
			if (dir_entry_tag < stack->src_tag ||
				dir_entry_tag >= stack->src_tag + mod->block_size)
			{
				continue;
			}

			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
			dir_entry_clear_sharer(dir, stack->set, stack->way, z,
				mod->low_net_node->index);
			if (dir_entry->owner == mod->low_net_node->index)
			{
				dir_entry_set_owner(dir, stack->set, stack->way, z,
					DIR_ENTRY_OWNER_NONE);
			}
		}

		/* Unlock the directory entry */
		dir = target_mod->dir;
		dir_entry_unlock(dir, stack->set, stack->way);

		esim_schedule_event(EV_MOD_NMOESI_EVICT_REPLY, stack, target_mod->latency);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_REPLY)
	{
		mem_debug("  %lld %lld 0x%x %s evict reply\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_reply\"\n",
			stack->id, target_mod->name);

		/* Send message */
		stack->msg = net_try_send_ev(target_mod->high_net, target_mod->high_net_node,
			mod->low_net_node, 8, EV_MOD_NMOESI_EVICT_REPLY_RECEIVE, stack,
			event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_REPLY_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s evict reply receive\n", esim_time, stack->id,
				stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_reply_receive\"\n",
				stack->id, mod->name);

		/* Receive message */
		net_receive_msg(mod->low_net, mod->low_net_node, stack->msg);

		/* Invalidate block if there was no error. */
		if (!stack->err)
		{
			int core = stack->client_info->core;
			int thread = stack->client_info->thread;
			int pos = core * x86_cpu_num_threads + thread;

			assert(mod->reachable_threads[pos]);
			assert(core >= 0 && core < x86_cpu_num_cores);
			assert(thread >= 0 && thread < x86_cpu_num_threads);

			/* If an access evicts a block, store the tag to know if it is accessed in the future and estimate pollution */

			/* Prefetch pollution */
			if (stack->prefetch)
			{
				assert(!prefetcher_uses_stream_buffers(mod->cache->prefetcher)); /* Prefetches to stream buffers cannot cause evictions */

				/* Prefetch pollution for adaptive prefetch */
				if (prefetcher_uses_pollution_filters(mod->cache->prefetcher))
				{
					struct bloom_t *bloom = mod->adapt_pref_stack->pref_pollution_filter;
					double false_pos_prob = BLOOM_ADD(bloom, stack->src_tag);
					if (false_pos_prob > bloom->max_false_pos_prob * 2)
						warning("%lld Module %s pollution filter full, false positive probability is %f", esim_time, mod->name, false_pos_prob);
				}

				/* Prefetch pollution */
				hash_table_gen_insert(mod->report_stack->pref_pollution_filter, (void*) &stack->src_tag, sizeof(stack->src_tag), (void*) 1);

				/* Interthread prefetch pollution */
				hash_table_gen_insert(mod->report_stack->pref_pollution_filter_per_thread[pos], (void*) &stack->src_tag, sizeof(stack->src_tag), (void*) 1);
			}

			/* Interthread demand pollution */
			else
				hash_table_gen_insert(mod->report_stack->dem_pollution_filter_per_thread[pos], (void*) &stack->src_tag, sizeof(stack->src_tag), (void*) 1);


			/* Invalidate block */
			cache_set_block(mod->cache, stack->src_set, stack->src_way,
					0, cache_block_invalid, stack->client_info);
		}
		assert(!dir_entry_group_shared_or_owned(mod->dir, stack->src_set, stack->src_way));
		esim_schedule_event(EV_MOD_NMOESI_EVICT_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_EVICT_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s evict finish\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:evict_finish\"\n",
			stack->id, mod->name);

		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_invalidate_slot(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;
	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;

	if (event == EV_MOD_NMOESI_INVALIDATE_SLOT)
	{
		mem_debug("%lld %lld 0x%x %s invalidate slot\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.new_access name=\"A-%lld\" type=\"invalidate_slot\" state=\"%s:invalidate_slot\" addr=0x%x\n", stack->id, mod->name, stack->addr);

		/* Set pref stream and slot */
		stack->pref_stream = stack->client_info->stream;
		stack->pref_slot = stack->client_info->slot;
		assert(stack->pref_stream >= 0 && stack->pref_stream < pref->max_num_streams);
		assert(stack->pref_slot >= 0 && stack->pref_slot < pref->max_num_slots);

		/* Next event */
		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_SLOT_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_INVALIDATE_SLOT_LOCK)
	{
		mem_debug("  %lld %lld 0x%x %s invalidate slot lock\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:invalidate_slot_lock\"\n", stack->id, mod->name);

		/* Call pref find and lock */
		new_stack = mod_stack_create(stack->id, mod, stack->addr, EV_MOD_NMOESI_INVALIDATE_SLOT_ACTION, stack, stack->prefetch);
		new_stack->blocking = 1;
		new_stack->retry = stack->retry;
		new_stack->pref_stream = stack->pref_stream;
		new_stack->pref_slot = stack->pref_slot;
		new_stack->access_kind = mod_access_invalidate_slot;
		new_stack->request_dir = mod_request_up_down;
		esim_schedule_event(EV_MOD_NMOESI_PREF_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_INVALIDATE_SLOT_ACTION)
	{
		int retry_lat;

		mem_debug("  %lld %lld 0x%x %s invalidate slot action\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:invalidate_slot_action\"\n", stack->id, mod->name);

		/* Error locking */
		if (stack->err)
		{
			mod->read_retries++;
			retry_lat = mod_get_retry_latency(mod);
			mem_debug("    lock error, retrying in %d cycles\n", retry_lat);
			stack->retry = 1;
			esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_SLOT_LOCK, stack, retry_lat);
			return;
		}

		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_SLOT_UNLOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_INVALIDATE_SLOT_UNLOCK)
	{
		mem_debug("  %lld %lld 0x%x %s invalidate slot unlock\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:invalidate_slot_unlock\"\n", stack->id, mod->name);

		/* Unlock directory entry */
		dir_pref_entry_unlock(mod->dir, stack->pref_stream, stack->pref_slot);

		/* Continue */
		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_SLOT_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_INVALIDATE_SLOT_FINISH)
	{
		struct stream_buffer_t *sb;

		mem_debug("%lld %lld 0x%x %s invalidate slot finish\n", esim_time, stack->id, stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:load_finish\"\n", stack->id, mod->name);
		mem_trace("mem.end_access name=\"A-%lld\"\n", stack->id);

		/* Increment witness variable */
		if (stack->witness_ptr)
			(*stack->witness_ptr)++;

		/* Free the mod_client_info object, if any */
		if (stack->client_info)
			mod_client_info_free(mod, stack->client_info);

		/* When only remains one pending prefetch the stream tag is set */
		sb = &pref->streams[stack->pref_stream];
		assert(sb->pending_prefetches > 0);
		if (!stack->ret_stack)
		{
			if (sb->pending_prefetches == 1)
			{
				sb->stream_tag = stack->addr & pref->stream_tag_mask;
				assert(sb->stream_tag == sb->stream_transcient_tag);
			}
			sb->pending_prefetches--;
		}

		/* Return event queue element into event queue */
		if (stack->event_queue && stack->event_queue_item)
			linked_list_add(stack->event_queue, stack->event_queue_item);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_read_request(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct mod_t *target_mod = stack->target_mod;

	struct cache_t *target_cache = target_mod->cache;

	struct dir_t *dir;
	struct dir_entry_t *dir_entry;

	unsigned int dir_entry_tag;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_READ_REQUEST)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s read request\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request\"\n",
			stack->id, mod->name);

		/* Default return values */
		ret->shared = 0;
		ret->err = 0;

		/* Checks */
		assert(stack->request_dir);
		assert(mod_get_low_mod(mod, stack->addr) == target_mod ||
			stack->request_dir == mod_request_down_up);
		assert(mod_get_low_mod(target_mod, stack->addr) == mod ||
			stack->request_dir == mod_request_up_down);

		/* Get source and destination nodes */
		if (stack->request_dir == mod_request_up_down)
		{
			net = mod->low_net;
			src_node = mod->low_net_node;
			dst_node = target_mod->high_net_node;
		}
		else
		{
			net = mod->high_net;
			src_node = mod->high_net_node;
			dst_node = target_mod->low_net_node;
		}

		/* Send message */
		stack->msg = net_try_send_ev(net, src_node, dst_node, 8,
			EV_MOD_NMOESI_READ_REQUEST_RECEIVE, stack, event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s read request receive\n", esim_time, stack->id,
			stack->addr, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_receive\"\n",
			stack->id, target_mod->name);

		/* Receive message */
		if (stack->request_dir == mod_request_up_down)
			net_receive_msg(target_mod->high_net, target_mod->high_net_node, stack->msg);
		else
			net_receive_msg(target_mod->low_net, target_mod->low_net_node, stack->msg);

		if (stack->request_dir == mod_request_up_down &&
			target_mod->kind != mod_kind_main_memory &&
			target_mod->cache->prefetcher)
		{
			/* Record access */
			mod_access_start(target_mod, stack, mod_access_read_request);
		}

		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_LOCK)
	{
		mem_debug("  %lld %lld 0x%x %s read request lock\n", esim_time, stack->id,
			stack->addr, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_lock\"\n",
			stack->id, mod->name);

		/* Find and lock */
		new_stack = mod_stack_create(stack->id, target_mod, stack->addr,
			EV_MOD_NMOESI_READ_REQUEST_ACTION, stack, stack->prefetch);
		new_stack->blocking = stack->request_dir == mod_request_down_up;
		new_stack->read = 1;
		new_stack->retry = stack->retry; //VVV
		new_stack->background = stack->background; /* Per si hi ha fallada (err=-1) en la eviction */
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->request_dir = stack->request_dir;
		new_stack->access_kind = mod_access_read_request;
		new_stack->event_queue_item = stack->event_queue_item;
		new_stack->src_atd_hit = stack->src_atd_hit; /* An ATD hit in a given level should not be an ATD hit in a deeper level */
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_ACTION)
	{
		mem_debug("  %lld %lld 0x%x %s read request action\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_action\"\n",
			stack->id, target_mod->name);

		/* Check block locking error. If read request is down-up, there should not
		 * have been any error while locking. */
		if (stack->err)
		{
			assert(stack->request_dir == mod_request_up_down);

			/* If this is a background stack ret must be null */
			if(stack->background)
			{
				assert(!ret);
				assert(!stack->stream_hit);
				esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_LOCK, stack, 0);
				return;
			}

			ret->err = 1;
			ret->retry |= 1 << target_mod->level;
			ret->stream_retried = stack->stream_retried;
			ret->stream_retried_cycle = stack->stream_retried_cycle;
			mod_stack_set_reply(ret, reply_ack_error);
			stack->reply_size = 8;

			/* Delete access */
			if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
				mod_access_finish(target_mod, stack);

			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, 0);
			return;
		}

		/* A fast resumed stack only has to return the block to the upper level cache */
		if (stack->fast_resume)
		{
			assert(target_mod->kind != mod_kind_main_memory);

			/* If the block is owned, non-coherent, or shared,
			 * mod (the higher-level cache) should never be exclusive */
			if (stack->state == cache_block_owned ||
				stack->state == cache_block_noncoherent ||
				stack->state == cache_block_shared)
				ret->shared = 1;

			mod_access_finish(target_mod, stack);

			/* Set reply message and size */
			stack->reply_size = mod->block_size + 8;
			mod_stack_set_reply(stack, reply_ack_data);

			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, target_mod->latency);
			return;
		}

		esim_schedule_event(stack->request_dir == mod_request_up_down ?
			EV_MOD_NMOESI_READ_REQUEST_UPDOWN : EV_MOD_NMOESI_READ_REQUEST_DOWNUP, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_UPDOWN)
	{
		struct mod_t *owner;

		mem_debug("  %lld %lld 0x%x %s read request updown (%s)\n", esim_time, stack->id,
			stack->tag, target_mod->name,str_map_value(&cache_block_state_map, stack->state));
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_updown\"\n",
			stack->id, target_mod->name);

		stack->pending = 1;

		/* Set the initial reply message and size.  This will be adjusted later if
		 * a transfer occur between peers. */
		stack->reply_size = mod->block_size + 8;
		mod_stack_set_reply(stack, reply_ack_data);

		/* If stream_hit or background stack, block can't be in any upper cache */
		if (stack->stream_hit || stack->background)
		{
			assert(target_mod->kind != mod_kind_main_memory);
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_UPDOWN_MISS, stack, 0);
			return;
		}

		if (stack->state)
		{
			/* Status = M/O/E/S/N
			 * Check: address is a multiple of requester's block_size
			 * Check: no sub-block requested by mod is already owned by mod */
			assert(stack->addr % mod->block_size == 0);
			dir = target_mod->dir;
			for (int z = 0; z < dir->zsize; z++)
			{
				dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
				assert(dir_entry_tag < stack->tag + target_mod->block_size);
				if (dir_entry_tag < stack->addr || dir_entry_tag >= stack->addr + mod->block_size)
					continue;
				dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
				//assert(dir_entry->owner != mod->low_net_node->index);
				if(dir_entry->owner == mod->low_net_node->index)
					mod->block_already_here++;
			}

			/* TODO If there is only sharers, should one of them
			 * send the data to mod instead of having target_mod do it? */

			/* Send read request to owners other than mod for all sub-blocks. */
			for (int z = 0; z < dir->zsize; z++)
			{
				struct net_node_t *node;

				dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
				dir_entry_tag = stack->tag + z * target_mod->sub_block_size;

				/* No owner */
				if (!DIR_ENTRY_VALID_OWNER(dir_entry))
					continue;

				/* Owner is mod */
				if (dir_entry->owner == mod->low_net_node->index)
					continue;

				/* Get owner mod */
				node = list_get(target_mod->high_net->node_list, dir_entry->owner);
				assert(node->kind == net_node_end);
				owner = node->user_data;
				assert(owner);

				/* Not the first sub-block */
				if (dir_entry_tag % owner->block_size)
					continue;

				/* Send read request */
				stack->pending++;
				new_stack = mod_stack_create(stack->id, target_mod, dir_entry_tag,
					EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH, stack, stack->prefetch);
				/* Only set peer if its a subblock that was requested */
				if (dir_entry_tag >= stack->addr &&
					dir_entry_tag < stack->addr + mod->block_size)
				{
					new_stack->peer = mod_stack_set_peer(mod, stack->state);
				}
				new_stack->target_mod = owner;
				new_stack->request_dir = mod_request_down_up;
				esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
			}

			/* Cache hit and ATD miss */
			if (stack->atd_hit == 0 && target_mod->kind == mod_kind_cache)
			{
				struct mod_t *low_mod = mod_get_low_mod(target_mod, stack->tag);
				assert(low_mod);
				new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
					ESIM_EV_NONE, stack, 0);
				new_stack->target_mod = low_mod;
				esim_schedule_event(EV_MOD_ATD_DELAYED_SET, new_stack, low_mod->latency);
			}

			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH, stack, 0);
		}
		else
		{
			/* State = I */
			assert(!dir_entry_group_shared_or_owned(target_mod->dir,
				stack->set, stack->way));
			new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
				EV_MOD_NMOESI_READ_REQUEST_UPDOWN_MISS, stack, stack->prefetch);
			/* Peer is NULL since we keep going up-down */
			new_stack->target_mod = mod_get_low_mod(target_mod, stack->tag);
			new_stack->retry = stack->retry; //VVV
			new_stack->stream_retried = stack->stream_retried;
			new_stack->stream_retried_cycle = stack->stream_retried_cycle;
			new_stack->request_dir = mod_request_up_down;
			new_stack->event_queue_item = stack->event_queue_item;
			new_stack->src_atd_hit = stack->src_atd_hit > 0 || stack->atd_hit > 0; /* ATD hit in this level or the previous one */
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
		}

		/* Stream buffers prefetching */
		prefetcher_update_tables(stack);
		if (prefetcher_uses_stream_buffers(target_cache->prefetcher))
		{
			if (stack->stream_hit)
				prefetcher_stream_buffer_hit(stack);
			else
				prefetcher_stream_buffer_miss(stack);
		}

		/* Cache prefetching */
		else
		{
			if (stack->state)
				prefetcher_cache_hit(stack, target_mod);
			else
				prefetcher_cache_miss(stack, target_mod); /* TODO: I'm not sure how relavant this is here for all states */
		}

		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_UPDOWN_MISS)
	{
		mem_debug("  %lld %lld 0x%x %s read request updown miss\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_updown_miss\"\n",
			stack->id, target_mod->name);

		/* Check error */
		if (stack->err)
		{
			assert(!stack->background);

			/* Unlock dir */
			dir_entry_unlock(target_mod->dir, stack->set, stack->way);
			if (stack->stream_hit)
				dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);

			/* Delete access */
			if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
				mod_access_finish(target_mod, stack);

			ret->err = 1;
			ret->retry |= 1 << target_mod->level;
			ret->stream_retried = stack->stream_retried;
			ret->stream_retried_cycle = stack->stream_retried_cycle;
			mod_stack_set_reply(ret, reply_ack_error);
			stack->reply_size = 8;

			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, 0);
			return;
		}

		/* Write block from write buffer to cache */
		if(stack->background)
		{
			struct write_buffer_block_t *wb_block = (struct write_buffer_block_t *) -1; /* Used as canary */
			struct cache_t *target_cache = target_mod->cache;
			assert(linked_list_count(target_cache->wb.blocks));
			LINKED_LIST_FOR_EACH(target_cache->wb.blocks)
			{
				wb_block = linked_list_get(target_cache->wb.blocks);
				if (wb_block->tag == stack->tag)
				{
					cache_set_block(target_cache, stack->set, stack->way, stack->tag, wb_block->state, stack->client_info);
					atd_set_block(target_mod->atd_per_thread[thread_id], stack->tag, wb_block->state);
					stack->state = wb_block->state;
					mod_stack_wake_up_write_buffer(wb_block);
					linked_list_remove(target_cache->wb.blocks);
					free(wb_block);
					wb_block = NULL;
					break;
				}
			}
			assert(!wb_block);
		}

		/* Move block from stream to cache */
		else if (stack->stream_hit)
		{
			struct stream_block_t *block = cache_get_pref_block(target_mod->cache, stack->pref_stream, stack->pref_slot);
			struct stream_buffer_t *sb = &target_mod->cache->prefetcher->streams[stack->pref_stream];
			assert(stack->tag == block->tag);
			cache_set_block(target_mod->cache, stack->set, stack->way, block->tag, block->state, stack->client_info);
			atd_set_block(target_mod->atd_per_thread[thread_id], block->tag, block->state);
			stack->state = block->state;
			block->state = cache_block_invalid;
			block->tag = -1;
			sb->count--; //COUNT
			if (stack->stream_head_hit)
				sb->head = (sb->head + 1) % target_mod->cache->prefetcher->max_num_slots; //HEAD
		}

		/* Write block coming from lower level to cache */
		else
		{
			/* Set block state to excl/shared depending on the return value 'shared'
			* that comes from a read request into the next cache level.
			* Also set the tag of the block. */
			cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive, stack->client_info);

			/* Cache miss and ATD miss*/
			if (stack->atd_hit == 0)
			{
				assert (stack->hit == 0);
				atd_set_block(target_mod->atd_per_thread[thread_id], stack->tag, stack->shared ? cache_block_shared : cache_block_exclusive);
			}

			mem_debug("  	%lld %lld 0x%x %s read request updown miss change state %d\n", esim_time, stack->id,
			stack->tag, target_mod->name,stack->shared ? cache_block_shared : cache_block_exclusive);
		}

		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH)
	{
		/* Ensure that a reply was received */
		assert(stack->reply);

		/* Ignore while pending requests */
		assert(stack->pending > 0);
		stack->pending--;
		if (stack->pending)
			return;

		/* Trace */
		mem_debug("  %lld %lld 0x%x %s read request updown finish\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_updown_finish\"\n",
			stack->id, target_mod->name);

		if(!stack->background)
		{
			/* If blocks were sent directly to the peer, the reply size would
			 * have been decreased.  Based on the final size, we can tell whether
			 * to send more data or simply ack */
			if (stack->reply_size == 8)
				mod_stack_set_reply(ret, reply_ack);
			else if (stack->reply_size > 8)
				mod_stack_set_reply(ret, reply_ack_data);
			else
				fatal("Invalid reply size: %d", stack->reply_size);
		}

		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_UPDOWN_LATENCY, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_UPDOWN_LATENCY)
	{
		int shared;

		mem_debug("  %lld %lld 0x%x %s read request updown latency\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_updown_latency\"\n",
			stack->id, target_mod->name);

		dir = target_mod->dir;

		/* If another module has not given the block, access main memory */
		if (target_mod->kind == mod_kind_main_memory &&
			target_mod->dram_system &&
			stack->request_dir == mod_request_up_down &&
			!stack->main_memory_accessed &&
			stack->reply != reply_ack_data_sent_to_peer)
		{
			struct dram_system_t *ds = target_mod->dram_system;
			struct x86_ctx_t *ctx = stack->client_info->ctx;
			assert(ctx);
			assert(ds);

			if (!dram_system_will_accept_trans(ds->handler, stack->addr))
			{
				stack->err = 1;
				ret->err = 1;
				ret->retry |= 1 << target_mod->level;
				mod_stack_set_reply(ret, reply_ack_error);
				stack->reply_size = 8;
				dir_entry_unlock(dir, stack->set, stack->way);

				mem_debug("    %lld 0x%x %s mc queue full, retrying...\n", stack->id, stack->tag, target_mod->name);

				esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, 0);

				return;
			}

			/* Access main memory system */
			mem_debug("  %lld %lld 0x%x %s dram access enqueued\n", esim_time, stack->id, stack->tag, stack->target_mod->dram_system->name);
			linked_list_add(ds->pending_reads, stack);
			dram_system_add_read_trans(ds->handler, stack->addr, stack->client_info->core, stack->client_info->thread);

			/* Ctx main memory stats */
			ctx->mm_read_accesses++;
			if (stack->prefetch)
				ctx->mm_pref_accesses++;
			return;
		}

		shared = 0;
		/* With the Owned state, the directory entry may remain owned by the sender */
		if (!stack->retain_owner)
		{
			/* Set owner to 0 for all directory entries not owned by mod. */
			for (int z = 0; z < dir->zsize; z++)
			{
				dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
				if (dir_entry->owner != mod->low_net_node->index)
					dir_entry_set_owner(dir, stack->set, stack->way, z, DIR_ENTRY_OWNER_NONE);
			}
		}

		/* For each sub-block requested by mod, set mod as sharer, and
		 * check whether there is other cache sharing it. */
		for (int z = 0; z < dir->zsize; z++)
		{
			dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
			if (dir_entry_tag < stack->addr || dir_entry_tag >= stack->addr + mod->block_size)
				continue;
			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
			dir_entry_set_sharer(dir, stack->set, stack->way, z, mod->low_net_node->index);
			if (dir_entry->num_sharers > 1 || stack->nc_write || stack->shared)
				shared = 1;

			/* If the block is owned, non-coherent, or shared,
			 * mod (the higher-level cache) should never be exclusive */
			if (stack->state == cache_block_owned ||
				stack->state == cache_block_noncoherent ||
				stack->state == cache_block_shared)
				shared = 1;
		}

		dir_entry_unlock(dir, stack->set, stack->way);

		if (stack->stream_hit)
		{
			assert(!stack->background);
			dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);
		}

		/* If no sub-block requested by mod is shared by other cache, set mod
		 * as owner of all of them. Otherwise, notify requester that the block is
		 * shared by setting the 'shared' return value to true. */
		if (ret)
			ret->shared = shared;
		if (!shared)
		{
			for (int z = 0; z < dir->zsize; z++)
			{
				dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
				if (dir_entry_tag < stack->addr || dir_entry_tag >= stack->addr + mod->block_size)
					continue;
				dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
				dir_entry_set_owner(dir, stack->set, stack->way, z, mod->low_net_node->index);
			}
		}

		/* A background stack has no more work to do */
		if(stack->background)
		{
			if(stack->client_info)
				mod_client_info_free(stack->target_mod, stack->client_info);
			mod_stack_return(stack);
			return;
		}

		/* Delete access */
		if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
			mod_access_finish(target_mod, stack);

		int latency = stack->reply == reply_ack_data_sent_to_peer || stack->main_memory_accessed ? 0 : target_mod->latency;
		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, latency);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_DOWNUP)
	{
		struct mod_t *owner;

		mem_debug("  %lld %lld 0x%x %s read request downup\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_downup\"\n",
			stack->id, target_mod->name);

		if (stack->stream_hit)
			cache_get_pref_block_data(target_mod->cache, stack->pref_stream, stack->pref_slot, NULL, &stack->state);

		/* Check: state must not be invalid or shared.
		 * By default, only one pending request.
		 * Response depends on state */
		//assert(stack->state != cache_block_invalid);
		assert(stack->state != cache_block_shared);
		assert(stack->state != cache_block_noncoherent);
		stack->pending = 1;

		/* Block was in streams but was silently evicted so there is nothing to send */
		if(stack->state == cache_block_invalid)
			stack->peer = NULL;

		/* Si ha hagut stream_hit, el bloc esta en el buffer de prefetch, per tant,
		 * ninguna caché en un nivell superior pot tenir el bloc. Si ha hagut una fallada down up, tampoc. */
		if (stack->stream_hit || !stack->state)
		{
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS, stack, 0);
			return;
		}

		/* Send a read request to the owner of each subblock. */
		dir = target_mod->dir;
		for (int z = 0; z < dir->zsize; z++)
		{
			struct net_node_t *node;

			dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
			assert(dir_entry_tag < stack->tag + target_mod->block_size);
			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);

			/* No owner */
			if (!DIR_ENTRY_VALID_OWNER(dir_entry))
				continue;

			/* Get owner mod */
			node = list_get(target_mod->high_net->node_list, dir_entry->owner);
			assert(node && node->kind == net_node_end);
			owner = node->user_data;

			/* Not the first sub-block */
			if (dir_entry_tag % owner->block_size)
				continue;

			stack->pending++;
			new_stack = mod_stack_create(stack->id, target_mod, dir_entry_tag,
				EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS, stack, stack->prefetch);
			new_stack->target_mod = owner;
			new_stack->request_dir = mod_request_down_up;
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST, new_stack, 0);
		}

		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS)
	{
		/* Ignore while pending requests */
		assert(stack->pending > 0);
		stack->pending--;
		if (stack->pending)
			return;

		mem_debug("  %lld %lld 0x%x %s read request downup wait for reqs\n",
			esim_time, stack->id, stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_downup_wait_for_reqs\"\n",
			stack->id, target_mod->name);

		if (stack->peer)
		{
			/* Send this block (or subblock) to the peer */
			new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
				EV_MOD_NMOESI_READ_REQUEST_DOWNUP_FINISH, stack, stack->prefetch);
			new_stack->peer = mod_stack_set_peer(stack->peer, stack->state);
			new_stack->target_mod = stack->target_mod;
			esim_schedule_event(EV_MOD_NMOESI_PEER_SEND, new_stack, 0);
		}
		else
		{
			/* No data to send to peer, so finish */
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_DOWNUP_FINISH, stack, 0);
		}

		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_DOWNUP_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s read request downup finish\n",
			esim_time, stack->id, stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_downup_finish\"\n",
			stack->id, target_mod->name);

		if (stack->reply == reply_ack_data)
		{
			/* If data was received, it was owned or modified by a higher level cache.
			 * We need to continue to propagate it up until a peer is found */

			if (stack->peer)
			{
				/* Peer was found, so this directory entry should be changed
				 * to owned */
				cache_set_block(target_mod->cache, stack->set, stack->way,
					stack->tag, cache_block_owned, stack->client_info);

				/* Higher-level cache changed to shared, set owner of
				 * sub-blocks to NONE. */
				dir = target_mod->dir;
				for (int z = 0; z < dir->zsize; z++)
				{
					dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
					assert(dir_entry_tag < stack->tag + target_mod->block_size);
					dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
					dir_entry_set_owner(dir, stack->set, stack->way, z,
						DIR_ENTRY_OWNER_NONE);
				}

				stack->reply_size = 8;
				mod_stack_set_reply(ret, reply_ack_data_sent_to_peer);

				/* Decrease the amount of data that mod will have to send back
				 * to its higher level cache */
				ret->reply_size -= target_mod->block_size;
				assert(ret->reply_size >= 8);

				/* Let the lower-level cache know not to delete the owner */
				ret->retain_owner = 1;
			}
			else
			{
				/* Set state to shared */
				cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_shared, stack->client_info);

				/* State is changed to shared, set owner of sub-blocks to 0. */
				dir = target_mod->dir;
				for (int z = 0; z < dir->zsize; z++)
				{
					dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
					assert(dir_entry_tag < stack->tag + target_mod->block_size);
					dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
					dir_entry_set_owner(dir, stack->set, stack->way, z,
						DIR_ENTRY_OWNER_NONE);
				}

				stack->reply_size = target_mod->block_size + 8;
				mod_stack_set_reply(ret, reply_ack_data);
			}
		}
		else if (stack->reply == reply_ack)
		{
			/* Higher-level cache was exclusive with no modifications above it */
			stack->reply_size = 8;

			/* Set state to shared */
			cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_shared, stack->client_info);

			/* State is changed to shared, set owner of sub-blocks to 0. */
			dir = target_mod->dir;
			for (int z = 0; z < dir->zsize; z++)
			{
				dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
				assert(dir_entry_tag < stack->tag + target_mod->block_size);
				dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
				dir_entry_set_owner(dir, stack->set, stack->way, z,
					DIR_ENTRY_OWNER_NONE);
			}

			if (stack->peer)
			{
				stack->reply_size = 8;
				mod_stack_set_reply(ret, reply_ack_data_sent_to_peer);

				/* Decrease the amount of data that mod will have to send back
				 * to its higher level cache */
				ret->reply_size -= target_mod->block_size;
				assert(ret->reply_size >= 8);
			}
			else
			{
				mod_stack_set_reply(ret, reply_ack);
				stack->reply_size = 8;
			}
		}
		else if (stack->reply == reply_none)
		{
			/* This block is not present in any higher level caches */

			if (stack->peer)
			{
				stack->reply_size = 8;
				mod_stack_set_reply(ret, reply_ack_data_sent_to_peer);

				/* Decrease the amount of data that mod will have to send back
				 * to its higher level cache */
				ret->reply_size -= target_mod->sub_block_size;
				assert(ret->reply_size >= 8);

				if (stack->stream_hit)
				{
					/* Set prefetched block to shared */
					struct stream_block_t *block = cache_get_pref_block(target_mod->cache, stack->pref_stream, stack->pref_slot);
					block->state = cache_block_shared;
				}
				else if (stack->state == cache_block_modified ||
					stack->state == cache_block_owned)
				{
					/* Let the lower-level cache know not to delete the owner */
					ret->retain_owner = 1;

					/* Set block to owned */
					cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_owned, stack->client_info);
				}
				else
				{
					/* Set block to shared */
					cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_shared, stack->client_info);
				}
			}
			else
			{
				if (stack->state == cache_block_exclusive ||
					stack->state == cache_block_shared)
				{
					stack->reply_size = 8;
					mod_stack_set_reply(ret, reply_ack);

				}
				else if (stack->state == cache_block_owned ||
					stack->state == cache_block_modified ||
					stack->state == cache_block_noncoherent)
				{
					/* No peer exists, so data is returned to mod */
					stack->reply_size = target_mod->sub_block_size + 8;
					mod_stack_set_reply(ret, reply_ack_data);
				}
				else if(stack->state == cache_block_invalid)
				{
					stack->reply_size = 8;
					mod_stack_set_reply(ret, reply_ack);

					/* If down up miss, remove sharer and owner */
					dir = mod->dir;
					for (int z = 0; z < dir->zsize; z++)
					{
						/* Skip other subblocks */
						dir_entry_tag = (stack->tag & ~mod->cache->block_mask) + z * mod->sub_block_size;
						assert(dir_entry_tag < (stack->tag & ~mod->cache->block_mask) + mod->block_size);
						if (dir_entry_tag != stack->tag)
							continue;

						/* We want the set and way of mod cache, not of target_mod cache.
						 * In stack we have the values for target_mod, so we must use ret. */
						dir_entry = dir_entry_get(dir, ret->set, ret->way, z);
						dir_entry_clear_sharer(dir, ret->set, ret->way, z, target_mod->low_net_node->index);
						if (dir_entry->owner == target_mod->low_net_node->index)
						{
							dir_entry_set_owner(dir, ret->set, ret->way, z,
								DIR_ENTRY_OWNER_NONE);
						}
					}
				}
				else
				{
					fatal("Invalid cache block state: %d\n", stack->state);
				}

				/* Set block state */
				if (stack->stream_hit)
				{
					/* TODO: usar una funció */
					struct stream_block_t * block = cache_get_pref_block(
						target_mod->cache, stack->pref_stream, stack->pref_slot);
					block->state = cache_block_shared;
				}
				else if(stack->state)
				{
					cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_shared, stack->client_info);
				}
			}
		}
		else
		{
			fatal("Unexpected reply type: %d\n", stack->reply);
		}

		if(stack->state)
		{
			if (stack->stream_hit)
				dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);
			else
				dir_entry_unlock(target_mod->dir, stack->set, stack->way);
		}

		int latency = stack->reply == reply_ack_data_sent_to_peer ? 0 : target_mod->latency;
		esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_REPLY, stack, latency);
		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_REPLY)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s read request reply to %s\n", esim_time, stack->id,
			stack->tag, target_mod->name, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_reply\"\n",
			stack->id, target_mod->name);

		/* Checks */
		assert(stack->reply_size);
		assert(stack->request_dir);
		assert(mod_get_low_mod(mod, stack->addr) == target_mod ||
			mod_get_low_mod(target_mod, stack->addr) == mod);

		/* Get network and nodes */
		if (stack->request_dir == mod_request_up_down)
		{
			net = mod->low_net;
			src_node = target_mod->high_net_node;
			dst_node = mod->low_net_node;
		}
		else
		{
			net = mod->high_net;
			src_node = target_mod->low_net_node;
			dst_node = mod->high_net_node;
		}

		/* Send message */
		stack->msg = net_try_send_ev(net, src_node, dst_node, stack->reply_size,
			EV_MOD_NMOESI_READ_REQUEST_FINISH, stack, event, stack);

		return;
	}

	if (event == EV_MOD_NMOESI_READ_REQUEST_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s read request finish\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:read_request_finish\"\n",
			stack->id, mod->name);

		/* Receive message */
		if (stack->request_dir == mod_request_up_down)
			net_receive_msg(mod->low_net, mod->low_net_node, stack->msg);
		else
			net_receive_msg(mod->high_net, mod->high_net_node, stack->msg);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_write_request(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct mod_t *target_mod = stack->target_mod;

	struct cache_t *target_cache = target_mod->cache;

	struct dir_t *dir;
	struct dir_entry_t *dir_entry;

	unsigned int dir_entry_tag;

	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;

	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_WRITE_REQUEST)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s write request\n", esim_time, stack->id,
			stack->addr, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request\"\n",
			stack->id, mod->name);

		/* Default return values */
		ret->err = 0;

		/* For write requests, we need to set the initial reply size because
		 * in updown, peer transfers must be allowed to decrease this value
		 * (during invalidate). If the request turns out to be downup, then
		 * these values will get overwritten. */
		stack->reply_size = mod->block_size + 8;
		mod_stack_set_reply(stack, reply_ack_data);

		/* Checks */
		assert(stack->request_dir);
		assert(mod_get_low_mod(mod, stack->addr) == target_mod ||
			stack->request_dir == mod_request_down_up);
		assert(mod_get_low_mod(target_mod, stack->addr) == mod ||
			stack->request_dir == mod_request_up_down);

		/* Get source and destination nodes */
		if (stack->request_dir == mod_request_up_down)
		{
			net = mod->low_net;
			src_node = mod->low_net_node;
			dst_node = target_mod->high_net_node;
		}
		else
		{
			net = mod->high_net;
			src_node = mod->high_net_node;
			dst_node = target_mod->low_net_node;
		}

		/* Send message */
		stack->msg = net_try_send_ev(net, src_node, dst_node, 8,
			EV_MOD_NMOESI_WRITE_REQUEST_RECEIVE, stack, event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s write request receive\n", esim_time, stack->id,
			stack->addr, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_receive\"\n",
			stack->id, target_mod->name);

		/* Receive message */
		if (stack->request_dir == mod_request_up_down)
			net_receive_msg(target_mod->high_net, target_mod->high_net_node, stack->msg);
		else
			net_receive_msg(target_mod->low_net, target_mod->low_net_node, stack->msg);

		if (stack->request_dir == mod_request_up_down &&
			target_mod->kind != mod_kind_main_memory &&
			target_mod->cache->prefetcher)
		{
			/* Record access */
			mod_access_start(target_mod, stack, mod_access_write_request);
		}

		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_LOCK, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_LOCK)
	{
		mem_debug("  %lld %lld 0x%x %s write request lock\n", esim_time, stack->id,
			stack->addr, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_lock\"\n",
			stack->id, target_mod->name);

		/* Call find and lock */
		new_stack = mod_stack_create(stack->id, target_mod, stack->addr,
			EV_MOD_NMOESI_WRITE_REQUEST_ACTION, stack, stack->prefetch);
		new_stack->blocking = stack->request_dir == mod_request_down_up;
		new_stack->write = 1;
		new_stack->retry = stack->retry; //VVV
		new_stack->background = stack->background;
		new_stack->stream_retried = stack->stream_retried;
		new_stack->stream_retried_cycle = stack->stream_retried_cycle;
		new_stack->request_dir = stack->request_dir;
		new_stack->access_kind = mod_access_write_request;
		new_stack->event_queue_item = stack->event_queue_item;
		new_stack->src_atd_hit = stack->src_atd_hit; /* An ATD hit in a given level should not be an ATD hit in a deeper level */
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_ACTION)
	{
		mem_debug("  %lld %lld 0x%x %s write request action\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_action\"\n",
			stack->id, target_mod->name);

		/* Check lock error. If write request is down-up, there should
		 * have been no error. */
		if (stack->err)
		{
			assert(stack->request_dir == mod_request_up_down);

			/* If this is a background stack ret must be null */
			if(stack->background)
			{
				assert(!ret);
				esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_LOCK, stack, 0);
				return;
			}

			ret->err = 1;
			ret->retry |= 1 << target_mod->level;
			ret->stream_retried = stack->stream_retried;
			ret->stream_retried_cycle = stack->stream_retried_cycle;
			stack->reply_size = 8;

			/* Delete access */
			if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
				mod_access_finish(target_mod, stack);

			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, 0);
			return;
		}

		/* If fast resumed access send block */
		if (stack->fast_resume)
		{
			assert(target_mod->kind != mod_kind_main_memory);

			/* Delete access */
			if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
				mod_access_finish(target_mod, stack);

			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, target_mod->latency);
			return;
		}

		/* If stream_hit, background stack or down up miss (silent eviction of stream block) there aren't any upper level sharers of the block */
		if (stack->stream_hit || stack->background ||
			(!stack->stream_hit && !stack->state && stack->request_dir == mod_request_down_up))
		{
			mem_debug("  %lld %lld 0x%x %s write prefetch hit direction %d\n",
				esim_time, stack->id, stack->tag, target_mod->name,
				stack->request_dir == mod_request_up_down);
			assert(target_mod->kind != mod_kind_main_memory);

			/* Block in write buffer */
			if(stack->background)
			{
				struct write_buffer_block_t *wb_block = NULL;
				struct cache_t *target_cache = target_mod->cache;
				assert(linked_list_count(target_cache->wb.blocks));
				LINKED_LIST_FOR_EACH(target_cache->wb.blocks)
				{
					wb_block = linked_list_get(target_cache->wb.blocks);
					if (wb_block->tag == stack->tag)
					{
						stack->state = wb_block->state;
						break;
					}
				}
				assert(wb_block);
				assert(wb_block->tag == stack->tag);
			}

			/* Stream hit */
			else if(stack->stream_hit)
			{
				int tag;
				cache_get_pref_block_data(target_mod->cache, stack->pref_stream, stack->pref_slot,
					&tag, &stack->state);
				assert(stack->tag == tag);
			}

			/* Else, block is a down up miss due a silent eviction in a stream */

			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_EXCLUSIVE, stack, 0);
			return;
		}

		/* Invalidate the rest of upper level sharers */
		new_stack = mod_stack_create(stack->id, target_mod, 0,
			EV_MOD_NMOESI_WRITE_REQUEST_EXCLUSIVE, stack, stack->prefetch);
		new_stack->except_mod = mod;
		new_stack->set = stack->set;
		new_stack->way = stack->way;
		new_stack->peer = mod_stack_set_peer(stack->peer, stack->state);
		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_EXCLUSIVE)
	{
		mem_debug("  %lld %lld 0x%x %s write request exclusive\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_exclusive\"\n",
			stack->id, target_mod->name);

		if (stack->request_dir == mod_request_up_down)
			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN, stack, 0);
		else
			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN)
	{
		mem_debug("  %lld %lld 0x%x %s write request updown\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_updown\"\n",
			stack->id, target_mod->name);

		/* state = M/E */
		if (stack->state == cache_block_modified ||
			stack->state == cache_block_exclusive ||
			stack->background)
		{
			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_FINISH, stack, 0);
		}

		/* state = O/S/I/N */
		else if (stack->state == cache_block_owned || stack->state == cache_block_shared ||
			stack->state == cache_block_invalid || stack->state == cache_block_noncoherent)
		{
			new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
				EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_FINISH, stack, stack->prefetch);
			new_stack->peer = mod_stack_set_peer(mod, stack->state);
			new_stack->target_mod = mod_get_low_mod(target_mod, stack->tag);
			new_stack->retry = stack->retry; //VVV
			new_stack->stream_retried = stack->stream_retried;
			new_stack->stream_retried_cycle = stack->stream_retried_cycle;
			new_stack->request_dir = mod_request_up_down;
			new_stack->event_queue_item = stack->event_queue_item;
			new_stack->src_atd_hit = stack->src_atd_hit > 0 || stack->atd_hit > 0; /* ATD hit in this level or the previous one */
			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST, new_stack, 0);
		}
		else
		{
			fatal("Invalid cache block state: %d\n", stack->state);
		}

		/* Stream buffer prefetching */
		prefetcher_update_tables(stack);
		if (prefetcher_uses_stream_buffers(target_cache->prefetcher))
		{
			if (stack->stream_hit)
				prefetcher_stream_buffer_hit(stack);
			else
				prefetcher_stream_buffer_miss(stack); /* TODO: I'm not sure how relavant this is here for all states */
		}

		/* Cache prefetching */
		else
		{
			if (stack->state)
				prefetcher_cache_hit(stack, target_mod);
			else
				prefetcher_cache_miss(stack, target_mod);
		}

		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s write request updown finish\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_updown_finish\"\n",
			stack->id, target_mod->name);

		/* Ensure that a reply was received */
		assert(stack->reply);

		/* Error in write request to next cache level */
		if (stack->err)
		{
			assert(!stack->background);

			/* Unlock dirs */
			dir_entry_unlock(target_mod->dir, stack->set, stack->way);
			if (stack->stream_hit)
				dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);

			/* Delete access */
			if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
				mod_access_finish(target_mod, stack);

			ret->err = 1;
			ret->retry |= 1 << target_mod->level;
			ret->stream_retried = stack->stream_retried;
			ret->stream_retried_cycle = stack->stream_retried_cycle;
			mod_stack_set_reply(ret, reply_ack_error);
			stack->reply_size = 8;

			esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, 0);
			return;
		}

		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_LATENCY, stack, 0);

		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_LATENCY)
	{
		mem_debug("  %lld %lld 0x%x %s write request updown latency\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_updown_latency\"\n",
			stack->id, target_mod->name);

		/* If another module has not given the block,
		access main memory */
		if (target_mod->kind == mod_kind_main_memory &&
				target_mod->dram_system &&
				stack->request_dir == mod_request_up_down &&
				!stack->main_memory_accessed &&
				stack->reply != reply_ack_data_sent_to_peer)
		{
			struct dram_system_t *ds = target_mod->dram_system;
			struct x86_ctx_t *ctx = stack->client_info->ctx;
			assert(ds);
			assert(ctx);

			if (!dram_system_will_accept_trans(ds->handler, stack->addr))
			{
				stack->err = 1;
				ret->err = 1;
				ret->retry |= 1 << target_mod->level;

				mod_stack_set_reply(ret, reply_ack_error);
				stack->reply_size = 8;

				dir_entry_unlock(target_mod->dir, stack->set, stack->way);

				mem_debug("    %lld 0x%x %s mc queue full, retrying...\n", stack->id, stack->tag, target_mod->name);

				esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, 0);
				return;
			}

			/* Access main memory system */
			mem_debug("  %lld %lld 0x%x %s dram access enqueued\n", esim_time, stack->id, stack->tag, stack->target_mod->dram_system->name);
			linked_list_add(ds->pending_reads, stack);
			dram_system_add_read_trans(ds->handler, stack->addr, stack->client_info->core, stack->client_info->thread);

			/* Ctx main memory stats */
			assert(!stack->prefetch);
			ctx->mm_read_accesses++;

			return;
		}

		/* Check that addr is a multiple of mod->block_size.
		 * Set mod as sharer and owner. */
		dir = target_mod->dir;
		for (int z = 0; z < dir->zsize; z++)
		{
			assert(stack->addr % mod->block_size == 0);
			dir_entry_tag = stack->tag + z * target_mod->sub_block_size;
			assert(dir_entry_tag < stack->tag + target_mod->block_size);
			if (dir_entry_tag < stack->addr || dir_entry_tag >= stack->addr + mod->block_size)
				continue;
			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
			dir_entry_set_sharer(dir, stack->set, stack->way, z, mod->low_net_node->index);
			dir_entry_set_owner(dir, stack->set, stack->way, z, mod->low_net_node->index);
			assert(dir_entry->num_sharers == 1);
		}

		/* Remove block from write buffer */
		if(stack->background)
		{
			struct write_buffer_block_t *wb_block = (struct write_buffer_block_t *) -1;
			struct cache_t *target_cache = target_mod->cache;
			assert(linked_list_count(target_cache->wb.blocks));
			LINKED_LIST_FOR_EACH(target_cache->wb.blocks)
			{
				wb_block = linked_list_get(target_cache->wb.blocks);
				if (wb_block->tag == stack->tag)
				{
					mod_stack_wake_up_write_buffer(wb_block);
					linked_list_remove(target_cache->wb.blocks);
					free(wb_block);
					wb_block = NULL;
					break;
				}
			}
			assert(!wb_block);
		}

		/* Remove block from stream */
		else if (stack->stream_hit)
		{
			cache_set_pref_block(target_mod->cache, stack->pref_stream, stack->pref_slot, -1, cache_block_invalid);
			struct stream_buffer_t *sb = &target_mod->cache->prefetcher->streams[stack->pref_stream];
			sb->count--; //COUNT
			if (stack->stream_head_hit)
				sb->head = (sb->head + 1) % target_mod->cache->prefetcher->max_num_slots; //HEAD
			dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);
		}

		/* Set states O/E/S/I->E */
		cache_set_block(target_mod->cache, stack->set, stack->way, stack->tag, cache_block_exclusive, stack->client_info);

		/* Cache hit and ATD miss */
		if (stack->hit && stack->atd_hit == 0 && target_mod->kind == mod_kind_cache)
		{
			struct mod_t *low_mod = mod_get_low_mod(target_mod, stack->tag);
			assert(low_mod);
			new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
				ESIM_EV_NONE, stack, 0);
			new_stack->target_mod = low_mod;
			esim_schedule_event(EV_MOD_ATD_DELAYED_SET, new_stack, low_mod->latency);
		}

		/* Otherwise */
		else
		{
			atd_set_block(target_mod->atd_per_thread[thread_id], stack->tag, cache_block_exclusive);
		}

		/* If blocks were sent directly to the peer, the reply size would
		 * have been decreased.  Based on the final size, we can tell whether
		 * to send more data up or simply ack */
		if(!stack->background)
		{
			if (stack->reply_size == 8)
				mod_stack_set_reply(ret, reply_ack);
			else if (stack->reply_size > 8)
				mod_stack_set_reply(ret, reply_ack_data);
			else
				fatal("Invalid reply size: %d", stack->reply_size);
		}


		/* Unlock, reply_size is the data of the size of the requester's block. */
		dir_entry_unlock(target_mod->dir, stack->set, stack->way);

		/* There is no more work for this stack */
		if (stack->background)
		{
			if(stack->client_info)
				mod_client_info_free(stack->target_mod, stack->client_info);
			mod_stack_return(stack);
			return;
		}

		/* Delete access */
		if (target_mod->cache->prefetcher && target_mod->kind != mod_kind_main_memory)
			mod_access_finish(target_mod, stack);

		int latency = stack->reply == reply_ack_data_sent_to_peer || stack->main_memory_accessed ? 0 : target_mod->latency;

		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, latency);

		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP)
	{
		mem_debug("  %lld %lld 0x%x %s write request downup\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_downup\"\n",
			stack->id, target_mod->name);

		//assert(stack->state != cache_block_invalid);
		assert(stack->stream_hit || stack->state == cache_block_invalid || // Stream hit or silent eviction
			!dir_entry_group_shared_or_owned(target_mod->dir, stack->set, stack->way));

		/* Compute reply size */
		if (stack->state == cache_block_exclusive ||
			stack->state == cache_block_shared ||
			stack->state == cache_block_invalid) //VVV
		{
			/* Exclusive and shared states send an ack */
			stack->reply_size = 8;
			mod_stack_set_reply(ret, reply_ack);
		}
		else if (stack->state == cache_block_noncoherent)
		{
			/* Non-coherent state sends data */
			stack->reply_size = target_mod->block_size + 8;
			mod_stack_set_reply(ret, reply_ack_data);
		}
		else if (stack->state == cache_block_modified ||
			stack->state == cache_block_owned)
		{
			if (stack->peer)
			{
				/* Modified or owned entries send data directly to peer
				 * if it exists */
				mod_stack_set_reply(ret, reply_ack_data_sent_to_peer);
				stack->reply_size = 8;

				/* This control path uses an intermediate stack that disappears, so
				 * we have to update the return stack of the return stack */
				ret->ret_stack->reply_size -= target_mod->block_size;
				assert(ret->ret_stack->reply_size >= 8);

				/* Send data to the peer */
				new_stack = mod_stack_create(stack->id, target_mod, stack->tag,
					EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP_FINISH, stack, stack->prefetch);
				new_stack->peer = mod_stack_set_peer(stack->peer, stack->state);
				new_stack->target_mod = stack->target_mod;

				esim_schedule_event(EV_MOD_NMOESI_PEER_SEND, new_stack, 0);
				return;
			}
			else
			{
				/* If peer does not exist, data is returned to mod */
				mod_stack_set_reply(ret, reply_ack_data);
				stack->reply_size = target_mod->block_size + 8;
			}
		}
		else
		{
			fatal("Invalid cache block state: %d\n", stack->state);
		}

		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP_FINISH, stack, 0);

		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s write request downup complete\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_downup_finish\"\n",
			stack->id, target_mod->name);

		/* Set state to I, unlock */
		if (stack->stream_hit)
		{
			cache_set_pref_block(target_mod->cache, stack->pref_stream, stack->pref_slot, -1, cache_block_invalid);
			struct stream_buffer_t *sb = &target_mod->cache->prefetcher->streams[stack->pref_stream];
			sb->count--; //COUNT
			dir_pref_entry_unlock(target_mod->dir, stack->pref_stream, stack->pref_slot);
			/* If we remove head, we must increment it */
			if (stack->pref_slot == sb->head)
				sb->head = (stack->pref_slot + 1) % target_mod->cache->prefetcher->max_num_slots; //HEAD
		}
		else if(stack->state) /* Sols fem això si no ha hagut un down_up miss degut als streams */
		{
			cache_set_block(target_mod->cache, stack->set, stack->way, 0, cache_block_invalid, stack->client_info);
			dir_entry_unlock(target_mod->dir, stack->set, stack->way);
		}

		int latency = ret->reply == reply_ack_data_sent_to_peer ? 0 : target_mod->latency;
		esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST_REPLY, stack, latency);
		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_REPLY)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s write request reply\n", esim_time, stack->id,
			stack->tag, target_mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_reply\"\n",
			stack->id, target_mod->name);

		/* Checks */
		assert(stack->reply_size);
		assert(mod_get_low_mod(mod, stack->addr) == target_mod ||
			mod_get_low_mod(target_mod, stack->addr) == mod);

		/* Get network and nodes */
		if (stack->request_dir == mod_request_up_down)
		{
			net = mod->low_net;
			src_node = target_mod->high_net_node;
			dst_node = mod->low_net_node;
		}
		else
		{
			net = mod->high_net;
			src_node = target_mod->low_net_node;
			dst_node = mod->high_net_node;
		}

		stack->msg = net_try_send_ev(net, src_node, dst_node, stack->reply_size,
			EV_MOD_NMOESI_WRITE_REQUEST_FINISH, stack, event, stack);

		return;
	}

	if (event == EV_MOD_NMOESI_WRITE_REQUEST_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s write request finish\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:write_request_finish\"\n",
			stack->id, mod->name);

		/* Receive message */
		if (stack->request_dir == mod_request_up_down)
			net_receive_msg(mod->low_net, mod->low_net_node, stack->msg);
		else
			net_receive_msg(mod->high_net, mod->high_net_node, stack->msg);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_peer(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_t *src = stack->target_mod;
	struct mod_t *peer = stack->peer;

	if (event == EV_MOD_NMOESI_PEER_SEND)
	{
		mem_debug("  %lld %lld 0x%x %s %s peer send\n", esim_time, stack->id,
			stack->tag, src->name, peer->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:peer\"\n",
			stack->id, src->name);

		/* Send message from src to peer */
		stack->msg = net_try_send_ev(src->low_net, src->low_net_node, peer->low_net_node,
			src->block_size + 8, EV_MOD_NMOESI_PEER_RECEIVE, stack, event, stack);

		return;
	}

	if (event == EV_MOD_NMOESI_PEER_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s %s peer receive\n", esim_time, stack->id,
			stack->tag, src->name, peer->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:peer_receive\"\n",
			stack->id, peer->name);

		/* Receive message from src */
		net_receive_msg(peer->low_net, peer->low_net_node, stack->msg);

		esim_schedule_event(EV_MOD_NMOESI_PEER_REPLY, stack, 0);

		return;
	}

	if (event == EV_MOD_NMOESI_PEER_REPLY)
	{
		mem_debug("  %lld %lld 0x%x %s %s peer reply ack\n", esim_time, stack->id,
			stack->tag, src->name, peer->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:peer_reply_ack\"\n",
			stack->id, peer->name);

		/* Send ack from peer to src */
		stack->msg = net_try_send_ev(peer->low_net, peer->low_net_node, src->low_net_node,
				8, EV_MOD_NMOESI_PEER_FINISH, stack, event, stack);

		return;
	}

	if (event == EV_MOD_NMOESI_PEER_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s %s peer finish\n", esim_time, stack->id,
			stack->tag, src->name, peer->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:peer_finish\"\n",
			stack->id, src->name);

		/* Receive message from src */
		net_receive_msg(src->low_net, src->low_net_node, stack->msg);

		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_invalidate(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;

	struct dir_t *dir;
	struct dir_entry_t *dir_entry;

	unsigned int dir_entry_tag;

	/* const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread; */

	assert(stack->client_info->core >= 0 && stack->client_info->core < x86_cpu_num_cores);
	assert(stack->client_info->thread >= 0 && stack->client_info->thread < x86_cpu_num_threads);

	if (event == EV_MOD_NMOESI_INVALIDATE)
	{
		struct mod_t *sharer;

		/* Get block info */
		cache_get_block(mod->cache, stack->set, stack->way, &stack->tag, &stack->state);
		mem_debug("  %lld %lld 0x%x %s invalidate (set=%d, way=%d, state=%s)\n", esim_time, stack->id,
			stack->tag, mod->name, stack->set, stack->way,
			str_map_value(&cache_block_state_map, stack->state));
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:invalidate\"\n",
			stack->id, mod->name);

		/* At least one pending reply */
		stack->pending = 1;

		/* Send write request to all upper level sharers except 'except_mod' */
		dir = mod->dir;
		for (int z = 0; z < dir->zsize; z++)
		{
			dir_entry_tag = stack->tag + z * mod->sub_block_size;
			assert(dir_entry_tag < stack->tag + mod->block_size);
			dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
			for (int i = 0; i < dir->num_nodes; i++)
			{
				struct net_node_t *node;

				/* Skip non-sharers and 'except_mod' */
				if (!dir_entry_is_sharer(dir, stack->set, stack->way, z, i))
					continue;

				node = list_get(mod->high_net->node_list, i);
				sharer = node->user_data;
				if (sharer == stack->except_mod)
					continue;

				/* Clear sharer and owner */
				dir_entry_clear_sharer(dir, stack->set, stack->way, z, i);
				if (dir_entry->owner == i)
					dir_entry_set_owner(dir, stack->set, stack->way, z, DIR_ENTRY_OWNER_NONE);

				/* Send write request upwards if beginning of block */
				if (dir_entry_tag % sharer->block_size)
					continue;
				new_stack = mod_stack_create(stack->id, mod, dir_entry_tag,
					EV_MOD_NMOESI_INVALIDATE_FINISH, stack, stack->prefetch);
				new_stack->target_mod = sharer;
				new_stack->request_dir = mod_request_down_up;

				esim_schedule_event(EV_MOD_NMOESI_WRITE_REQUEST, new_stack, 0);
				stack->pending++;
			}
		}
		esim_schedule_event(EV_MOD_NMOESI_INVALIDATE_FINISH, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_INVALIDATE_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s invalidate finish\n", esim_time, stack->id,
			stack->tag, mod->name);
		mem_trace("mem.access name=\"A-%lld\" state=\"%s:invalidate_finish\"\n",
			stack->id, mod->name);

		if (stack->reply == reply_ack_data)
			cache_set_block(mod->cache, stack->set, stack->way, stack->tag, cache_block_modified, stack->client_info);

		/* Ignore while pending */
		assert(stack->pending > 0);
		stack->pending--;
		if (stack->pending)
			return;
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_nmoesi_message(int event, void *data)
{
	struct mod_stack_t *stack = data;
	struct mod_stack_t *ret = stack->ret_stack;
	struct mod_stack_t *new_stack;

	struct mod_t *mod = stack->mod;
	struct mod_t *target_mod = stack->target_mod;

	struct dir_t *dir = NULL;
	struct dir_entry_t *dir_entry;

	if (event == EV_MOD_NMOESI_MESSAGE)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s message\n", esim_time, stack->id,
			stack->addr, mod->name);

		stack->reply_size = 8;
		stack->reply = reply_ack;

		/* Default return values*/
		ret->err = 0;

		/* Checks */
		assert(stack->message);

		/* Get source and destination nodes */
		net = mod->low_net;
		src_node = mod->low_net_node;
		dst_node = target_mod->high_net_node;

		/* Send message */
		stack->msg = net_try_send_ev(net, src_node, dst_node, 8,
			EV_MOD_NMOESI_MESSAGE_RECEIVE, stack, event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_MESSAGE_RECEIVE)
	{
		mem_debug("  %lld %lld 0x%x %s message receive\n", esim_time, stack->id,
			stack->addr, target_mod->name);

		/* Receive message */
		net_receive_msg(target_mod->high_net, target_mod->high_net_node, stack->msg);

		/* Find and lock */
		new_stack = mod_stack_create(stack->id, target_mod, stack->addr,
			EV_MOD_NMOESI_MESSAGE_ACTION, stack, stack->prefetch);
		new_stack->message = stack->message;
		new_stack->blocking = 0;
		new_stack->retry = 0;
		esim_schedule_event(EV_MOD_NMOESI_FIND_AND_LOCK, new_stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_MESSAGE_ACTION)
	{
		mem_debug("  %lld %lld 0x%x %s clear owner action\n", esim_time, stack->id,
			stack->tag, target_mod->name);

		assert(stack->message);

		/* Check block locking error. */
		mem_debug("stack err = %u\n", stack->err);
		if (stack->err)
		{
			ret->err = 1;
			mod_stack_set_reply(ret, reply_ack_error);
			esim_schedule_event(EV_MOD_NMOESI_MESSAGE_REPLY, stack, 0);
			return;
		}

		if (stack->message == message_clear_owner)
		{
			/* Remove owner */
			dir = target_mod->dir;
			for (int z = 0; z < dir->zsize; z++)
			{
				/* Skip other subblocks */
				if (stack->addr == stack->tag + z * target_mod->sub_block_size)
				{
					/* Clear the owner */
					dir_entry = dir_entry_get(dir, stack->set, stack->way, z);
					assert(dir_entry->owner == mod->low_net_node->index);
					dir_entry_set_owner(dir, stack->set, stack->way, z,
						DIR_ENTRY_OWNER_NONE);
				}
			}
		}
		else
		{
			fatal("Unexpected message");
		}

		/* Unlock the directory entry */
		dir_entry_unlock(dir, stack->set, stack->way);

		esim_schedule_event(EV_MOD_NMOESI_MESSAGE_REPLY, stack, 0);
		return;
	}

	if (event == EV_MOD_NMOESI_MESSAGE_REPLY)
	{
		struct net_t *net;
		struct net_node_t *src_node;
		struct net_node_t *dst_node;

		mem_debug("  %lld %lld 0x%x %s message reply\n", esim_time, stack->id,
			stack->tag, target_mod->name);

		/* Checks */
		assert(mod_get_low_mod(mod, stack->addr) == target_mod ||
			mod_get_low_mod(target_mod, stack->addr) == mod);

		/* Get network and nodes */
		net = mod->low_net;
		src_node = target_mod->high_net_node;
		dst_node = mod->low_net_node;

		/* Send message */
		stack->msg = net_try_send_ev(net, src_node, dst_node, stack->reply_size,
			EV_MOD_NMOESI_MESSAGE_FINISH, stack, event, stack);
		return;
	}

	if (event == EV_MOD_NMOESI_MESSAGE_FINISH)
	{
		mem_debug("  %lld %lld 0x%x %s message finish\n", esim_time, stack->id,
			stack->tag, mod->name);

		/* Receive message */
		net_receive_msg(mod->low_net, mod->low_net_node, stack->msg);

		/* Return */
		mod_stack_return(stack);
		return;
	}

	abort();
}


void mod_handler_atd_delayed_set(int event, void *data)
{
	struct mod_stack_t *stack = data;
	const int thread_id = stack->client_info->core * x86_cpu_num_threads + stack->client_info->thread;
	assert(event == EV_MOD_ATD_DELAYED_SET);
	atd_set_block(stack->mod->atd_per_thread[thread_id], stack->tag, cache_block_exclusive);
	mod_stack_return(stack);
}
