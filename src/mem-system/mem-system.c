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

#include <arch/common/arch.h>
#include <arch/x86/timing/cpu.h>
#include <arch/x86/timing/uop.h>
#include <dramsim/bindings-c.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/file.h>
#include <lib/util/hash-table.h>
#include <lib/util/list.h>
#include <lib/util/linked-list.h>
#include <lib/util/string.h>
#include <network/network.h>

#include "cache.h"
#include "command.h"
#include "config.h"
#include "local-mem-protocol.h"
#include "mem-system.h"
#include "mod-stack.h"
#include "module.h"
#include "nmoesi-protocol.h"
#include "prefetcher.h"


/*
 * Global Variables
 */

extern unsigned long long int  hit_0;
extern unsigned long long int hit_1;
extern unsigned long long int hit_2;
extern unsigned long long int acum_cycles;
extern unsigned long long int acum_cycles_times;

int mem_debug_category;
int mem_trace_category;
int mem_peer_transfers;

int EV_MAIN_MEMORY_TIC;

unsigned long long int rob_mem_cont = 0;
unsigned long long int ciclos_tot = 0;

/* Frequency domain, as returned by function 'esim_new_domain'. */
int mem_frequency = 3000;
int mem_domain_index;

struct mem_system_t *mem_system;

char *mem_report_file_name = "";
char *main_mem_report_file_name = "";
char *own_report_file_name = "";
double stall_rob_aux;
unsigned long long int cycles_after_reset;

/*
 * Memory System Object
 */

struct mem_system_t *mem_system_create(void)
{
	struct mem_system_t *mem_system;

	/* Initialize */
	mem_system = xcalloc(1, sizeof(struct mem_system_t));
	mem_system->net_list = list_create();
	mem_system->mod_list = list_create();
	mem_system->mm_mod_list = list_create();
	mem_system->dram_systems = hash_table_create(0, 0); /* Dram systems, if any */

	/* Return */
	return mem_system;
}


void mem_system_free(struct mem_system_t *mem_system)
{
	char *key;
	struct dram_system_t *dram_system;

	/* Free memory modules */
	while (list_count(mem_system->mod_list))
		mod_free(list_pop(mem_system->mod_list));
	list_free(mem_system->mod_list);
	list_free(mem_system->mm_mod_list);

	/* Free networks */
	while (list_count(mem_system->net_list))
		net_free(list_pop(mem_system->net_list));
	list_free(mem_system->net_list);

	/* Free dram_systems */
	HASH_TABLE_FOR_EACH(mem_system->dram_systems, key, dram_system)
	{
		free(dram_system->name);
		assert(linked_list_count(dram_system->pending_reads) == 0);
		linked_list_free(dram_system->pending_reads);
		dram_system_free(dram_system->handler);
		free(dram_system);
	}
	hash_table_free(mem_system->dram_systems);

	/* Free memory system */
	free(mem_system);
}


/*
 * Public Functions
 */

static char *mem_err_timing =
	"\tA command-line option related with the memory hierarchy ('--mem' prefix)\n"
	"\thas been specified, by no architecture is running a detailed simulation.\n"
	"\tPlease specify at least one detailed simulation (e.g., with option\n"
	"\t'--x86-sim detailed'.\n";


void mem_system_init(void)
{
	int count;
	int i;

	/* If any file name was specific for a command-line option related with the
	 * memory hierarchy, make sure that at least one architecture is running
	 * timing simulation. */
	count = arch_get_sim_kind_detailed_count();
	if (mem_report_file_name && *mem_report_file_name && !count)
		fatal("memory report file given, but no timing simulation.\n%s",
				mem_err_timing);
	if (mem_config_file_name && *mem_config_file_name && !count)
		fatal("memory configuration file given, but no timing simulation.\n%s",
				mem_err_timing);

	/* Create trace category. This needs to be done before reading the
	 * memory configuration file with 'mem_config_read', since the latter
	 * function generates the trace headers. */
	mem_trace_category = trace_new_category();

	/* Create global memory system. This needs to be done before reading the
	 * memory configuration file with 'mem_config_read', since the latter
	 * function inserts caches and networks in 'mem_system', and relies on
	 * these lists to have been created. */
	mem_system = mem_system_create();

	/* Read memory configuration file */
	mem_config_read();

	/* Try to open report file */
	if (*mem_report_file_name && !file_can_open_for_write(mem_report_file_name))
		fatal("%s: cannot open GPU cache report file",
			mem_report_file_name);

	/* Create Frequency domain */
	mem_domain_index = esim_new_domain(mem_frequency);

	/* Event handler for memory hierarchy commands */
	EV_MEM_SYSTEM_COMMAND = esim_register_event_with_name(mem_system_command_handler,
			mem_domain_index, "mem_system_command");
	EV_MEM_SYSTEM_END_COMMAND = esim_register_event_with_name(mem_system_end_command_handler,
			mem_domain_index, "mem_system_end_command");

	/* NMOESI memory event-driven simulation */

	EV_MOD_NMOESI_LOAD = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load");
	EV_MOD_NMOESI_LOAD_LOCK = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load_lock");
	EV_MOD_NMOESI_LOAD_ACTION = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load_action");
	EV_MOD_NMOESI_LOAD_MISS = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load_miss");
	EV_MOD_NMOESI_LOAD_UNLOCK = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load_unlock");
	EV_MOD_NMOESI_LOAD_FINISH = esim_register_event_with_name(mod_handler_nmoesi_load,
			mem_domain_index, "mod_nmoesi_load_finish");

	EV_MOD_NMOESI_STORE = esim_register_event_with_name(mod_handler_nmoesi_store,
			mem_domain_index, "mod_nmoesi_store");
	EV_MOD_NMOESI_STORE_LOCK = esim_register_event_with_name(mod_handler_nmoesi_store,
			mem_domain_index, "mod_nmoesi_store_lock");
	EV_MOD_NMOESI_STORE_ACTION = esim_register_event_with_name(mod_handler_nmoesi_store,
			mem_domain_index, "mod_nmoesi_store_action");
	EV_MOD_NMOESI_STORE_UNLOCK = esim_register_event_with_name(mod_handler_nmoesi_store,
			mem_domain_index, "mod_nmoesi_store_unlock");
	EV_MOD_NMOESI_STORE_FINISH = esim_register_event_with_name(mod_handler_nmoesi_store,
			mem_domain_index, "mod_nmoesi_store_finish");

	EV_MOD_NMOESI_NC_STORE = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store");
	EV_MOD_NMOESI_NC_STORE_LOCK = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_lock");
	EV_MOD_NMOESI_NC_STORE_WRITEBACK = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_writeback");
	EV_MOD_NMOESI_NC_STORE_ACTION = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_action");
	EV_MOD_NMOESI_NC_STORE_MISS= esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_miss");
	EV_MOD_NMOESI_NC_STORE_UNLOCK = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_unlock");
	EV_MOD_NMOESI_NC_STORE_FINISH = esim_register_event_with_name(mod_handler_nmoesi_nc_store,
			mem_domain_index, "mod_nmoesi_nc_store_finish");

	EV_MOD_NMOESI_PREFETCH = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch");
	EV_MOD_NMOESI_PREFETCH_LOCK = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch_lock");
	EV_MOD_NMOESI_PREFETCH_ACTION = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch_action");
	EV_MOD_NMOESI_PREFETCH_MISS = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch_miss");
	EV_MOD_NMOESI_PREFETCH_UNLOCK = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch_unlock");
	EV_MOD_NMOESI_PREFETCH_FINISH = esim_register_event_with_name(mod_handler_nmoesi_prefetch,
			mem_domain_index, "mod_nmoesi_prefetch_finish");

	EV_MOD_NMOESI_FIND_AND_LOCK = esim_register_event_with_name(mod_handler_nmoesi_find_and_lock,
			mem_domain_index, "mod_nmoesi_find_and_lock");
	EV_MOD_NMOESI_FIND_AND_LOCK_PORT = esim_register_event_with_name(mod_handler_nmoesi_find_and_lock,
			mem_domain_index, "mod_nmoesi_find_and_lock_port");
	EV_MOD_NMOESI_FIND_AND_LOCK_PREF_STREAM = esim_register_event_with_name(mod_handler_nmoesi_find_and_lock,
			mem_domain_index, "mod_nmoesi_find_and_lock_pref_stream");
	EV_MOD_NMOESI_FIND_AND_LOCK_ACTION = esim_register_event_with_name(mod_handler_nmoesi_find_and_lock,
			mem_domain_index, "mod_nmoesi_find_and_lock_action");
	EV_MOD_NMOESI_FIND_AND_LOCK_FINISH = esim_register_event_with_name(mod_handler_nmoesi_find_and_lock,
			mem_domain_index, "mod_nmoesi_find_and_lock_finish");

	EV_MOD_NMOESI_EVICT = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict");
	EV_MOD_NMOESI_EVICT_INVALID = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_invalid");
	EV_MOD_NMOESI_EVICT_ACTION = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_action");
	EV_MOD_NMOESI_EVICT_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_receive");
	EV_MOD_NMOESI_EVICT_PROCESS = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_process");
	EV_MOD_NMOESI_EVICT_PROCESS_NONCOHERENT = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_process_noncoherent");
	EV_MOD_NMOESI_EVICT_REPLY = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_reply");
	EV_MOD_NMOESI_EVICT_REPLY = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_reply");
	EV_MOD_NMOESI_EVICT_REPLY_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_reply_receive");
	EV_MOD_NMOESI_EVICT_FINISH = esim_register_event_with_name(mod_handler_nmoesi_evict,
			mem_domain_index, "mod_nmoesi_evict_finish");

	EV_MOD_NMOESI_WRITE_REQUEST = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request");
	EV_MOD_NMOESI_WRITE_REQUEST_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_receive");
	EV_MOD_NMOESI_WRITE_REQUEST_LOCK = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_lock");
	EV_MOD_NMOESI_WRITE_REQUEST_ACTION = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_action");
	EV_MOD_NMOESI_WRITE_REQUEST_EXCLUSIVE = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_exclusive");
	EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_updown");
	EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_FINISH = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_updown_finish");
	EV_MOD_NMOESI_WRITE_REQUEST_UPDOWN_LATENCY = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_updown_latency");
	EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_downup");
	EV_MOD_NMOESI_WRITE_REQUEST_DOWNUP_FINISH = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_downup_finish");
	EV_MOD_NMOESI_WRITE_REQUEST_REPLY = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_reply");
	EV_MOD_NMOESI_WRITE_REQUEST_FINISH = esim_register_event_with_name(mod_handler_nmoesi_write_request,
			mem_domain_index, "mod_nmoesi_write_request_finish");

	EV_MOD_NMOESI_READ_REQUEST = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request");
	EV_MOD_NMOESI_READ_REQUEST_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_receive");
	EV_MOD_NMOESI_READ_REQUEST_LOCK = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_lock");
	EV_MOD_NMOESI_READ_REQUEST_ACTION = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_action");
	EV_MOD_NMOESI_READ_REQUEST_UPDOWN = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_updown");
	EV_MOD_NMOESI_READ_REQUEST_UPDOWN_MISS = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_updown_miss");
	EV_MOD_NMOESI_READ_REQUEST_UPDOWN_FINISH = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_updown_finish");
	EV_MOD_NMOESI_READ_REQUEST_UPDOWN_LATENCY = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_updown_latency");
	EV_MOD_NMOESI_READ_REQUEST_DOWNUP = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_downup");
	EV_MOD_NMOESI_READ_REQUEST_DOWNUP_WAIT_FOR_REQS = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_downup_wait_for_reqs");
	EV_MOD_NMOESI_READ_REQUEST_DOWNUP_FINISH = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_downup_finish");
	EV_MOD_NMOESI_READ_REQUEST_REPLY = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_reply");
	EV_MOD_NMOESI_READ_REQUEST_FINISH = esim_register_event_with_name(mod_handler_nmoesi_read_request,
			mem_domain_index, "mod_nmoesi_read_request_finish");

	EV_MOD_NMOESI_INVALIDATE = esim_register_event_with_name(mod_handler_nmoesi_invalidate,
			mem_domain_index, "mod_nmoesi_invalidate");
	EV_MOD_NMOESI_INVALIDATE_FINISH = esim_register_event_with_name(mod_handler_nmoesi_invalidate,
			mem_domain_index, "mod_nmoesi_invalidate_finish");

	EV_MOD_NMOESI_PEER_SEND = esim_register_event_with_name(mod_handler_nmoesi_peer,
			mem_domain_index, "mod_nmoesi_peer_send");
	EV_MOD_NMOESI_PEER_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_peer,
			mem_domain_index, "mod_nmoesi_peer_receive");
	EV_MOD_NMOESI_PEER_REPLY = esim_register_event_with_name(mod_handler_nmoesi_peer,
			mem_domain_index, "mod_nmoesi_peer_reply");
	EV_MOD_NMOESI_PEER_FINISH = esim_register_event_with_name(mod_handler_nmoesi_peer,
			mem_domain_index, "mod_nmoesi_peer_finish");

	EV_MOD_NMOESI_MESSAGE = esim_register_event_with_name(mod_handler_nmoesi_message,
			mem_domain_index, "mod_nmoesi_message");
	EV_MOD_NMOESI_MESSAGE_RECEIVE = esim_register_event_with_name(mod_handler_nmoesi_message,
			mem_domain_index, "mod_nmoesi_message_receive");
	EV_MOD_NMOESI_MESSAGE_ACTION = esim_register_event_with_name(mod_handler_nmoesi_message,
			mem_domain_index, "mod_nmoesi_message_action");
	EV_MOD_NMOESI_MESSAGE_REPLY = esim_register_event_with_name(mod_handler_nmoesi_message,
			mem_domain_index, "mod_nmoesi_message_reply");
	EV_MOD_NMOESI_MESSAGE_FINISH = esim_register_event_with_name(mod_handler_nmoesi_message,
			mem_domain_index, "mod_nmoesi_message_finish");

	/* Streams prefetch */
	EV_MOD_PREF = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams");
	EV_MOD_PREF_LOCK = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams_lock");
	EV_MOD_PREF_ACTION = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams_action");
	EV_MOD_PREF_MISS = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams_miss");
	EV_MOD_PREF_UNLOCK = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams_unlock");
	EV_MOD_PREF_FINISH = esim_register_event_with_name(mod_handler_pref,
			mem_domain_index, "mod_nmoesi_prefetch_streams_finish");

	EV_MOD_NMOESI_PREF_FIND_AND_LOCK = esim_register_event_with_name(mod_handler_nmoesi_pref_find_and_lock,
			mem_domain_index,"mod_nmoesi_pref_find_and_lock");
	EV_MOD_NMOESI_PREF_FIND_AND_LOCK_PORT = esim_register_event_with_name(mod_handler_nmoesi_pref_find_and_lock,
			mem_domain_index,"mod_nmoesi_pref_find_and_lock_port");
	EV_MOD_NMOESI_PREF_FIND_AND_LOCK_ACTION = esim_register_event_with_name(mod_handler_nmoesi_pref_find_and_lock,
			mem_domain_index,"mod_nmoesi_pref_find_and_lock_action");
	EV_MOD_NMOESI_PREF_FIND_AND_LOCK_FINISH = esim_register_event_with_name(mod_handler_nmoesi_pref_find_and_lock,
			mem_domain_index,"mod_nmoesi_pref_find_and_lock_finish");

	EV_MOD_NMOESI_INVALIDATE_SLOT = esim_register_event_with_name(mod_handler_nmoesi_invalidate_slot,
			mem_domain_index, "mod_nmoesi_invalidate_slot");
	EV_MOD_NMOESI_INVALIDATE_SLOT_LOCK = esim_register_event_with_name(mod_handler_nmoesi_invalidate_slot,
			mem_domain_index, "mod_nmoesi_invalidate_slot_lock");
	EV_MOD_NMOESI_INVALIDATE_SLOT_ACTION = esim_register_event_with_name(mod_handler_nmoesi_invalidate_slot,
			 mem_domain_index, "mod_nmoesi_invalidate_slot_action");
	EV_MOD_NMOESI_INVALIDATE_SLOT_UNLOCK = esim_register_event_with_name(mod_handler_nmoesi_invalidate_slot,
			mem_domain_index, "mod_nmoesi_invalidate_slot_unlock");
	EV_MOD_NMOESI_INVALIDATE_SLOT_FINISH = esim_register_event_with_name(mod_handler_nmoesi_invalidate_slot,
			mem_domain_index, "mod_nmoesi_invalidate_slot_finish");

	/* Event for adaptative prefetch */
	EV_MOD_ADAPT_PREF = esim_register_event_with_name(mod_adapt_pref_handler, mem_domain_index, "mod_adapt_pref");

	LIST_FOR_EACH(mem_system->mod_list, i)
	{
		struct mod_t *mod = list_get(mem_system->mod_list, i);

		if (mod->kind != mod_kind_cache)
			continue;

		/* Shedule adaptative prefetch */
		if (mod->cache->prefetcher && mod->cache->prefetcher->adapt_policy)
			mod_adapt_pref_schedule(mod);
	}


	EV_MOD_ATD_DELAYED_SET = esim_register_event_with_name(mod_handler_atd_delayed_set, mem_domain_index, "mod_atd_delayed_set");

	/* Local memory event driven simulation */

	EV_MOD_LOCAL_MEM_LOAD = esim_register_event_with_name(mod_handler_local_mem_load,
			mem_domain_index, "mod_local_mem_load");
	EV_MOD_LOCAL_MEM_LOAD_LOCK = esim_register_event_with_name(mod_handler_local_mem_load,
			mem_domain_index, "mod_local_mem_load_lock");
	EV_MOD_LOCAL_MEM_LOAD_FINISH = esim_register_event_with_name(mod_handler_local_mem_load,
			mem_domain_index, "mod_local_mem_load_finish");

	EV_MOD_LOCAL_MEM_STORE = esim_register_event_with_name(mod_handler_local_mem_store,
			mem_domain_index, "mod_local_mem_store");
	EV_MOD_LOCAL_MEM_STORE_LOCK = esim_register_event_with_name(mod_handler_local_mem_store,
			mem_domain_index, "mod_local_mem_store_lock");
	EV_MOD_LOCAL_MEM_STORE_FINISH = esim_register_event_with_name(mod_handler_local_mem_store,
			mem_domain_index, "mod_local_mem_store_finish");

	EV_MOD_LOCAL_MEM_FIND_AND_LOCK = esim_register_event_with_name(mod_handler_local_mem_find_and_lock,
			mem_domain_index, "mod_local_mem_find_and_lock");
	EV_MOD_LOCAL_MEM_FIND_AND_LOCK_PORT = esim_register_event_with_name(mod_handler_local_mem_find_and_lock,
			mem_domain_index, "mod_local_mem_find_and_lock_port");
	EV_MOD_LOCAL_MEM_FIND_AND_LOCK_ACTION = esim_register_event_with_name(mod_handler_local_mem_find_and_lock,
			mem_domain_index, "mod_local_mem_find_and_lock_action");
	EV_MOD_LOCAL_MEM_FIND_AND_LOCK_FINISH = esim_register_event_with_name(mod_handler_local_mem_find_and_lock,
			mem_domain_index, "mod_local_mem_find_and_lock_finish");
}


void mem_system_done(void)
{
	/* Dump report */
	mem_system_dump_report();

	/* Free memory system */
	mem_system_free(mem_system);
}


void mem_system_dump_report(void)
{
	struct net_t *net;
	struct mod_t *mod;
	struct cache_t *cache;

	FILE *f;
	FILE *fh;

	int i;
	unsigned long long int sum_pen = 0;
	unsigned long long int aux1 = 0;
	//double stall_rob_aux = 0; 

	/* Open file */
	f = file_open_for_write(mem_report_file_name);
	if (!f)
		return;

	/* Intro */
	fprintf(f, "; Report for caches, TLBs, and main memory\n");
	fprintf(f, ";    Accesses - Total number of accesses\n");
	fprintf(f, ";    Hits, Misses - Accesses resulting in hits/misses\n");
	fprintf(f, ";    HitRatio - Hits divided by accesses\n");
	fprintf(f, ";    Evictions - Invalidated or replaced cache blocks\n");
	fprintf(f, ";    Retries - For L1 caches, accesses that were retried\n");
	fprintf(f, ";    ReadRetries, WriteRetries, NCWriteRetries - Read/Write retried accesses\n");
	fprintf(f, ";    NoRetryAccesses - Number of accesses that were not retried\n");
	fprintf(f, ";    NoRetryHits, NoRetryMisses - Hits and misses for not retried accesses\n");
	fprintf(f, ";    NoRetryHitRatio - NoRetryHits divided by NoRetryAccesses\n");
	fprintf(f, ";    NoRetryReads, NoRetryWrites - Not retried reads and writes\n");
	fprintf(f, ";    Reads, Writes, NCWrites - Total read/write accesses\n");
	fprintf(f, ";    BlockingReads, BlockingWrites, BlockingNCWrites - Reads/writes coming from lower-level cache\n");
	fprintf(f, ";    NonBlockingReads, NonBlockingWrites, NonBlockingNCWrites - Coming from upper-level cache\n");
	fprintf(f, ";    Programmed Prefetches - Number of programmed prefetch accesses\n");
	fprintf(f, ";    Completed Prefetches - Number of completed prefetch accesses\n");
	fprintf(f, ";    Canceled Prefetches - Number of canceled prefetch accesses\n");
	fprintf(f, ";    Useful Prefetches - Number of useful prefetches\n");
	fprintf(f, ";    Delayed hits - Number of loads that access a block with is being fetched by a prefetch\n");
	fprintf(f, ";    Prefetch Accuracy - Useful prefetches / total completed prefetches\n");
	fprintf(f, ";    Prefetch Coverage - Useful prefetches / Faults if we dont use prefetch\n");
	fprintf(f, ";    MPKI - Misses / commited instructions\n");
	fprintf(f, "\n\n");

	/* Report for each cache */
	for (i = 0; i < list_count(mem_system->mod_list); i++)
	{
		mod = list_get(mem_system->mod_list, i);
		cache = mod->cache;
		fprintf(f, "[ %s ]\n", mod->name);
		fprintf(f, "\n");

		/* Configuration */
		if (cache) {
			fprintf(f, "Sets = %d\n", cache->num_sets);
			fprintf(f, "Assoc = %d\n", cache->assoc);
			fprintf(f, "Policy = %s\n", str_map_value(&cache_policy_map, cache->policy));
		}
		fprintf(f, "BlockSize = %d\n", mod->block_size);
		fprintf(f, "Latency = %d\n", mod->latency);
		fprintf(f, "Ports = %d\n", mod->num_ports);
		fprintf(f, "\n");

		/* Statistics */
		fprintf(f, "Accesses = %lld\n", mod->accesses);
		fprintf(f, "Hits = %lld\n", mod->hits);
		fprintf(f, "Misses = %lld\n", mod->accesses - mod->hits);
		fprintf(f, "HitRatio = %.4g\n", mod->accesses ?
			(double) mod->hits / mod->accesses : 0.0);
		fprintf(f, "Evictions = %lld\n", mod->evictions);
		fprintf(f, "Retries = %lld\n", mod->read_retries + mod->write_retries +
			mod->nc_write_retries);
		fprintf(f, "\n");
		fprintf(f, "Reads = %lld\n", mod->reads);
		fprintf(f, "ReadRetries = %lld\n", mod->read_retries);
		fprintf(f, "BlockingReads = %lld\n", mod->blocking_reads);
		fprintf(f, "NonBlockingReads = %lld\n", mod->non_blocking_reads);
		fprintf(f, "ReadHits = %lld\n", mod->read_hits);
		fprintf(f, "ReadMisses = %lld\n", mod->reads - mod->read_hits);
		fprintf(f, "\n");
		fprintf(f, "Writes = %lld\n", mod->writes);
		fprintf(f, "WriteRetries = %lld\n", mod->write_retries);
		fprintf(f, "BlockingWrites = %lld\n", mod->blocking_writes);
		fprintf(f, "NonBlockingWrites = %lld\n", mod->non_blocking_writes);
		fprintf(f, "WriteHits = %lld\n", mod->write_hits);
		fprintf(f, "WriteMisses = %lld\n", mod->writes - mod->write_hits);
		fprintf(f, "\n");
		fprintf(f, "NCWrites = %lld\n", mod->nc_writes);
		fprintf(f, "NCWriteRetries = %lld\n", mod->nc_write_retries);
		fprintf(f, "NCBlockingWrites = %lld\n", mod->blocking_nc_writes);
		fprintf(f, "NCNonBlockingWrites = %lld\n", mod->non_blocking_nc_writes);
		fprintf(f, "NCWriteHits = %lld\n", mod->nc_write_hits);
		fprintf(f, "NCWriteMisses = %lld\n", mod->nc_writes - mod->nc_write_hits);
		fprintf(f, "Prefetches = %lld\n", mod->prefetches);
		fprintf(f, "\n");
		fprintf(f, "NoRetryAccesses = %lld\n", mod->no_retry_accesses);
		fprintf(f, "NoRetryHits = %lld\n", mod->no_retry_hits);
		fprintf(f, "NoRetryMisses = %lld\n", mod->no_retry_accesses - mod->no_retry_hits);
		fprintf(f, "NoRetryHitRatio = %.4g\n", mod->no_retry_accesses ?
			(double) mod->no_retry_hits / mod->no_retry_accesses : 0.0);
		fprintf(f, "NoRetryReads = %lld\n", mod->no_retry_reads);
		fprintf(f, "NoRetryReadHits = %lld\n", mod->no_retry_read_hits);
		fprintf(f, "NoRetryReadMisses = %lld\n", (mod->no_retry_reads -
			mod->no_retry_read_hits));
		fprintf(f, "NoRetryWrites = %lld\n", mod->no_retry_writes);
		fprintf(f, "NoRetryWriteHits = %lld\n", mod->no_retry_write_hits);
		fprintf(f, "NoRetryWriteMisses = %lld\n", mod->no_retry_writes
			- mod->no_retry_write_hits);
		fprintf(f, "NoRetryNCWrites = %lld\n", mod->no_retry_nc_writes);
		fprintf(f, "NoRetryNCWriteHits = %lld\n", mod->no_retry_nc_write_hits);
		fprintf(f, "NoRetryNCWriteMisses = %lld\n", mod->no_retry_nc_writes
			- mod->no_retry_nc_write_hits);
		fprintf(f, "\n");
		fprintf(f, "\n");

		fprintf(f, "ProgrammedPrefetches = %lld\n", mod->programmed_prefetches);
		fprintf(f, "CompletedPrefetches = %lld\n", mod->completed_prefetches);
		fprintf(f, "CanceledPrefetches = %lld\n", mod->canceled_prefetches);
		fprintf(f, "CanceledPrefetchEndStream = %lld\n", mod->canceled_prefetches_end_stream);
		fprintf(f, "CanceledPrefetchCoalesce = %lld\n", mod->canceled_prefetches_coalesce);
		fprintf(f, "CanceledPrefetchCacheHit = %lld\n", mod->canceled_prefetches_cache_hit);
		fprintf(f, "CanceledPrefetchStreamHit = %lld\n", mod->canceled_prefetches_stream_hit);
		fprintf(f, "CanceledPrefetchRetry = %lld\n", mod->canceled_prefetches_retry);
		fprintf(f, "PrefetchRetries = %lld\n", mod->prefetch_retries);
		fprintf(f, "\n");

		fprintf(f, "UsefulPrefetches = %lld\n", mod->useful_prefetches);
		fprintf(f, "PrefetchAccuracy = %.4g\n", mod->completed_prefetches ? (double) mod->useful_prefetches / mod->completed_prefetches : 0.0);
		fprintf(f, "\n");

		fprintf(f, "SinglePrefetches = %lld\n", mod->single_prefetches);
		fprintf(f, "GroupPrefetches = %lld\n", mod->group_prefetches);
		fprintf(f, "CanceledPrefetchGroups = %lld\n", mod->canceled_prefetch_groups);

		fprintf(f, "\n");
		fprintf(f, "DelayedHits = %lld\n", mod->delayed_hits);
		fprintf(f, "DelayedHitsCyclesCounted = %lld\n", mod->delayed_hits_cycles_counted);
		fprintf(f, "DelayedHitAvgLostCycles = %.4g\n", mod->delayed_hits_cycles_counted? mod->delayed_hit_cycles / (double) mod->delayed_hits_cycles_counted : 0.0);
		fprintf(f, "\n");

		fprintf(f, "StreamHits = %lld\n", mod->stream_hits);

		fprintf(f, "PrefetchHits (rw)(up_down) = %lld\n", mod->up_down_hits);
		fprintf(f, "PrefetchHeadHits (rw)(up_down) = %lld\n", mod->up_down_head_hits);
		fprintf(f, "PrefetchHits(r)(down_up) = %lld\n", mod->down_up_read_hits);
		fprintf(f, "PrefetchHits(w)(down_up) = %lld\n", mod->down_up_write_hits);
		fprintf(f, "\n");
		fprintf(f, "FastResumedAccesses  = %lld\n", mod->fast_resumed_accesses);
		fprintf(f, "WriteBufferReadHits = %lld\n", mod->write_buffer_read_hits);
		fprintf(f, "WriteBufferWriteHits = %lld\n", mod->write_buffer_read_hits);
		fprintf(f, "\n");
		fprintf(f, "StreamEvictions = %lld\n", mod->stream_evictions);
		fprintf(f, "DownUpReadMisses = %lld\n", mod->down_up_read_misses);
		fprintf(f, "DownUpWriteMisses = %lld\n", mod->down_up_write_misses);
		fprintf(f, "BlocksAlreadyHere = %lld\n", mod->block_already_here);
		fprintf(f, "\n\n");
	}

	/* Dump report for networks */
	for (i = 0; i < list_count(mem_system->net_list); i++)
	{
		net = list_get(mem_system->net_list, i);
		net_dump_report(net, f);
	}


	//Dumping Hugo stats	
	if(strcmp(own_report_file_name,"")){
	fh = file_open_for_write(own_report_file_name);
	if (!fh){
		printf("Fallo en la creación del report own report!\n");
		return;
	}
	//Headers
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
		fprintf(fh, "Hits-%s,",mod->name);
	}
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                //fprintf(fh, "Hits-data-%s,",mod->name);
		fprintf(fh, "Hits-data-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
		//fprintf(fh, "Hits-instructions-%s,",mod->name);
                fprintf(fh, "Hits-instructions-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                //fprintf(fh, "Misses-%s,",mod->name);
		fprintf(fh, "Misses-%s,",mod->name);
	}
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                //fprintf(fh, "Misses-%s,",mod->name);
                fprintf(fh, "Misses-data-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                //fprintf(fh, "Misses-%s,",mod->name);
                fprintf(fh, "Misses-instructions-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
		//fprintf(fh, "Accesses-%s,",mod->name);
                fprintf(fh, "Accesses-%s,",mod->name);                                                                                                                                                                                           }
	for (i = 0; i < list_count(mem_system->mod_list); i++)                                                                                                                                                                               {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;                                                                                                                                                                                                                  fprintf(fh, "Invalids-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)                                                                                                                                                                               {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;                                                                                                                                                                                                                  fprintf(fh, "Up-down-%s,",mod->name);
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)                                                                                                                                                                               {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;                                                                                                                                                                                                                  fprintf(fh, "Down-up-%s,",mod->name);
        }
	//Mis hit-ratios
	
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
		if(mod->RTM ||mod->TapeCache)
		{
			break;
		}
		       
        }
	if(mod->RTM )
        {

                for( int i = 0; i < mod->cache->assoc   ; i++ )
                {
                        for(int w = 0; w < mod->cache->num_sets/mod->headers  ;w++ )
                        {
                                fprintf(fh, "%s-%s-%d-%s-%d,", mod->name,"via",i, "ciclos-penalizacion", w);
                        }
                }
		for( int i = 0; i < mod->cache->assoc   ; i++ )
                {
                        for(int w = 0; w < mod->cache->num_sets/mod->headers  ;w++ )
                        {
                                fprintf(fh, "%s-%s-%d-%s-%d,", mod->name,"via",i, "ciclos-penalizacion-hits", w);
                        }
                }
		for( int i = 0; i < mod->cache->assoc   ; i++ )
                {
                        for(int w = 0; w < mod->cache->num_sets/mod->headers  ;w++ )
                        {
                                fprintf(fh, "%s-%s-%d-%s-%d,", mod->name,"via",i, "ciclos-penalizacion-misses", w);
                        }
                }
               
                fprintf(fh, "%s","Cantidad de desplazamientos totales,");
		
        }
	//fprintf(fh,"Ciclos penalizacion ROB,");
	fprintf(fh, "Ciclos pen ROB propios,");
	//fprintf(fh, ",%.3f", dispatch_stall_int[i]);	
	fprintf(fh,"Ciclos de ejecucion,");
	if(mod->TapeCache == 0){	
		fprintf(fh, "Hits-RTM-0,");
        	fprintf(fh, "Hits-RTM-1,");
        	fprintf(fh, "Hits-RTM-2,");
	}
	if(mod->TapeCache){
		for( int i = 0; i < mod->cache->num_sets/mod->submodulos   ; i++ ){
			fprintf(fh, "Ciclos-pen-TC-%d,",i);
		}
		
	}
	fprintf(fh, "Cycles-L1D+L2,");
	fprintf(fh, "Apariciones-L1+L2\n");
	
	//DATA
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
		
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                //printf("Mod-Name: %s, mod_ac:%lld,mod_h:%lld,mod_m:%lld\n",mod->name,mod->accesses,mod->hits, mod->accesses - mod->hits);

        }		
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->hits_p);

        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)                                                                                                                                                                               {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,", mod->hits_data);

        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)                                                                                                                                                                               {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->hits_instructions);

        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->misses_p);                                                                                                                                                                                       
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->misses_data);                                                                                                                                                             
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,", mod->misses_instructions);                                                                                                                                                             
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->accesses_p);                                                                                                                                                                                       
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->accesses_invalid);                                                                                                                                                                                
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->accesses_up_down);                                                                                                                                                                                
        }
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {
                mod = list_get(mem_system->mod_list, i);
                cache = mod->cache;
                fprintf(fh, "%llu,",mod->accesses_down_up);                                                                                                                                                                                
        }
	
	for (i = 0; i < list_count(mem_system->mod_list); i++)
        {       mod = list_get(mem_system->mod_list, i);
                if(mod->RTM ||mod->TapeCache)
                {
                        break;
                }	
	}
	if(mod->RTM )
        {
                for(int i = 0; i< mod->cache->assoc ;i++){
                        for(int j = 0; j < mod->cache->num_sets/mod->headers;j++){
				for(int k = 0; k < mod->submodulos;k++){
					sum_pen += mod->RTM_data[k].penalizations[i][j];
                                	//fprintf(fh, "%lld,", (long long int) mod->RTM_data->penalizations[i][j]);
                                	//accesses_n += mod->RTM_data[k].penalizations[i][j];
					//misses_n += mod->RTM_data[k].pen_miss[i][j];
					//hits_n += mod->RTM_data[k].pen_hit[i][j];
				
				}
				fprintf(fh, "%llu,",  sum_pen);
				sum_pen = 0;
                        }
                
		}
		sum_pen = 0;
		//Printing hits
		for(int i = 0; i< mod->cache->assoc ;i++){
                        for(int j = 0; j < mod->cache->num_sets/mod->headers;j++){
                                for(int k = 0; k < mod->submodulos;k++){
                                        sum_pen += mod->RTM_data[k].pen_hit[i][j];
                                 
                                }
				fprintf(fh, "%llu,",  sum_pen);
				sum_pen = 0;
                        }
                }
		sum_pen = 0;
		//Printing misses
		for(int i = 0; i< mod->cache->assoc ;i++){
                        for(int j = 0; j < mod->cache->num_sets/mod->headers;j++){
                                for(int k = 0; k < mod->submodulos;k++){
                                        sum_pen += mod->RTM_data[k].pen_miss[i][j];
                
                                }                                                                                                                                                                                                                                      fprintf(fh, "%llu,",  sum_pen);
                                sum_pen = 0;
                        }
                }

              	for(int i = 0; i < mod->submodulos;i++){ 
                	aux1 += mod->RTM_data[i].total_shifts;
		}
		fprintf(fh, "%llu,", aux1);
        }

	
	//fprintf(fh, "%.3f,", stall_rob_aux);
	fprintf(fh,"%llu,",rob_mem_cont );
	fprintf(fh,"%llu,", ciclos_tot - cycles_after_reset);
	if(mod->RTM){
		fprintf(fh,"%llu,",hit_0 );
        	fprintf(fh,"%llu,",hit_1 );
        	fprintf(fh,"%llu,",hit_2 );
	}
	
	sum_pen = 0;
	if(mod->TapeCache){
		//2 es el cost del swap
		for(int i = 0; i< (mod->cache->num_sets/mod->submodulos) ;i++){
                        for(int j = 0; j < mod->cache->assoc;j++){
                                for(int k = 0; k < mod->submodulos;k++){
                                        sum_pen += mod->RTM_data[k].penalizations[j][i];
                        	}
			}
			fprintf(fh, "%llu,",  sum_pen);
                        sum_pen = 0;
		}
	}
	
	fprintf(fh,"%llu,",acum_cycles);
	fprintf(fh,"%llu\n",acum_cycles_times );	
	//printf("Dades Hugo-> Acc:%d,hit:%d,miss:%d\n",accesses_n,hits_n,misses_n); 
	
	/* Done */
	fclose(fh);
	}
	fclose(f);
}


struct mod_t *mem_system_get_mod(char *mod_name)
{
	struct mod_t *mod;

	int mod_id;

	/* Look for module */
	LIST_FOR_EACH(mem_system->mod_list, mod_id)
	{
		mod = list_get(mem_system->mod_list, mod_id);
		if (!strcasecmp(mod->name, mod_name))
			return mod;
	}

	/* Not found */
	return NULL;
}


struct net_t *mem_system_get_net(char *net_name)
{
	struct net_t *net;

	int net_id;

	/* Look for network */
	LIST_FOR_EACH(mem_system->net_list, net_id)
	{
		net = list_get(mem_system->net_list, net_id);
		if (!strcasecmp(net->name, net_name))
			return net;
	}

	/* Not found */
	return NULL;
}


void main_memory_power_callback(double a, double b, double c, double d)
{
}


void main_memory_read_callback(void *payload, unsigned int id, uint64_t address, uint64_t interthread_penalty)
{
	int found = 0;
	int cpu_freq; /* In MHz */
	int dram_freq; /* In MHz */
	struct x86_uop_t *uop;
	struct mod_stack_t *stack = NULL;
	struct dram_system_t *dram_system = (struct dram_system_t *) payload;

	/* You cannnot use LINKED_LIST_FOR_EACH if you plan to remove elements */
	linked_list_head(dram_system->pending_reads);
	while(!linked_list_is_end(dram_system->pending_reads))
	{
		stack = linked_list_get(dram_system->pending_reads);
		if (stack->addr == address)
		{
			mem_debug("  %lld %lld 0x%x %s dram access completed\n", esim_time, stack->id, stack->tag, stack->target_mod->dram_system->name);
			stack->main_memory_accessed = 1;
			esim_schedule_event(EV_MOD_NMOESI_READ_REQUEST_UPDOWN_LATENCY, stack, 0);
			linked_list_remove(dram_system->pending_reads);
			found++;
		}
		else
			linked_list_next(dram_system->pending_reads);
	}
	assert(found == 1);

	cpu_freq = arch_x86->frequency;
	dram_freq = esim_domain_frequency(dram_system->dram_domain_index);

	uop = stack->event_queue_item;
	if (uop)
		uop->uinst->dram_interthread_penalty_cycles += interthread_penalty * cpu_freq / dram_freq;
	assert(uop || stack->client_info->instr_fetch || stack->prefetch); /* If there isn't a uop, then it must be a instruction fetch access or a prefetch */
}


void main_memory_write_callback(void *payload, unsigned int id, uint64_t address, uint64_t clock_cycle)
{
}


/* Schedules an event to notify dramsim that a main memory cycle has passed */
void main_memory_tic_scheduler(struct dram_system_t *ds)
{
	int cpu_freq = arch_x86->frequency; /* In MHz */
	int dram_freq = dram_system_get_dram_freq(ds->handler) / 1000000; /* In MHz */

	assert(cpu_freq >= dram_freq);

	/* New domain and event for dramsim clock tics */
	ds->dram_domain_index = esim_new_domain(dram_freq);
	EV_MAIN_MEMORY_TIC = esim_register_event_with_name(main_memory_tic_handler, ds->dram_domain_index, "dram_system_tic");

	esim_schedule_event(EV_MAIN_MEMORY_TIC, ds, 1);
}


/* Notifies dramsim that a main memory cycle has passed */
void main_memory_tic_handler(int event, void *data)
{
	struct dram_system_t *ds = (struct dram_system_t*) data;

	/* If simulation has ended and dram system has no more petitions, no more
	 * events to schedule. */
	if (esim_finish && !linked_list_count(ds->pending_reads) && !esim_event_count())
		return;

	dram_system_dram_tick(ds->handler);
	esim_schedule_event(event, ds, 1);
}


void mem_system_interval_report_init(void)
{
	for (int i = 0; i < list_count(mem_system->mod_list); i++)
	{
		struct mod_t *mod = list_get(mem_system->mod_list, i);
		mod_interval_report_init(mod);
	}
}


void mem_system_interval_report(void)
{
	/* Report for each cache */
	for (int i = 0; i < list_count(mem_system->mod_list); i++)
	{
		struct mod_t *mod = list_get(mem_system->mod_list, i);
		mod_interval_report(mod);
	}
}
