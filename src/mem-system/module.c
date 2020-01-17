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
#include <math.h>
#include <stdlib.h>
#include <limits.h>

#include <arch/x86/timing/cpu.h>
#include <arch/x86/emu/context.h>
#include <arch/x86/emu/emu.h>
#include <dramsim/bindings-c.h>
#include <lib/esim/esim.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/bloom.h>
#include <lib/util/debug.h>
#include <lib/util/file.h>
#include <lib/util/hash-table-gen.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>
#include <lib/util/line-writer.h>
#include <lib/util/misc.h>
#include <lib/util/repos.h>
#include <lib/util/stats.h>
#include <lib/util/string.h>

#include "atd.h"
#include "directory.h"
#include "local-mem-protocol.h"
#include "mem-system.h"
#include "mod-stack.h"
#include "nmoesi-protocol.h"
#include "prefetcher.h"


/* String map for access type */
struct str_map_t mod_access_kind_map =
{
	3, {
		{ "Load", mod_access_load },
		{ "Store", mod_access_store },
		{ "NCStore", mod_access_nc_store },
		{ "Prefetch", mod_access_prefetch }
	}
};



/* Event used for updating the state of adaptative prefetch policy */
int EV_MOD_ADAPT_PREF;

/* Max cache level across all archs */
int max_mod_level;


/*
 * Public Functions
 */


//Hugo creating a 'catch-all' structure to work with RTM
char *mod_RTM_type[] = {"Nulo","SL","SE","DL"};

struct RTM_data_t *RTM_data_create(int num_sets, int assoc, int headers)
{
	struct RTM_data_t *RTM_data = xcalloc(1,sizeof(struct RTM_data_t));

	int last_read_set = 0;
        int *headers_pos = xcalloc( headers,sizeof(int*));
	int **penalizations = xcalloc(assoc, sizeof(int*));
	
	for(int i = 0; i < assoc    ;i++)
	{
		
		penalizations[i] = xcalloc(num_sets,sizeof(int));
		
	}

	for(int i = 0; i<headers;i++)
	{
		headers_pos[i] = (num_sets/headers)/2 -1 + (num_sets/headers)*i;
	}
	
	for (int i = 0; i < assoc; i++)
		for (int j = 0; j < num_sets; j++)
			penalizations[i][j] = 0;

	/* Pointers*/	
	RTM_data->last_read_set = last_read_set;
	RTM_data->headers_pos = headers_pos;
	RTM_data->penalizations = penalizations;

	return RTM_data;

}

struct mod_t *mod_create(char *name, enum mod_kind_t kind, int num_ports,
	int block_size, int latency)
{
	struct mod_t *mod;


	/* Initialize */
	mod = xcalloc(1, sizeof(struct mod_t));
	mod->name = xstrdup(name);
	mod->kind = kind;
	mod->latency = latency;

	/* Ports */
	mod->num_ports = num_ports;
	mod->ports = xcalloc(num_ports, sizeof(struct mod_port_t));

	/* Lists */
	mod->low_mod_list = linked_list_create();
	mod->high_mod_list = linked_list_create();

	/* Block size */
	mod->block_size = block_size;
	assert(!(block_size & (block_size - 1)) && block_size >= 4);
	mod->log_block_size = log_base2(block_size);

	mod->client_info_repos = repos_create(sizeof(struct mod_client_info_t), mod->name);

	/* Alternate Tag Directory per thread */
	mod->atd_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(struct adt_t *));

	mod->reachable_threads = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(char));
	mod->reachable_mm_modules = list_create();

	mod->atd_hits_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	mod->atd_misses_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	mod->atd_unknown_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	mod->atd_intramisses_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	mod->atd_intermisses_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));

	mod->mc_id = -1; /* By default */
	/*Hugo initializating RTM_type*/
	//mod->RTM_type = RTM_type;


	return mod;
}


void mod_free(struct mod_t *mod)
{
	int core;
	int thread;

	linked_list_free(mod->low_mod_list);
	linked_list_free(mod->high_mod_list);

	cache_free(mod->cache);
	dir_free(mod->dir);

	free(mod->ports);
	repos_free(mod->client_info_repos);
	free(mod->name);

	/* Adaptative prefetch */
	if (mod->adapt_pref_stack)
		bloom_free(mod->adapt_pref_stack->pref_pollution_filter);
	free(mod->adapt_pref_stack);

	/* Interval report */
	if(mod->report_stack)
	{
		struct mod_report_stack_t *stack = mod->report_stack;

		hash_table_gen_free(stack->pref_pollution_filter);
		X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
		{
			int pos = core * x86_cpu_num_threads + thread;
			if (mod->reachable_threads[pos])
			{
				hash_table_gen_free(stack->dem_pollution_filter_per_thread[pos]);
				hash_table_gen_free(stack->pref_pollution_filter_per_thread[pos]);
			}
		}
		free(stack->dem_pollution_filter_per_thread);
		free(stack->pref_pollution_filter_per_thread);

		free(stack->hits_per_thread_int);
		free(stack->stream_hits_per_thread_int);
		free(stack->misses_per_thread_int);
		free(stack->retries_per_thread_int);
		free(stack->evictions_per_thread_int);
		free(stack->dem_pollution_per_thread_int);
		free(stack->pref_pollution_per_thread_int);
		free(stack->atd_hits_per_thread_int);
		free(stack->atd_misses_per_thread_int);
		free(stack->atd_unknown_per_thread_int);
		free(stack->atd_intramisses_per_thread_int);
		free(stack->atd_intermisses_per_thread_int);
		file_close(stack->report_file);
	}
	free(mod->report_stack);

	free(mod->reachable_threads);
	list_free(mod->reachable_mm_modules);

	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int thread_id = core * x86_cpu_num_threads + thread;
		atd_free(mod->atd_per_thread[thread_id]);
	}
	free(mod->atd_per_thread);
	free(mod->atd_hits_per_thread);
	free(mod->atd_misses_per_thread);
	free(mod->atd_unknown_per_thread);
	free(mod->atd_intramisses_per_thread);
	free(mod->atd_intermisses_per_thread);
	

	free(mod);
}


void mod_dump(struct mod_t *mod, FILE *f)
{
}


/* Access a memory module.
 * Variable 'witness', if specified, will be increased when the access completes.
 * The function returns a unique access ID.
 */
long long mod_access(struct mod_t *mod, enum mod_access_kind_t access_kind,
	unsigned int addr, int *witness_ptr, struct linked_list_t *event_queue,
	void *event_queue_item, struct mod_client_info_t *client_info)
{
	struct mod_stack_t *stack;
	int event = ESIM_EV_NONE;

	/* Create module stack with new ID */
	mod_stack_id++;
	stack = mod_stack_create(mod_stack_id,
		mod, addr, ESIM_EV_NONE, NULL, access_kind == mod_access_prefetch);

	/* Initialize */
	stack->witness_ptr = witness_ptr;
	stack->event_queue = event_queue;
	stack->event_queue_item = event_queue_item;
	stack->client_info = client_info;


	/* Select initial CPU/GPU event */
	if (mod->kind == mod_kind_cache || mod->kind == mod_kind_main_memory)
	{
		if (access_kind == mod_access_load)
		{
			event = EV_MOD_NMOESI_LOAD;
			assert(client_info->ctx);
		}
		else if (access_kind == mod_access_store)
		{
			event = EV_MOD_NMOESI_STORE;
		}
		else if (access_kind == mod_access_nc_store)
		{
			event = EV_MOD_NMOESI_NC_STORE;
		}
		else if (access_kind == mod_access_prefetch)
		{
			assert(mod->cache->prefetcher->type);
			if (prefetcher_uses_stream_buffers(mod->cache->prefetcher))
				event = EV_MOD_PREF;

			/* Prefetches go to cache */
			else
				event = EV_MOD_NMOESI_PREFETCH;
		}
		else if (access_kind == mod_access_invalidate_slot)
		{
			assert(prefetcher_uses_stream_buffers(mod->cache->prefetcher));
			event = EV_MOD_NMOESI_INVALIDATE_SLOT;
		}
		else
		{
			panic("%s: invalid access kind", __FUNCTION__);
		}
	}
	else if (mod->kind == mod_kind_local_memory)
	{
		if (access_kind == mod_access_load)
		{
			event = EV_MOD_LOCAL_MEM_LOAD;
		}
		else if (access_kind == mod_access_store)
		{
			event = EV_MOD_LOCAL_MEM_STORE;
		}
		else
		{
			panic("%s: invalid access kind", __FUNCTION__);
		}
	}
	else
	{
		panic("%s: invalid mod kind", __FUNCTION__);
	}

	/* Schedule */
	esim_execute_event(event, stack);

	/* Return access ID */
	return stack->id;
}


/* Return true if module can be accessed. */
int mod_can_access(struct mod_t *mod, unsigned int addr)
{
	int non_coalesced_accesses;

	/* There must be a free port */
	assert(mod->num_locked_ports <= mod->num_ports);
	if (mod->num_locked_ports == mod->num_ports)
		return 0;

	/* If no MSHR is given, module can be accessed */
	if (!mod->mshr_size)
		return 1;

	/* Module can be accessed if number of non-coalesced in-flight
	 * accesses is smaller than the MSHR size. */
	non_coalesced_accesses = mod->access_list_count -
		mod->access_list_coalesced_count;
	return non_coalesced_accesses < mod->mshr_size;
}


/* Search for a block in a stream and return the slot where the block is found or -1 if the block is not in the stream */
int mod_find_block_in_stream(struct mod_t *mod, unsigned int addr, int stream)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_buffer_t *sb = &pref->streams[stream];
	struct stream_block_t *block;
	int tag = addr & ~cache->block_mask;
	int i, slot, count;

	count = sb->head + pref->max_num_slots;
	for(i = sb->head; i < count; i++){
		slot = i % pref->max_num_slots;
		block = cache_get_pref_block(cache, sb->stream, slot);
		if(block->tag == tag && block->state)
			return slot;
	}
	return -1;
}


/* Return {set, way, tag, state} for an address.
 * The function returns TRUE on hit, FALSE on miss. */
int mod_find_block(struct mod_t *mod, unsigned int addr, int *set_ptr,
	int *way_ptr, int *tag_ptr, int *state_ptr)
{
	struct cache_t *cache = mod->cache;
	struct cache_block_t *blk,*mru_blk;
	struct dir_lock_t *dir_lock;
	

	int set = -1;
	int way = -1;
	int tag = -1;

	/* A transient tag is considered a hit if the block is
	 * locked in the corresponding directory. */
	tag = addr & ~cache->block_mask;
	if (mod->range_kind == mod_range_interleaved)
	{
		unsigned int num_mods = mod->range.interleaved.mod;
		set = ((tag >> cache->log_block_size) / num_mods) % cache->num_sets;
	}
	else if (mod->range_kind == mod_range_bounds)
	{
		set = (tag >> cache->log_block_size) % cache->num_sets;
	}
	else
	{
		panic("%s: invalid range kind (%d)", __FUNCTION__, mod->range_kind);
	}

	for (way = 0; way < cache->assoc; way++)
	{
		blk = &cache->sets[set].blocks[way];
		mru_blk = cache->sets[set].way_head;
		
		if (blk->tag == tag && blk->state){
			//Hugo
			if(blk->tag == mru_blk->tag){
				mod->mru_hits++;
			}
			//End of modification
			break;
		}
			
		if (blk->transient_tag == tag)
		{
			dir_lock = dir_lock_get(mod->dir, set, way);
			if (dir_lock->lock)
				break;
		}
	}

	PTR_ASSIGN(set_ptr, set);
	PTR_ASSIGN(tag_ptr, tag);

	/* Miss */
	if (way == cache->assoc)
		return 0;

	/* Hit */
	PTR_ASSIGN(way_ptr, way);
	PTR_ASSIGN(state_ptr, cache->sets[set].blocks[way].state);
	return 1;
}


/* Look for a block in prefetch buffer.
 * The function returns 0 on miss, 1 if hit on head and 2 if hit in the middle of the stream. */
int mod_find_pref_block(struct mod_t *mod, unsigned int addr, int *pref_stream_ptr, int *pref_slot_ptr)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct stream_block_t *blk;
	struct dir_lock_t *dir_lock;
	struct stream_buffer_t *sb = NULL;

	/* A transient tag is considered a hit if the block is
	 * locked in the corresponding directory */
	int tag = addr & ~cache->block_mask;

	unsigned int stream_tag = addr & pref->stream_tag_mask;
	int stream = -1;
	int slot = -1;
	int num_streams = pref->max_num_streams;

	for(stream = 0; stream < num_streams; stream++)
	{
		int i, count;
		sb = &pref->streams[stream];

		/* Block can't be in this stream */
		/*if(!sb->stream_tag == stream_tag)
			continue;*/

		count = sb->head + pref->max_num_slots;
		for(i = sb->head; i < count; i++)
		{
			slot = i % pref->max_num_slots;
			blk = cache_get_pref_block(cache, stream, slot);

			/* Increment any invalid unlocked head */
			if(slot == sb->head && !blk->state)
			{
				dir_lock = dir_pref_lock_get(mod->dir, stream, slot);
				if(!dir_lock->lock)
				{
					sb->head = (sb->head + 1) % pref->max_num_slots;
					continue;
				}
			}

			/* Tag hit */
			if (blk->tag == tag && blk->state)
				goto hit;

			/* Locked block and transient tag hit */
			if (blk->transient_tag == tag)
			{
				dir_lock = dir_pref_lock_get(mod->dir, stream, slot);
				if (dir_lock->lock)
					goto hit;
			}
		}
	}

	/* Miss */
	if (stream == num_streams)
	{
		PTR_ASSIGN(pref_stream_ptr, -1);
		PTR_ASSIGN(pref_slot_ptr, -1);
		return 0;
	}

hit:
	assert(sb->stream_tag == stream_tag || sb->stream_transcient_tag == stream_tag); /* Assegurem-nos de que el bloc estava on tocava */
	PTR_ASSIGN(pref_stream_ptr, stream);
	PTR_ASSIGN(pref_slot_ptr, slot);
	if(sb->head == slot)
		return 1; //Hit in head
	else
		return 2; //Hit in the middle of the stream
}


void mod_set_prefetched_bit(struct mod_t *mod, unsigned int addr, int val)
{
	int set, way;

	assert(mod->kind == mod_kind_cache && mod->cache != NULL);
	if (mod->cache->prefetcher && mod_find_block(mod, addr, &set, &way, NULL, NULL))
	{
		mod->cache->sets[set].blocks[way].prefetched = val;
	}
}


int mod_get_prefetched_bit(struct mod_t *mod, unsigned int addr)
{
	int set, way;

	assert(mod->kind == mod_kind_cache && mod->cache != NULL);
	if (mod->cache->prefetcher && mod_find_block(mod, addr, &set, &way, NULL, NULL))
	{
		return mod->cache->sets[set].blocks[way].prefetched;
	}

	return 0;
}


/* Lock a port, and schedule event when done.
 * If there is no free port, the access is enqueued in the port
 * waiting list, and it will retry once a port becomes available with a
 * call to 'mod_unlock_port'. */
void mod_lock_port(struct mod_t *mod, struct mod_stack_t *stack, int event)
{
	struct mod_port_t *port = NULL;
	int i;

	/* No free port */
	if (mod->num_locked_ports >= mod->num_ports)
	{
		assert(!DOUBLE_LINKED_LIST_MEMBER(mod, port_waiting, stack));

		/* If the request to lock the port is down-up, give it priority since
		 * it is possibly holding up a large portion of the memory hierarchy */
		if (stack->request_dir == mod_request_down_up)
		{
			DOUBLE_LINKED_LIST_INSERT_HEAD(mod, port_waiting, stack);
		}
		else
		{
			DOUBLE_LINKED_LIST_INSERT_TAIL(mod, port_waiting, stack);
		}
		stack->port_waiting_list_event = event;
		return;
	}

	/* Get free port */
	for (i = 0; i < mod->num_ports; i++)
	{
		port = &mod->ports[i];
		if (!port->stack)
			break;
	}

	/* Lock port */
	assert(port && i < mod->num_ports);
	port->stack = stack;
	stack->port = port;
	mod->num_locked_ports++;

	/* Debug */
	mem_debug("  %lld stack %lld %s port %d locked\n", esim_time, stack->id, mod->name, i);

	/* Schedule event */
	esim_schedule_event(event, stack, 0);
}


void mod_unlock_port(struct mod_t *mod, struct mod_port_t *port,
	struct mod_stack_t *stack)
{
	int event;
	/* Checks */
	assert(mod->num_locked_ports > 0);
	assert(stack->port == port && port->stack == stack);
	assert(stack->mod == mod);

	/* Unlock port */
	stack->port = NULL;
	port->stack = NULL;
	mod->num_locked_ports--;

	/* Debug */
	mem_debug("  %lld %lld %s port unlocked\n", esim_time,
		stack->id, mod->name);

	/* Check if there was any access waiting for free port */
	if (!mod->port_waiting_list_count)
		return;


	/* Wake up one access waiting for a free port */
	stack = mod->port_waiting_list_head;
	event = stack->port_waiting_list_event;
	assert(DOUBLE_LINKED_LIST_MEMBER(mod, port_waiting, stack));
	DOUBLE_LINKED_LIST_REMOVE(mod, port_waiting, stack);
	mod_lock_port(mod, stack, event);
}


void mod_access_start(struct mod_t *mod, struct mod_stack_t *stack,
	enum mod_access_kind_t access_kind)
{
	int index;

	/* Record access kind */
	stack->access_kind = access_kind;

	/* Insert in access list */
	DOUBLE_LINKED_LIST_INSERT_TAIL(mod, access, stack);

	/* Insert in write access list */
	if (access_kind == mod_access_store)
		DOUBLE_LINKED_LIST_INSERT_TAIL(mod, write_access, stack);

	/* Insert in access hash table */
	index = (stack->addr >> mod->log_block_size) % MOD_ACCESS_HASH_TABLE_SIZE;
	DOUBLE_LINKED_LIST_INSERT_TAIL(&mod->access_hash_table[index], bucket, stack);
}


void mod_access_finish(struct mod_t *mod, struct mod_stack_t *stack)
{
	int index;

	if (stack->background)
		return;

	/* Remove from access list */
	DOUBLE_LINKED_LIST_REMOVE(mod, access, stack);

	/* Remove from write access list */
	assert(stack->access_kind);
	if (stack->access_kind == mod_access_store)
		DOUBLE_LINKED_LIST_REMOVE(mod, write_access, stack);

	/* Remove from hash table */
	index = (stack->addr >> mod->log_block_size) % MOD_ACCESS_HASH_TABLE_SIZE;
	DOUBLE_LINKED_LIST_REMOVE(&mod->access_hash_table[index], bucket, stack);

	/* If this was a coalesced access, update counter */
	if (stack->coalesced)
	{
		assert(mod->access_list_coalesced_count > 0);
		mod->access_list_coalesced_count--;
	}
}


/* Return true if the access with identifier 'id' is in flight.
 * The address of the access is passed as well because this lookup is done on the
 * access truth table, indexed by the access address.
 */
int mod_in_flight_access(struct mod_t *mod, long long id, unsigned int addr)
{
	struct mod_stack_t *stack;
	int index;

	/* Look for access */
	index = (addr >> mod->log_block_size) % MOD_ACCESS_HASH_TABLE_SIZE;
	for (stack = mod->access_hash_table[index].bucket_list_head; stack; stack = stack->bucket_list_next)
		if (stack->id == id)
			return 1;

	/* Not found */
	return 0;
}


/* Return the youngest in-flight access older than 'older_than_stack' to block containing 'addr'.
 * If 'older_than_stack' is NULL, return the youngest in-flight access containing 'addr'.
 * The function returns NULL if there is no in-flight access to block containing 'addr'.
 */
struct mod_stack_t *mod_in_flight_address(struct mod_t *mod, unsigned int addr,
	struct mod_stack_t *older_than_stack)
{
	struct mod_stack_t *stack;
	int index;

	/* Look for address */
	index = (addr >> mod->log_block_size) % MOD_ACCESS_HASH_TABLE_SIZE;
	for (stack = mod->access_hash_table[index].bucket_list_head; stack;
		stack = stack->bucket_list_next)
	{
		/* This stack is not older than 'older_than_stack' */
		if (older_than_stack && stack->id >= older_than_stack->id)
			continue;

		/* Address matches */
		if (stack->addr >> mod->log_block_size == addr >> mod->log_block_size)
			return stack;
	}

	/* Not found */
	return NULL;
}


/* Return the youngest in-flight write older than 'older_than_stack'. If 'older_than_stack'
 * is NULL, return the youngest in-flight write. Return NULL if there is no in-flight write.
 */
struct mod_stack_t *mod_in_flight_write(struct mod_t *mod,
	struct mod_stack_t *older_than_stack)
{
	struct mod_stack_t *stack;

	/* No 'older_than_stack' given, return youngest write */
	if (!older_than_stack)
		return mod->write_access_list_tail;

	/* Search */
	for (stack = older_than_stack->access_list_prev; stack;
		stack = stack->access_list_prev)
		if (stack->access_kind == mod_access_store)
			return stack;

	/* Not found */
	return NULL;
}


int mod_serves_address(struct mod_t *mod, unsigned int addr)
{
	/* Address bounds */
	if (mod->range_kind == mod_range_bounds)
		return addr >= mod->range.bounds.low &&
			addr <= mod->range.bounds.high;

	/* Interleaved addresses */
	if (mod->range_kind == mod_range_interleaved)
		return (addr / mod->range.interleaved.div) %
			mod->range.interleaved.mod ==
			mod->range.interleaved.eq;

	/* Invalid */
	panic("%s: invalid range kind", __FUNCTION__);
	return 0;
}


/* Return the low module serving a given address. */
struct mod_t *mod_get_low_mod(struct mod_t *mod, unsigned int addr)
{
	struct mod_t *low_mod;
	struct mod_t *server_mod;

	/* Main memory does not have a low module */
	assert(mod_serves_address(mod, addr));
	if (mod->kind == mod_kind_main_memory)
	{
		assert(!linked_list_count(mod->low_mod_list));
		return NULL;
	}

	/* Check which low module serves address */
	server_mod = NULL;
	LINKED_LIST_FOR_EACH(mod->low_mod_list)
	{
		/* Get new low module */
		low_mod = linked_list_get(mod->low_mod_list);
		if (!mod_serves_address(low_mod, addr))
			continue;

		/* Address served by more than one module */
		if (server_mod)
			fatal("%s: low modules %s and %s both serve address 0x%x",
				mod->name, server_mod->name, low_mod->name, addr);

		/* Assign server */
		server_mod = low_mod;
	}

	/* Error if no low module serves address */
	if (!server_mod)
		fatal("module %s: no lower module serves address 0x%x",
			mod->name, addr);

	/* Return server module */
	return server_mod;
}


int mod_get_retry_latency(struct mod_t *mod)
{
	return random() % mod->latency + mod->latency;
}


/* Check if an access to a module can be coalesced with another access older
 * than 'older_than_stack'. If 'older_than_stack' is NULL, check if it can
 * be coalesced with any in-flight access.
 * If it can, return the access that it would be coalesced with. Otherwise,
 * return NULL. */
struct mod_stack_t *mod_can_coalesce(struct mod_t *mod,enum mod_access_kind_t access_kind, unsigned int addr,
	struct mod_stack_t *older_than_stack)
{
	struct mod_stack_t *stack;
	struct mod_stack_t *tail;

	/* For efficiency, first check in the hash table of accesses
	 * whether there is an access in flight to the same block. */
	assert(access_kind);
	/* En el cas dels prefetch a L2+ no podem usar la id per saber
	 * si ha arribat o no abans al mòdul, per tant no podem usar
	 * la funció mod_in_flight_address. Es pot millorar. */
	if ((!mod->cache->prefetcher || mod->level == 1) && !mod_in_flight_address(mod, addr, older_than_stack))
		return NULL;

	/* Get youngest access older than 'older_than_stack' */
	tail = older_than_stack ? older_than_stack->access_list_prev :
		mod->access_list_tail;

	/* Coalesce depending on access kind */
	switch (access_kind)
	{

	case mod_access_load:
	{
		for (stack = tail; stack; stack = stack->access_list_prev)
		{
			/* Only coalesce with groups of loads or prefetches at the tail */
			if (stack->access_kind != mod_access_load &&
			    stack->access_kind != mod_access_prefetch)
				return NULL;

			if (stack->addr >> mod->log_block_size ==
				addr >> mod->log_block_size)
				return stack->master_stack ? stack->master_stack : stack;
		}
		break;
	}

	case mod_access_store:
	{
		/* Only coalesce with last access */
		stack = tail;
		if (!stack)
			return NULL;

		/* Only if it is a write */
		if (stack->access_kind != mod_access_store)
			return NULL;

		/* Only if it is an access to the same block */
		if (stack->addr >> mod->log_block_size != addr >> mod->log_block_size)
			return NULL;

		/* Only if previous write has not started yet */
		if (stack->port_locked)
			return NULL;

		/* Coalesce */
		return stack->master_stack ? stack->master_stack : stack;
	}

	case mod_access_nc_store:
	{
		/* Only coalesce with last access */
		stack = tail;
		if (!stack)
			return NULL;

		/* Only if it is a non-coherent write */
		if (stack->access_kind != mod_access_nc_store)
			return NULL;

		/* Only if it is an access to the same block */
		if (stack->addr >> mod->log_block_size != addr >> mod->log_block_size)
			return NULL;

		/* Only if previous write has not started yet */
		if (stack->port_locked)
			return NULL;

		/* Coalesce */
		return stack->master_stack ? stack->master_stack : stack;
	}

	case mod_access_prefetch:
	{
		for (stack = tail; stack; stack = stack->access_list_prev)
		{
			if (stack->addr >> mod->log_block_size ==
				addr >> mod->log_block_size)
				return stack;
		}
		break;
	}

	default:
		panic("%s: invalid access type", __FUNCTION__);
		break;
	}

	/* No access found */
	return NULL;
}


void mod_coalesce(struct mod_t *mod, struct mod_stack_t *master_stack,
	struct mod_stack_t *stack)
{
	/* Debug */
	mem_debug("  %lld %lld 0x%x %s coalesce with %lld\n", esim_time,
		stack->id, stack->addr, mod->name, master_stack->id);

	/* Master stack must not have a parent. We only want one level of
	 * coalesced accesses. */
	assert(!master_stack->master_stack);

	/* Access must have been recorded already, which sets the access
	 * kind to a valid value. */
	assert(stack->access_kind);

	/* Set slave stack as a coalesced access */
	stack->coalesced = 1;

	/* If master stack is a prefetch only this access will coalesce with it.
	 * Next accesses will coalesce with this access. */
	if(master_stack->access_kind != mod_access_prefetch) //VVV
		stack->master_stack = master_stack;

	assert(mod->access_list_coalesced_count <= mod->access_list_count);

	/* Record in-flight coalesced access in module */
	mod->access_list_coalesced_count++;
}


struct mod_client_info_t *mod_client_info_create(struct mod_t *mod)
{
	struct mod_client_info_t *client_info;

	/* Create object */
	client_info = repos_create_object(mod->client_info_repos);

	client_info->core = -1;
	client_info->thread = -1;

	client_info->stream = -1;
	client_info->slot = -1;

	client_info->prefetcher_eip = -1;

	/* Return */
	return client_info;
}


struct mod_client_info_t *mod_client_info_clone(struct mod_t *mod, struct mod_client_info_t *original)
{
	struct mod_client_info_t *client_info;

	/* Create object */
	client_info = repos_create_object(mod->client_info_repos);

	*client_info = *original;

	/* Return */
	return client_info;
}


void mod_client_info_free(struct mod_t *mod, struct mod_client_info_t *client_info)
{
	repos_free_object(mod->client_info_repos, client_info);
}


void mod_interval_report_init(struct mod_t *mod)
{
	struct mod_report_stack_t *stack;
	char interval_report_file_name[MAX_PATH_SIZE];
	int thread;
	int core;
	int ret;
	



	/* Create new stack */
	stack = xcalloc(1, sizeof(struct mod_report_stack_t));
	stack->hits_per_thread_int        = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->stream_hits_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->misses_per_thread_int      = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->retries_per_thread_int     = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->evictions_per_thread_int   = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->atd_hits_per_thread_int    = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->atd_misses_per_thread_int  = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->atd_unknown_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->atd_intramisses_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->atd_intermisses_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));

	/* To measure pollution */

	/* General hash table */
	stack->pref_pollution_filter = hash_table_gen_create(256);
	/* A hash table per thread */
	stack->pref_pollution_filter_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(struct hash_table_gen_t *));
	stack->dem_pollution_filter_per_thread = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(struct hash_table_gen_t *));
	/* A counter per thread */
	stack->dem_pollution_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));
	stack->pref_pollution_per_thread_int = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(long long));

	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int pos = core * x86_cpu_num_threads + thread;
		if (mod->reachable_threads[pos])
		{
			stack->dem_pollution_filter_per_thread[pos] = hash_table_gen_create(256);
			stack->pref_pollution_filter_per_thread[pos] = hash_table_gen_create(256);
		}
	}

	/* Interval reporting of stats */
	ret = snprintf(interval_report_file_name, MAX_PATH_SIZE, "%s/%s.intrep.csv", mod_interval_reports_dir, mod->name);
	if (ret < 0 || ret >= MAX_PATH_SIZE)
		fatal("warning: function %s: string too long %s", __FUNCTION__, interval_report_file_name);

	stack->report_file = file_open_for_write(interval_report_file_name);
	if (!stack->report_file)
		fatal("%s: cannot open interval report file", interval_report_file_name);

	stack->mod = mod;

	mod->report_stack = stack;

	fprintf(stack->report_file, "%s", "esim-time");                                   /* Global simulation time */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-int");                     /* Prefetches executed in the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-useful-int");              /* Prefetches executed in the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-acc-int");                 /* Prefetch acuracy for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-late-int");                /* Late prefetches in the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "hits-int");                     /* Cache hits for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "stream-hits-int");              /* Hits in stream buffer for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "misses-int");                   /* Cache misses for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "retries-int");                  /* Retries for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-cov-int");                 /* Prefetch coverage for the interval */
	fprintf(stack->report_file, ",%s-%s", mod->name, "delayed-hits-int");             /* Hits on a block being brought by a prefetch */
	fprintf(stack->report_file, ",%s-%s", mod->name, "delayed-hit-avg-delay-int");    /* Average cycles waiting for a block that is being brought by a prefetch */
	fprintf(stack->report_file, ",%s-%s", mod->name, "pref-pollution-int");           /* Ratio between prefetch-caused misses and total misses in the interval */
	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH                                             /* Thread - thread pollution */
	{
		int pos = core * x86_cpu_num_threads + thread;
		if (mod->reachable_threads[pos])
		{
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "hits-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "stream-hits-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "misses-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "retries-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "evictions-int");
			/* Alternate Tag Directory */
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "atd-hits-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "atd-misses-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "atd-unknown-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "atd-intramisses-int");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "atd-intermisses-int");
			/* Demand pollution suffered by thread */
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "dem-pollution-int");
			/* Prefetch pollution suffered by thread */
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "pref-pollution-int");
			/* Cache partitioning */
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "allocated-ways-inst");
			fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "used-ways-inst");
		}
	}
	//Hugo printing new stats
	
	fprintf(stack->report_file, ",%s-c%dt%d-%s", mod->name, core, thread, "mru-hits");
	//for num vias
	if(mod->RTM )
	{
		for( int i = 0; i < mod->cache->assoc   ; i++ )
        	{
                	for(int w = 0; w < mod->cache->num_sets  ;w++ )
                	{
                        	fprintf(stack->report_file, ",%s-c%dt%d-%s-%d-%s-%d", mod->name, core, thread,"via",i, "ciclos-penalizacion", w);
                	}
       		}
	}
	

	//End
	fprintf(stack->report_file, "\n");
	fflush(stack->report_file);
}


void mod_interval_report(struct mod_t *mod)
{
	struct mod_report_stack_t *stack = mod->report_stack;

	int core;
	int thread;



	/* Prefetch accuracy */
	long long completed_prefetches_int = mod->completed_prefetches - stack->completed_prefetches;
	long long useful_prefetches_int = mod->useful_prefetches - stack->useful_prefetches;
	double prefetch_accuracy_int = completed_prefetches_int ? (double) useful_prefetches_int / completed_prefetches_int : NAN;
	prefetch_accuracy_int = prefetch_accuracy_int > 1 ? 1 : prefetch_accuracy_int; /* May be slightly greather than 1 due bad timing with cycles */

	/* Delayed hits */
	long long delayed_hits_int = mod->delayed_hits - stack->delayed_hits;
	long long delayed_hit_cycles_int = mod->delayed_hit_cycles - stack->delayed_hit_cycles;
	double delayed_hit_avg_lost_cycles_int = delayed_hits_int ? (double) delayed_hit_cycles_int / delayed_hits_int : NAN;

	/* Cache hits */
	long long hits_int = mod->hits - stack->hits;

	/* Cache stream buffer hits */
	long long stream_hits_int = mod->stream_hits - stack->stream_hits;

	/* Cache misses */
	long long misses_int = mod->misses - stack->misses;

	/* Cache retries */
	long long retries_int = mod->retries - stack->retries;

	/* Late prefetches */
	long long late_prefetches_int = mod->late_prefetches - stack->late_prefetches;

	/* Coverage */
	double coverage_int = (misses_int + useful_prefetches_int) ?
		(double) useful_prefetches_int / (misses_int + useful_prefetches_int) : NAN;
	
	/*Number of MRU hits by module, Hugo*/
        long long  MRU_hits = mod-> mru_hits;

	fprintf(stack->report_file, "%lld", esim_time);
	fprintf(stack->report_file, ",%lld", completed_prefetches_int);
	fprintf(stack->report_file, ",%lld", useful_prefetches_int);
	fprintf(stack->report_file, ",%.3f", prefetch_accuracy_int);
	fprintf(stack->report_file, ",%lld", late_prefetches_int);
	fprintf(stack->report_file, ",%lld", hits_int);
	fprintf(stack->report_file, ",%lld", stream_hits_int);
	fprintf(stack->report_file, ",%lld", misses_int);
	fprintf(stack->report_file, ",%lld", retries_int);
	fprintf(stack->report_file, ",%.3f", coverage_int);
	fprintf(stack->report_file, ",%lld", delayed_hits_int);
	fprintf(stack->report_file, ",%.3f", delayed_hit_avg_lost_cycles_int);
	fprintf(stack->report_file, ",%.3f", misses_int ? (double) stack->pref_pollution_int / misses_int : NAN);
	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH /* Thread - thread pollution */
	{
		int pos = core * x86_cpu_num_threads + thread;
		if (mod->reachable_threads[pos])
		{
			fprintf(stack->report_file, ",%lld", stack->hits_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->stream_hits_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->misses_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->retries_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->evictions_per_thread_int[pos]);
			/* ATD */
			fprintf(stack->report_file, ",%lld", stack->atd_hits_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->atd_misses_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->atd_unknown_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->atd_intramisses_per_thread_int[pos]);
			fprintf(stack->report_file, ",%lld", stack->atd_intermisses_per_thread_int[pos]);
			/* Percentage of misses for this thread that have been caused by evictions caused by DEMAND requests of other threads */
			fprintf(stack->report_file, ",%.3f", stack->misses_per_thread_int[pos] ? (double) stack->dem_pollution_per_thread_int[pos] / stack->misses_per_thread_int[pos] : NAN);
			/* Percentage of misses for this thread that have been caused by evictions caused by PREFETCH requests of other threads */
			fprintf(stack->report_file, ",%.3f", stack->misses_per_thread_int[pos] ? (double) stack->pref_pollution_per_thread_int[pos] / stack->misses_per_thread_int[pos] : NAN);
			/* Ways the partitioning cache algorithm has assigned to the thread. This is a snapshot value, not an average. */
			if (mod->cache->partitioning.policy)
				fprintf(stack->report_file, ",%d", mod->cache->assigned_ways[pos]);
			else
				fprintf(stack->report_file, ",%f", NAN);
			fprintf(stack->report_file, ",%.3f", (double) mod->cache->used_ways[pos] / mod->cache->num_sets);
		}
	}
	//Hugo adding MRU hits per module of cache and cycle penalties
	
	fprintf(stack->report_file, ",%lld", MRU_hits);
	if(mod->RTM)  
	{	
		for(int i = 0; i< mod->cache->assoc ;i++){
			for(int j = 0; j < mod->cache->num_sets;j++){
				fprintf(stack->report_file, ",%lld", (long long int) mod->RTM_data->penalizations[i][j]);
			}
		}
	}
		
		//for(int j = 0; j <= mod->cache->num_sets/mod->readers  ;j++){
                //                fprintf(stack->report_file, ",%lld", (long long int) mod->mod_last_used_set->added_cycles[i][j]);
                  //      }
	



        //End
	fprintf(stack->report_file, "\n");
	fflush(stack->report_file);

	/* Update stack */
	stack->delayed_hits = mod->delayed_hits;
	stack->delayed_hit_cycles = mod->delayed_hit_cycles;
	stack->useful_prefetches = mod->useful_prefetches;
	stack->completed_prefetches = mod->completed_prefetches;
	stack->hits = mod->hits;
	stack->stream_hits = mod->stream_hits;
	stack->misses = mod->misses;
	stack->retries = mod->retries;
	stack->late_prefetches = mod->late_prefetches;
	stack->pref_pollution_int = 0;

	hash_table_gen_clear(stack->pref_pollution_filter);
	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int pos = core * x86_cpu_num_threads + thread;
		if (mod->reachable_threads[pos])
		{
			stack->hits_per_thread_int[pos] = 0;
			stack->stream_hits_per_thread_int[pos] = 0;
			stack->misses_per_thread_int[pos] = 0;
			stack->retries_per_thread_int[pos] = 0;
			stack->evictions_per_thread_int[pos] = 0;
			stack->dem_pollution_per_thread_int[pos] = 0;
			stack->pref_pollution_per_thread_int[pos] = 0;
			hash_table_gen_clear(stack->dem_pollution_filter_per_thread[pos]);
			hash_table_gen_clear(stack->pref_pollution_filter_per_thread[pos]);
			stack->atd_hits_per_thread_int[pos] = 0;
			stack->atd_misses_per_thread_int[pos] = 0;
			stack->atd_unknown_per_thread_int[pos] = 0;
			stack->atd_intramisses_per_thread_int[pos] = 0;
			stack->atd_intermisses_per_thread_int[pos] = 0;
		}
	}
}


void mod_report_stack_reset_stats(struct mod_report_stack_t *stack)
{
	/* TODO */
}


void mod_adapt_pref_stack_reset_stats(struct mod_adapt_pref_stack_t *stack)
{
	/* TODO */
}


void mod_reset_stats(struct mod_t *mod)
{
	/* TODO */
}


/* Up-down recursive reset of module stats */
void mod_recursive_reset_stats(struct mod_t *mod)
{
	mod_reset_stats(mod);
	LINKED_LIST_FOR_EACH(mod->low_mod_list)
	{
		struct mod_t *low_mod = linked_list_get(mod->low_mod_list);
		mod_recursive_reset_stats(low_mod);
	}
}


void mod_adapt_pref_adp(struct mod_t *mod)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct mod_adapt_pref_stack_t *stack = mod->adapt_pref_stack;

	/* Main stats */
	double acc_int;
	double cov_int;
	double rob_stall_int;
	double bwno_int;
	double ipc_int;

	/* Auxiliar stats */
	long long cycles_int;

	long long prefs_int;
	long long useful_prefs_int;

	long long misses_int;

	long long dispatch_slots_lost;
	long long dispatch_slots_lost_int;

	long long uinsts;
	long long uinsts_int;

	long long bwno;

	/* Aggr. levels */
	int a1 = pref->th.adp.a1;
	int a2 = pref->th.adp.a2;
	int a3 = pref->th.adp.a3;

	/* Counters */
	int reachable_cores;

	int min_dram_freq = INT_MAX;

	assert(pref);
	assert(pref->adapt_policy == adapt_pref_policy_adp);
	assert(a1 < a2 && a2 < a3);

	/* Cycles */
	cycles_int = esim_cycle() - stack->last_cycle;

	/* Completed prefetches */
	prefs_int = mod->completed_prefetches - stack->last_completed_prefetches;

	/* Useful prefetches */
	useful_prefs_int = mod->useful_prefetches - stack->last_useful_prefetches;

	/* Cache misses */
	misses_int = mod->misses - stack->last_misses;

	/* Coverage */
	cov_int = (misses_int + useful_prefs_int) ? (double) useful_prefs_int / (misses_int + useful_prefs_int) : 0.0;

	/* Accuracy */
	acc_int = prefs_int ? (double) useful_prefs_int / prefs_int : 0.0;
	acc_int = acc_int > 1 ? 1 : acc_int; /* May be slightly greather than 1 due bad timing with cycles */

	/* Data involving all accessing cores */
	reachable_cores = 0;
	bwno = 0;
	dispatch_slots_lost = 0;
	uinsts = 0;
	for (int core = 0; core < x86_cpu_num_cores; core++)
	{
		/* Reachable cores */
		if (mod->reachable_threads[core * x86_cpu_num_threads])
		{
			dispatch_slots_lost += x86_cpu->core[core].dispatch_stall[x86_dispatch_stall_rob_mem];
			uinsts += X86_CORE.num_committed_uinst;
			reachable_cores++;
		}

		/* Non reachable cores */
		else
		{
			int i;
			LIST_FOR_EACH(mod->reachable_mm_modules, i)
			{
				struct mod_t *mm_mod = list_get(mod->reachable_mm_modules, i);
				min_dram_freq = MIN(min_dram_freq, esim_domain_frequency(mm_mod->dram_system->dram_domain_index));
				bwno += dram_system_get_bwn(mm_mod->dram_system->handler, mm_mod->mc_id, core);
			}
		}
	}
	dispatch_slots_lost /= reachable_cores;
	uinsts /= reachable_cores;
	bwno_int = bwno - stack->last_bwno;
	bwno_int /= (cycles_int * min_dram_freq / esim_frequency);

	/* Average IPC for the accessing cores */
	uinsts_int = uinsts - stack->last_uinsts;
	ipc_int = cycles_int ? (double) uinsts_int / cycles_int : 0.0;

	/* Average pct. of ROB stall cicles due memory instructions for the accessig cores */
	dispatch_slots_lost_int = dispatch_slots_lost - stack->last_dispatch_slots_lost;
	rob_stall_int = cycles_int > 0 ? (double) dispatch_slots_lost_int / (cycles_int * x86_cpu_dispatch_width): 0.0;

	/* Pref enabled */
	if (pref->enabled)
	{
		double bwno_th         = pref->th.adp.bwno;
		double cov_th          = pref->th.adp.cov;
		double acc_high_th     = pref->th.adp.acc_high;
		double acc_low_th      = pref->th.adp.acc_low;
		double acc_very_low_th = pref->th.adp.acc_very_low;

		/* Very low accuracy */
		if (acc_int < acc_very_low_th)
		{
			/* We can disable */
			if (a1 == 0)
			{
				/* Reduce aggr. and disable */
				pref->aggr = a2;
				pref->enabled = 0;
			}

			/* Disabling is forbidden */
			else
				pref->aggr = a1; /* Only reduce aggr */

			/* Good coverage and others don't require lots of BW */
			if (cov_int > cov_th && bwno_int < bwno_th)
				pref->enabled = 1;
		}

		/* Low accuracy */
		else if (acc_int < acc_low_th)
		{
			/* Reduce aggr. */
			pref->aggr = a2;

			/* Bad coverage and others need bw */
			if (cov_int < cov_th && bwno_int > bwno_th)
			{
				/* We can disable */
				if (a1 == 0)
					pref->enabled = 0;

				/* Cannot disable */
				else
					pref->aggr = a1; /* Further reduce aggr */
			}
		}

		/* Medium accuracy */
		else if(acc_int < acc_high_th)
		{
			/* Others need bandwidth */
			if (bwno_int > bwno_th)
				pref->aggr = a2; /* Reduce aggr. */
		}

		/* High accuracy */
		else
		{
			/* Others need bandwidth */
			if (bwno_int > bwno_th)
				pref->aggr = a2;

			/* Others don't need a lot of bw */
			else
			{
				/* Bad coverage */
				if (cov_int < cov_th)
					pref->aggr = a3;
			}
		}
	}

	/* Pref disabled */
	else
	{
		double bwno_th      = pref->th.adp.bwno;
		double misses_th    = pref->th.adp.misses;
		double ipc_th       = pref->th.adp.ipc;
		double rob_stall_th = pref->th.adp.rob_stall; /* Ratio of cycles with the ROB stalled due memory instructions */

		if (bwno_int < bwno_th &&
				(misses_int > stack->last_misses_int * misses_th ||
				rob_stall_int > rob_stall_th ||
				ipc_int < stack->last_ipc_int * ipc_th))
		{
			pref->enabled = 1;
		}
	}

	stack->last_cycle = esim_cycle();
	stack->last_ipc_int = ipc_int;
	stack->last_completed_prefetches = mod->completed_prefetches;
	stack->last_useful_prefetches = mod->useful_prefetches;
	stack->last_misses = mod->misses;
	stack->last_uinsts = uinsts;
	stack->last_dispatch_slots_lost = dispatch_slots_lost;
	stack->last_misses_int = misses_int;
	stack->last_bwno = bwno;
}


void mod_adapt_pref_fdp(struct mod_t *mod)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct mod_adapt_pref_stack_t *stack = mod->adapt_pref_stack;

	/* Main stats */
	double acc_int;
	double lateness_int;
	double pollution_int; /* A ratio, not an absolute number like in HPAC */

	/* Auxiliar stats */
	long long prefs_int;
	long long useful_prefs_int;
	long long misses_int;
	long long late_prefs_int;

	/* Thresholds */
	double acc_high_th  = pref->th.fdp.acc_high;
	double acc_low_th   = pref->th.fdp.acc_low;
	double lateness_th  = pref->th.fdp.lateness;
	double pollution_th = pref->th.fdp.pollution;

	/* Decision variables */
	bool acc_high;
	bool acc_medium;
	bool acc_low;
	bool late;
	bool polluting;

	/* Aggr. levels */
	int a1 = pref->th.fdp.a1;
	int a2 = pref->th.fdp.a2;
	int a3 = pref->th.fdp.a3;

	assert(pref);
	assert(pref->adapt_policy == adapt_pref_policy_fdp || pref->adapt_policy == adapt_pref_policy_hpac); /* HPAC uses FDP */
	assert(a1 < a2 && a2 < a3);

	/* Completed prefetches */
	prefs_int = mod->completed_prefetches - stack->last_completed_prefetches;

	/* Useful prefetches */
	useful_prefs_int = mod->useful_prefetches - stack->last_useful_prefetches;

	/* Cache misses */
	misses_int = mod->misses - stack->last_misses;

	/* Accuracy */
	acc_int = prefs_int ? (double) useful_prefs_int / prefs_int : 0.0;
	acc_int = acc_int > 1 ? 1 : acc_int; /* May be slightly greather than 1 due bad timing with cycles */

	/* Lateness */
	late_prefs_int = mod->late_prefetches - stack->last_late_prefetches;
	lateness_int = useful_prefs_int ? (double) late_prefs_int / useful_prefs_int : 0.0;

	/* Pollution */
	pollution_int = misses_int ? (double) stack->pref_pollution_int / misses_int : 0.0;

	/* Decision variables */
	acc_high = acc_int >= acc_high_th;
	acc_medium = acc_int < acc_high_th && acc_int >= acc_low_th;
	acc_low = acc_int < acc_low_th;
	late = lateness_int > lateness_th;
	polluting = pollution_int > pollution_th;

	/* Action taken {0 : none, 1 : increment, -1 : decrement} */
	stack->last_action = 0;

	/* Case 1 */
	if (acc_high && late && !polluting)
		goto increment;

	/* Case 2 */
	else if (acc_high && late && polluting)
		goto increment;

	/* Cases 3 7 11 */
	else if (!late && !polluting)
		goto done;

	/* Cases 4 8 12 */
	else if (!late && polluting)
		goto decrement;

	/* Case 5 */
	else if (acc_medium && late && !polluting)
		goto increment;

	/* Case 6 */
	else if (acc_medium && late && polluting)
		goto decrement;

	/* Case 9 */
	else if (acc_low && late && !polluting)
		goto decrement;

	/* Case 10 */
	else if (acc_low && late && polluting)
		goto decrement;

	/* Error */
	else
		fatal("%s: Error in adaptive algorithm", __FUNCTION__);

increment:
	if (pref->aggr == a1)
		pref->aggr = a2;
	else if(pref->aggr == a2)
		pref->aggr = a2;
	else if(pref->aggr == a3)
		goto done;
	else
		fatal("%s: Error in adaptive algorithm", __FUNCTION__);
	stack->last_action = 1;
	goto done;

decrement:
	if (pref->aggr == a1)
		goto done;
	else if(pref->aggr == a2)
		pref->aggr = a1;
	else if(pref->aggr == a3)
		pref->aggr = a2;
	else
		fatal("%s: Error in adaptive algorithm", __FUNCTION__);
	stack->last_action = -1;

done:
	stack->last_completed_prefetches = mod->completed_prefetches;
	stack->last_useful_prefetches = mod->useful_prefetches;
	stack->last_misses = mod->misses;
	stack->last_late_prefetches = mod->late_prefetches;
	stack->pref_pollution_int = 0;
	bloom_clear(stack->pref_pollution_filter);
}


void mod_adapt_pref_hpac(struct mod_t *mod)
{
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;
	struct mod_adapt_pref_stack_t *stack = mod->adapt_pref_stack;

	/* Main stats */
	double acc_int;
	double bwc_int;
	double bwno_int;
	long long  pollution_int; /* Absolute number, not a ratio like in FDP */

	/* Auxiliar stats */
	long long prefs_int;
	long long useful_prefs_int;
	long long bwno_glob;
	long long bwc_glob;
	int reachable_cores;

	/* Thresholds */
	double acc_th       = pref->th.hpac.acc;
	double bwc_th       = pref->th.hpac.bwc;
	double bwno_th      = pref->th.hpac.bwno;
	double pollution_th = pref->th.hpac.pollution;

	/* Decision variables */
	bool acc;
	bool bwc;
	bool polluting;
	bool bwno;

	/* Aggr. levels */
	int a1 = pref->th.hpac.a1;
	int a2 = pref->th.hpac.a2;
	int a3 = pref->th.hpac.a3;

	assert(pref);
	assert(pref->adapt_policy == adapt_pref_policy_hpac);
	assert(a1 < a2 && a2 < a3);

	/* Completed prefetches */
	prefs_int = mod->completed_prefetches - stack->last_completed_prefetches;

	/* Useful prefetches */
	useful_prefs_int = mod->useful_prefetches - stack->last_useful_prefetches;

	/* Accuracy */
	acc_int = prefs_int ? (double) useful_prefs_int / prefs_int : 0.0;
	acc_int = acc_int > 1 ? 1 : acc_int; /* May be slightly greather than 1 due bad timing with cycles */

	/* Pollution */
	pollution_int = stack->pref_pollution_int;

	/* BWC and BWNO */
	reachable_cores = 0;
	bwc_glob = 0;
	bwno_glob = 0;
	for (int core = 0; core < x86_cpu_num_cores; core++)
	{
		int i;
		/* Reachable cores */
		if (mod->reachable_threads[core * x86_cpu_num_threads])
		{
			LIST_FOR_EACH(mod->reachable_mm_modules, i)
			{
				struct mod_t *mm_mod = list_get(mod->reachable_mm_modules, i);
				bwc_glob += dram_system_get_bwc(mm_mod->dram_system->handler, mm_mod->mc_id, core);
				reachable_cores++;
			}
		}

		/* Non reachable cores */
		else
		{
			LIST_FOR_EACH(mod->reachable_mm_modules, i)
			{
				struct mod_t *mm_mod = list_get(mod->reachable_mm_modules, i);
				bwno_glob += dram_system_get_bwn(mm_mod->dram_system->handler, mm_mod->mc_id, core);
			}
		}
	}
	bwc_int = bwc_glob - stack->last_bwc;
	bwno_int = bwno_glob - stack->last_bwno;
	bwc_int /= (reachable_cores * list_count(mod->reachable_mm_modules));
	bwno_int /= list_count(mod->reachable_mm_modules);

	/* Decision variables */
	acc = acc_int >= acc_th;
	bwc = bwc_int > bwc_th;
	polluting = pollution_int > pollution_th;
	bwno = bwno_int > bwno_th;

	/* Apply FDP policy */
	mod_adapt_pref_fdp(mod);

	/* Correct FDP decisions */

	/* Cases 1 - 7 */
	if (!polluting)
	{
		/* Case 3a 3b */
		if (!acc && bwno)
			goto enf_down;
		/* Case 2 */
		else if(!acc && bwc && !bwno)
			goto allow_down;
		/* Cases 1 4 5 6 7 */
		else
			goto done;
	}

	/* Cases 8 - 14 */
	else
	{
		/* Cases 8 9 10a 10b */
		if (!acc)
			goto enf_down;

		/* Cases 11 12 13 14 */
		else
		{
			/* Case 11 */
			if (!bwc && !bwno)
				goto done;
			/* Case 14 */
			else if (bwc && bwno)
				goto enf_down;
			/* Cases 12 13 */
			else
				goto allow_down;
		}
	}

/* Action taken {0 : none, 1 : increment, -1 : decrement} */

allow_down:
	/* Undo any increment */
	if (stack->last_action == 1)
	{
		if(pref->aggr == a2)
			pref->aggr = a1;
		else if(pref->aggr == a3)
			pref->aggr = a2;
		else
			fatal("%s: Error in adaptive algorithm", __FUNCTION__);
		stack->last_action = 0;
	}
	goto done;

enf_down:
	/* Enforce throttle down */
	if (stack->last_action != -1)
	{
		if (pref->aggr == a1)
			goto done;
		else if(pref->aggr == a2)
			pref->aggr = a1;
		else if(pref->aggr == a3)
			pref->aggr = a2;
		else
			fatal("%s: Error in adaptive algorithm", __FUNCTION__);
		stack->last_action = -1;
	}

done:
	stack->last_completed_prefetches = mod->completed_prefetches;
	stack->last_useful_prefetches = mod->useful_prefetches;
	stack->pref_pollution_int = 0;
	bloom_clear(stack->pref_pollution_filter);
}


void mod_adapt_pref_mbp(struct mod_t *mod)
{
	struct prefetcher_t *pref = mod->cache->prefetcher;
	struct mod_adapt_pref_stack_t *stack = mod->adapt_pref_stack;

	double random = (double) rand() / RAND_MAX;
	double max_avg_reward;
	double acc_int;
	double prefs_int;
	double useful_prefs_int;

	/* Aggr. levels */
	int a1 = pref->th.mbp.a1;
	int a2 = pref->th.mbp.a2;
	int a3 = pref->th.mbp.a3;

	int choice = -1;

	assert(pref);
	assert(pref->adapt_policy == adapt_pref_policy_mbp);
	assert(a1 < a2 && a2 < a3);

	/* Completed prefetches */
	prefs_int = mod->completed_prefetches - stack->last_completed_prefetches;

	/* Useful prefetches */
	useful_prefs_int = mod->useful_prefetches - stack->last_useful_prefetches;

	/* Accuracy */
	acc_int = prefs_int ? (double) useful_prefs_int / prefs_int : 0.0;
	acc_int = acc_int > 1 ? 1 : acc_int * useful_prefs_int; /* May be slightly greather than 1 due bad timing with cycles */

	/* Add reward from last choice and count it */
	stack->reward[stack->last_choice] += acc_int;
	stack->times_used[stack->last_choice]++;

	/* Exploration */
	if (random < pref->th.mbp.ratio)
		choice = rand() % 3;

	/* Exploitation */
	else
	{
		max_avg_reward = 0;
		for (int i=0; i<3; i++)
		{
			double avg_reward = stack->times_used[i] ? stack->reward[i] / stack->times_used[i] : 0.0;
			if (avg_reward >= max_avg_reward)
			{
				max_avg_reward = avg_reward;
				choice = i;
			}
		}
		assert(choice != -1);
	}

	/* Make changes */
	switch(choice)
	{
		case 0:
			pref->aggr = a1;
			break;
		case 1:
			pref->aggr = a2;
			break;
		case 2:
			pref->aggr = a3;
			break;
		default:
			fatal("Invalid level choice");
			break;
	}

	printf("%s %d\n", mod->name, choice);

	stack->last_choice = choice;
	stack->last_completed_prefetches = mod->completed_prefetches;
	stack->last_useful_prefetches = mod->useful_prefetches;
}


void mod_adapt_pref_schedule(struct mod_t *mod)
{
	struct mod_adapt_pref_stack_t *stack;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;

	assert(cache->prefetcher->adapt_policy);

	/* Create new stack */
	stack = xcalloc(1, sizeof(struct mod_adapt_pref_stack_t));
	stack->mod = mod;
	stack->backoff = 50000;
	stack->last_choice = 2;
	mod->adapt_pref_stack = stack;

	if (prefetcher_uses_pollution_filters(pref))
	{
		stack->pref_pollution_filter = bloom_create(pref->bloom_bits, pref->bloom_capacity, pref->bloom_false_pos_prob);
		/* stack->thread_pollution_filter[i] = bloom_create(pref->bloom_bits, pref->bloom_capacity, pref->bloom_false_pos_prob); */
		/* stack->pref_thread_pollution_filter[i] = bloom_create(pref->bloom_bits, pref->bloom_capacity, pref->bloom_false_pos_prob); */
	}

	/* Schedule first event */
	assert(cache->prefetcher->adapt_interval);
	esim_schedule_event(EV_MOD_ADAPT_PREF, stack, cache->prefetcher->adapt_interval_kind == interval_kind_cycles ?
			cache->prefetcher->adapt_interval : stack->backoff);
}


/* This function will be called at fixed size intervals of cycles, instructions or
 * evictions to tune prefetch aggressivity. If the interval is in cycles, this function will be called only when nedded.
 * If the interval is in instructions or evictions, this function is called every N cycles (N heuristically computed) and only
 * does something if the requisites of intr/evictions are fulfilled. */
void mod_adapt_pref_handler(int event, void *data)
{
	struct mod_adapt_pref_stack_t *stack = data;
	struct mod_t *mod = stack->mod;
	struct cache_t *cache = mod->cache;
	struct prefetcher_t *pref = cache->prefetcher;

	long long cycles_int = esim_cycle() - stack->last_cycle;
	long long uinsts = 0;

	/* If simulation has ended, no more
	 * events to schedule. */
	if (esim_finish)
		return;

	/* Find out if an interval has finished */
	switch(cache->prefetcher->adapt_interval_kind)
	{
		case interval_kind_cycles:
			/* We can be sure the interval has finished */
			break;

		case interval_kind_instructions:
		{
			long long uinsts_int;
			double ipc_int;

			/* Number of uinsts executed in this interval by the threads accessing this module */
			for (int core = 0; core < x86_cpu_num_cores; core++)
				if (mod->reachable_threads[core * x86_cpu_num_threads])
					uinsts += X86_CORE.num_committed_uinst;
			uinsts_int = uinsts - stack->last_uinsts;

			/* Mean IPC for all the threads accessing this module */
			ipc_int = cycles_int ? (double) uinsts_int / cycles_int : 0.0;

			/* Try to predict when the next interval will begin */
			stack->backoff = ipc_int ? 0.75 * (pref->adapt_interval - uinsts_int) / ipc_int : 10000;
			stack->backoff = MAX(stack->backoff, 100);
			stack->backoff = MIN(stack->backoff, 75000);

			/* Interval has not finished yet */
			if (uinsts_int < cache->prefetcher->adapt_interval)
				goto schedule_next_event;
			break;
		}

		case interval_kind_evictions:
		{
			/* Evictions in this interval */
			long long evictions_int = mod->evictions - stack->last_evictions;
			double epc_int;

			/* Try to predict when the next interval will begin */
			epc_int = cycles_int ? (double) evictions_int / cycles_int : 0.0;
			stack->backoff =  epc_int ? 0.75 * (pref->adapt_interval - evictions_int) / epc_int : 10000;
			stack->backoff = MAX(stack->backoff, 100);
			stack->backoff = MIN(stack->backoff, 75000);

			/* Interval has not finished yet */
			if (evictions_int < pref->adapt_interval)
				goto schedule_next_event;
			break;
		}

		case interval_kind_invalid:
			fatal("%s: Invalid interval kind", __FUNCTION__);
			break;
	}

	switch(mod->cache->prefetcher->adapt_policy)
	{
		case adapt_pref_policy_adp:
			mod_adapt_pref_adp(mod);
			break;

		case adapt_pref_policy_fdp:
			mod_adapt_pref_fdp(mod);
			break;

		case adapt_pref_policy_hpac:
			mod_adapt_pref_hpac(mod);
			break;

		case adapt_pref_policy_mbp:
			mod_adapt_pref_mbp(mod);
			break;

		default:
			fatal("Invalid adaptative prefetch policy");
			break;
	}

	switch(cache->prefetcher->adapt_interval_kind)
	{
		case interval_kind_cycles:
			stack->last_cycle = esim_cycle();
			break;

		case interval_kind_instructions:
			stack->last_uinsts = uinsts;
			break;

		case interval_kind_evictions:
			stack->last_evictions = mod->evictions;
			break;

		case interval_kind_invalid:
			fatal("%s: Invalid interval kind", __FUNCTION__);
			break;
	}

schedule_next_event:
	assert(pref->adapt_interval);
	assert(pref->adapt_interval_kind);
	esim_schedule_event(event, stack,
			pref->adapt_interval_kind == interval_kind_cycles ?
			pref->adapt_interval : stack->backoff);
}
