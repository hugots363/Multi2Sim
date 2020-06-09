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

#ifndef MEM_SYSTEM_MODULE_H
#define MEM_SYSTEM_MODULE_H


#include <stdio.h>

#include "cache.h"
#include "stream-prefetcher.h"

extern int EV_MOD_ADAPT_PREF;

extern int max_mod_level;

/* Hugo's */

struct RTM_data_t
{
	int last_read_set;
	int *headers_pos;
	int **penalizations;
	int **pen_hit;
	int **pen_miss; 
	long long int total_shifts;	
}; 

extern enum RTM_type_t
{
        mod_RTM_type_invalid = 0,
        mod_RTM_type_SL,
        mod_RTM_type_SE,
        mod_RTM_type_DL
}RTM_type;
 
extern char *mod_RTM_type_map[];
//= {"Nulo","SL","SE","DL"};
/* */


struct mod_report_stack_t
{
	struct mod_t *mod;
	long long completed_prefetches;
	long long useful_prefetches;
	long long late_prefetches;
	long long hits;
	long long stream_hits;
	long long misses;
	long long retries;

	long long delayed_hits;
	long long delayed_hit_cycles;

	struct hash_table_gen_t *pref_pollution_filter; /* Blocks replaced by prefetches */
	struct hash_table_gen_t **dem_pollution_filter_per_thread; /* Blocks replaced by DEMAND requests, per thread */
	struct hash_table_gen_t **pref_pollution_filter_per_thread; /* Blocks replaced by PREFETCH requests, per thread */

	long long pref_pollution_int; /* Number prefetch-evicted blocks that are later requested */
	long long *dem_pollution_per_thread_int; /* Pollution SUFFERED per thread that is caused by DEMAND request from other threads */
	long long *pref_pollution_per_thread_int; /* Pollution SUFFERED per thread that is caused by PREFETCH request from other threads */

	long long *hits_per_thread_int;
	long long *misses_per_thread_int;
	long long *retries_per_thread_int;
	long long *evictions_per_thread_int;
	long long *stream_hits_per_thread_int;

	long long *atd_hits_per_thread_int;
	long long *atd_misses_per_thread_int;
	long long *atd_unknown_per_thread_int;
	long long *atd_intramisses_per_thread_int;
	long long *atd_intermisses_per_thread_int;

	FILE *report_file;
};


/* Port */
struct mod_port_t
{
	/* Port lock status */
	int locked;
	long long lock_when;  /* Cycle when it was locked */
	struct mod_stack_t *stack;  /* Access locking port */

	/* Waiting list */
	struct mod_stack_t *waiting_list_head;
	struct mod_stack_t *waiting_list_tail;
	int waiting_list_count;
	int waiting_list_max;
};

/* String map for access type */
extern struct str_map_t mod_access_kind_map;

/* Access type */
enum mod_access_kind_t
{
	mod_access_invalid = 0,
	mod_access_load,
	mod_access_store,
	mod_access_nc_store,
	mod_access_prefetch,
	mod_access_read_request,
	mod_access_write_request,
	mod_access_invalidate_slot
};

/* Module types */
enum mod_kind_t
{
	mod_kind_invalid = 0,
	mod_kind_cache,
	mod_kind_main_memory,
	mod_kind_local_memory
};

/* Any info that clients (cpu/gpu) can pass
 * to the memory system when mod_access()
 * is called. */
struct mod_client_info_t
{
	int core;
	int thread;
	struct x86_ctx_t *ctx;

	/* Fields used by stream prefetchers */
	int stream;
	int slot;
	enum stream_request_kind_t stream_request_kind;

	/* This field is for use by the prefetcher. It is set
	 * to the PC of the instruction accessing the module */
	unsigned int prefetcher_eip;

	unsigned int late_prefetch : 1; /* Flag that marks this access as a late prefetch */
	unsigned int instr_fetch : 1;   /* Flag, access requesting a block of intructions */
};

/* Type of address range */
enum mod_range_kind_t
{
	mod_range_invalid = 0,
	mod_range_bounds,
	mod_range_interleaved
};

struct mod_adapt_pref_stack_t
{
	struct mod_t *mod;
	struct bloom_t *pref_pollution_filter; /* Evictions of blocks caused by prefetch requests */

	long long last_cycle;
	long long last_uinsts;
	long long last_evictions;
	long long last_useful_prefetches;
	long long last_completed_prefetches;
	long long last_dispatch_slots_lost;
	long long last_misses;
	long long last_late_prefetches;
	long long last_bwno;
	long long last_bwc;

	long long last_misses_int;

	long long pref_pollution_int;

	long long backoff;

	double last_ipc_int;

	int last_action;
	double reward[3];
	long long times_used[3];
	int last_choice;
	long long *uinsts_per_core;
};



#define MOD_ACCESS_HASH_TABLE_SIZE  17

/* Memory module */
struct mod_t
{
	/* Parameters */
	enum mod_kind_t kind;
	char *name;
	int block_size;
	int log_block_size;
	int latency;
	int dir_latency;
	int mshr_size;

	/* Main memory module */
	struct reg_rank_t *regs_rank; // ranks which this channels connects with
	int num_regs_rank;
	int num_req_input_buffer;

	/* Mem controller associated to mm */
	struct mem_controller_t *mem_controller; /* DEPRECATED */

	/* Dramsim */
	int mc_id; /* If dramsim enabled, in main memory modules this field stores the id of the memory controller attached */
	struct dram_system_t *dram_system;

	/* Module level starting from entry points */
	int level;

	/* Address range served by module */
	enum mod_range_kind_t range_kind;
	union
	{
		/* For range_kind = mod_range_bounds */
		struct
		{
			unsigned int low;
			unsigned int high;
		} bounds;

		/* For range_kind = mod_range_interleaved */
		struct
		{
			unsigned int mod;
			unsigned int div;
			unsigned int eq;
		} interleaved;
	} range;

	/* Ports */
	struct mod_port_t *ports;
	int num_ports;
	int num_locked_ports;

	/* Accesses waiting to get a port */
	struct mod_stack_t *port_waiting_list_head;
	struct mod_stack_t *port_waiting_list_tail;
	int port_waiting_list_count;
	int port_waiting_list_max;

	/* Directory */
	struct dir_t *dir;
	int dir_size;
	int dir_assoc;
	int dir_num_sets;

	/* Waiting list of events */
	struct mod_stack_t *waiting_list_head;
	struct mod_stack_t *waiting_list_tail;
	int waiting_list_count;
	int waiting_list_max;

	/* Cache structure */
	struct cache_t *cache;

	/* Low and high memory modules */
	struct linked_list_t *high_mod_list;
	struct linked_list_t *low_mod_list;

	/* Smallest block size of high nodes. When there is no high node, the
	 * sub-block size is equal to the block size. */
	int sub_block_size;
	int num_sub_blocks;  /* block_size / sub_block_size */

	/* Interconnects */
	struct net_t *high_net;
	struct net_t *low_net;
	struct net_node_t *high_net_node;
	struct net_node_t *low_net_node;

	/* Access list */
	struct mod_stack_t *access_list_head;
	struct mod_stack_t *access_list_tail;
	int access_list_count;
	int access_list_max;

	/* Write access list */
	struct mod_stack_t *write_access_list_head;
	struct mod_stack_t *write_access_list_tail;
	int write_access_list_count;
	int write_access_list_max;

	/* Number of in-flight coalesced accesses. This is a number
	 * between 0 and 'access_list_count' at all times. */
	int access_list_coalesced_count;

	/* Clients (CPU/GPU) that use this module can fill in some
	 * optional information in the mod_client_info_t structure.
	 * Using a repos_t memory allocator for these structures. */
	struct repos_t *client_info_repos;

	/* Hash table of accesses */
	struct
	{
		struct mod_stack_t *bucket_list_head;
		struct mod_stack_t *bucket_list_tail;
		int bucket_list_count;
		int bucket_list_max;
	} access_hash_table[MOD_ACCESS_HASH_TABLE_SIZE];

	/* Architecture accessing this module. For versions of Multi2Sim where it is
	 * allowed to have multiple architectures sharing the same subset of the
	 * memory hierarchy, the field is used to check this restriction. */
	struct arch_t *arch;

	int num_reachable_threads;
	char *reachable_threads; /* Vector of booleans with cores x threads_per_core size. Indicates the threads that can reach this module. */
	struct list_t *reachable_mm_modules; /* List of main memory modules that can be reached from this module */

	/* Stack for activate/deactivate prefetch at intervals */
	struct mod_adapt_pref_stack_t *adapt_pref_stack;

	/* Reporting statistics at intervals */
	struct mod_report_stack_t *report_stack;

	/* Alternate Tag Directory per thread */
	struct atd_t **atd_per_thread;

	/* Statistics */
	/* Vicent's Seal of Approval */
	long long hits;
	long long misses;
	long long retries;
	long long late_prefetches;
	long long completed_prefetches;
	long long useful_prefetches;

	long long *atd_hits_per_thread;
	long long *atd_misses_per_thread;
	long long *atd_unknown_per_thread;
	long long *atd_intramisses_per_thread;
	long long *atd_intermisses_per_thread;
	
	/*Hugo MRU*/
	long long mru_hits;

	/* Stats not approved */
	long long accesses;

	long long reads;
	long long effective_reads;
	long long effective_read_hits;
	long long writes;
	long long effective_writes;
	long long effective_write_hits;
	long long nc_writes;
	long long effective_nc_writes;
	long long effective_nc_write_hits;
	long long prefetches;
	long long evictions;

	long long blocking_reads;
	long long non_blocking_reads;
	long long read_hits;
	long long blocking_writes;
	long long non_blocking_writes;
	long long write_hits;
	long long blocking_nc_writes;
	long long non_blocking_nc_writes;
	long long nc_write_hits;

	long long read_retries;
	long long write_retries;
	long long nc_write_retries;

	long long no_retry_accesses;
	long long no_retry_hits;
	long long no_retry_reads;
	long long no_retry_read_hits;
	long long no_retry_writes;
	long long no_retry_write_hits;
	long long no_retry_nc_writes;
	long long no_retry_nc_write_hits;
	long long no_retry_stream_hits;

	/* Prefetch */
	long long programmed_prefetches;
	long long canceled_prefetches;
	long long canceled_prefetches_end_stream;
	long long canceled_prefetches_coalesce;
	long long canceled_prefetches_cache_hit;
	long long canceled_prefetches_stream_hit;
	long long canceled_prefetches_retry;
	long long effective_useful_prefetches; /* Useful prefetches with less delay hit cicles than 1/3 of the delay of accesing MM */
	long long pollution;

	long long prefetch_retries;

	long long stream_hits;
	long long delayed_hits; /* Hit on a block being brougth by a prefetch */
	long long delayed_hit_cycles; /* Cicles lost due delayed hits */
	long long delayed_hits_cycles_counted; /* Number of delayed hits whose lost cycles has been counted */

	long long single_prefetches; /* Prefetches on hit */
	long long group_prefetches; /* Number of GROUPS */
	long long canceled_prefetch_groups;

	long long up_down_hits;
	long long up_down_head_hits;
	long long down_up_read_hits;
	long long down_up_write_hits;

	long long fast_resumed_accesses;
	long long write_buffer_read_hits;
	long long write_buffer_write_hits;
	long long write_buffer_prefetch_hits;

	long long stream_evictions;

	/* Silent replacement */
	long long down_up_read_misses;
	long long down_up_write_misses;
	long long block_already_here;

	/*Hugo RTM */
	int RTM;
	int mov_cabezal;
	int headers;
	int WU;
	int WU_f;
	int entrelazado;	
	struct RTM_data_t *RTM_data;
	enum RTM_type_t RTM_type;
	
	
};

struct mod_t *mod_create(char *name, enum mod_kind_t kind, int num_ports,
	int block_size, int latency);
void mod_free(struct mod_t *mod);
void mod_dump(struct mod_t *mod, FILE *f);
void mod_stack_set_reply(struct mod_stack_t *stack, int reply);
struct mod_t *mod_stack_set_peer(struct mod_t *peer, int state);

long long mod_access(struct mod_t *mod, enum mod_access_kind_t access_kind,
	unsigned int addr, int *witness_ptr, struct linked_list_t *event_queue,
	void *event_queue_item, struct mod_client_info_t *client_info);
int mod_can_access(struct mod_t *mod, unsigned int addr);

int mod_find_block(struct mod_t *mod, unsigned int addr, int *set_ptr, int *way_ptr,
	int *tag_ptr, int *state_ptr);

void mod_set_prefetched_bit(struct mod_t *mod, unsigned int addr, int val);
int mod_get_prefetched_bit(struct mod_t *mod, unsigned int addr);

void mod_lock_port(struct mod_t *mod, struct mod_stack_t *stack, int event);
void mod_unlock_port(struct mod_t *mod, struct mod_port_t *port,
	struct mod_stack_t *stack);

void mod_access_start(struct mod_t *mod, struct mod_stack_t *stack,
	enum mod_access_kind_t access_kind);
void mod_access_finish(struct mod_t *mod, struct mod_stack_t *stack);

int mod_in_flight_access(struct mod_t *mod, long long id, unsigned int addr);
struct mod_stack_t *mod_in_flight_address(struct mod_t *mod, unsigned int addr,
	struct mod_stack_t *older_than_stack);
struct mod_stack_t *mod_in_flight_write(struct mod_t *mod,
	struct mod_stack_t *older_than_stack);

int mod_serves_address(struct mod_t *mod, unsigned int addr);
struct mod_t *mod_get_low_mod(struct mod_t *mod, unsigned int addr);

int mod_get_retry_latency(struct mod_t *mod);

struct mod_stack_t *mod_can_coalesce(struct mod_t *mod,
	enum mod_access_kind_t access_kind, unsigned int addr,
	struct mod_stack_t *older_than_stack);
void mod_coalesce(struct mod_t *mod, struct mod_stack_t *master_stack,
	struct mod_stack_t *stack);

struct mod_client_info_t *mod_client_info_create(struct mod_t *mod);
struct mod_client_info_t *mod_client_info_clone(struct mod_t *mod, struct mod_client_info_t *original);
void mod_client_info_free(struct mod_t *mod, struct mod_client_info_t *client_info);

/* Up down reset of stats across memory hierarchy */
void mod_recursive_reset_stats(struct mod_t *mod);
void mod_reset_stats(struct mod_t *mod);


/* Prefetch */
int mod_find_pref_block(struct mod_t *mod, unsigned int addr, int *pref_stream_ptr, int* pref_slot_ptr);
int mod_find_block_in_stream(struct mod_t *mod, unsigned int addr, int stream);

void mod_adapt_pref_schedule(struct mod_t *mod);
void mod_adapt_pref_handler(int event, void *data);

void mod_interval_report_init(struct mod_t *mod);
void mod_interval_report(struct mod_t *mod);

//Header penalization, Hugo
//struct mod_last_used_set_t *mod_last_used_set_create(int num_sets, int assoc);
struct RTM_data_t *RTM_data_create(int num_sets, int assoc, int headers);
void reset_shift_stats();
#endif
