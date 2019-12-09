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

#ifndef MEM_SYSTEM_ATD_H
#define MEM_SYSTEM_ATD_H

#include "cache.h"

struct atd_block_t
{
	struct atd_block_t *way_next;
	struct atd_block_t *way_prev;
	int tag;
	int way;
	enum cache_block_state_t state;
};

struct atd_set_t
{
	struct atd_block_t *way_head;
	struct atd_block_t *way_tail;
	struct atd_block_t *blocks;
};

struct atd_t
{
	int num_sets;
	struct mod_t *mod;
	struct atd_set_t *sets;

	long long *stack_distance_counters;
};


struct atd_t *atd_create(struct mod_t *mod, int num_sets);
void atd_free(struct atd_t *atd);

int atd_set_block(struct atd_t *atd, int addr, int state);
int atd_find_block(struct atd_t *atd, unsigned int addr, int *set_ptr, int *way_ptr, int *tag_ptr, int *state_ptr);
int atd_access_block(struct atd_t *atd, unsigned int addr);

void atd_update_stack_distance_counters(struct atd_t *atd, int set, int way);

#endif /* MEM_SYSTEM_ATD_H */

