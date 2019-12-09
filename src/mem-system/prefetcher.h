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

#ifndef MEM_SYSTEM_PREFETCHER_H
#define MEM_SYSTEM_PREFETCHER_H

#include <lib/util/interval-kind.h>

/*
 * This file implements a global history buffer
 * based prefetcher. Refer to the 2005 paper by
 * Nesbit and Smith.
 */

extern struct str_map_t prefetcher_type_map;
extern struct str_map_t adapt_pref_policy_map;
extern struct str_map_t interval_kind_map;

enum prefetcher_type_t
{
	prefetcher_type_invalid = 0,
	prefetcher_type_pc_cs,
	prefetcher_type_pc_dc,
	prefetcher_type_pc_cs_sb,
	prefetcher_type_cz_cs_sb,
	prefetcher_type_cz_cs,
};

/* Doesn't really make sense to have a big lookup depth */
#define PREFETCHER_LOOKUP_DEPTH_MAX 4

/* Global history buffer. */
struct prefetcher_ghb_t
{
	/* The global address miss this entry corresponds to */
	unsigned int addr;
	/* Next element in the linked list : -1 implies none */
	int next;
	/* Previous element in the linked list : -1 implies none */
	int prev;
	/* Whether the previous element is a GHB entry or a pointer
	 * to the index table. */
	enum prefetcher_ptr_t
	{
		prefetcher_ptr_ghb = 0,
		prefetcher_ptr_it,
	} prev_it_ghb;
};

/* Index table. */
struct prefetcher_it_t
{
	/* The tag to compare to before indexing into the table */
	unsigned int tag;
	/* Pointer into the GHB. -1 implies no entry in GHB. */
   	int ptr;
};

/* Adaptive policy */
enum adapt_pref_policy_t
{
	adapt_pref_policy_none = 0,
	adapt_pref_policy_adp,
	adapt_pref_policy_fdp,
	adapt_pref_policy_hpac,
	adapt_pref_policy_mbp,
};

/* Thresholds for adaptive algorithms */
struct thresholds_t
{
	struct
	{
		int a1; /* Aggr level */
		int a2;
		int a3;
		double acc_high;
		double acc_low;
		double acc_very_low;
		double cov;
		double bwno;
		double rob_stall;
		double ipc;
		double misses;
	} adp;
	struct
	{
		int a1; /* Aggr level */
		int a2;
		int a3;
		double acc_high;
		double acc_low;
		double lateness;
		double pollution;
	} fdp;
	struct
	{
		int a1; /* Aggr level */
		int a2;
		int a3;
		double acc;
		double bwno;
		double bwc;
		double pollution;
	} hpac;
	struct
	{
		int a1; /* Aggr level */
		int a2;
		int a3;
		double ratio; /* Ratio exploration / exploitation */
	} mbp;
};

/* The main prefetcher object */
struct prefetcher_t
{
	enum prefetcher_type_t type; 	/* Type of prefetcher */
	int enabled;
	int ghb_size;
	int it_size;
	int lookup_depth;
	int aggr;

	struct prefetcher_ghb_t *ghb;
	struct prefetcher_it_t *index_table;
	int ghb_head;

	/* CZone prefetchers */
	int czone_bits;					/* Size in bits of the czone */
	unsigned int czone_mask; 		/* For obtaining czone */

	/* Streaming prefetchers */
	int max_num_streams;			/* Max number of streams */
	int max_num_slots;				/* Max number of blocks per stream */
	int distance;					/* How ahead of the accesses of the processor this prefetcher is. If buffers are used, this can't be greater than max_num_slots. */
	int stream_tag_bits;			/* Size in bits of the tag */
	unsigned int stream_tag_mask;	/* For obtaining stream_tag */
	struct stream_buffer_t *streams;
	struct stream_buffer_t *stream_head;
	struct stream_buffer_t *stream_tail;

	/* Adaptive prefetchers */
	enum adapt_pref_policy_t adapt_policy;		/* Adaptative policy used */
	unsigned int aggr_ini;						/* Initial aggressivity */
	long long adapt_interval;					/* Interval at wich the adaptative policy is aplied */
	enum interval_kind_t adapt_interval_kind;	/* What units are used for the interval */
	int bloom_bits;								/* For pollution filters */
	int bloom_capacity;
	double bloom_false_pos_prob;

	/* Thresholds */
	struct thresholds_t th;

	struct cache_t *parent_cache;
};


struct mod_client_info_t;
struct mod_stack_t;
struct mod_t;


struct prefetcher_t *prefetcher_create(int prefetcher_ghb_size, int prefetcher_it_size, int prefetcher_lookup_depth, enum prefetcher_type_t type, int aggr);
void prefetcher_stream_buffers_create(struct prefetcher_t *pref, int max_num_streams, int max_num_slots);
void prefetcher_free(struct prefetcher_t *pref);

int prefetcher_update_tables(struct mod_stack_t *stack);

void prefetcher_cache_miss(struct mod_stack_t *stack, struct mod_t *mod);
void prefetcher_cache_hit(struct mod_stack_t *stack, struct mod_t *mod);

void prefetcher_stream_buffer_hit(struct mod_stack_t *stack);
void prefetcher_stream_buffer_miss(struct mod_stack_t *stack);

int prefetcher_uses_stream_buffers(struct prefetcher_t *pref);
int prefetcher_uses_pc_indexed_ghb(struct prefetcher_t *pref);
int prefetcher_uses_czone_indexed_ghb(struct prefetcher_t *pref);
int prefetcher_uses_pollution_filters(struct prefetcher_t *pref);

void prefetcher_set_default_adaptive_thresholds(struct prefetcher_t *pref);
#endif
