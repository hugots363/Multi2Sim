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

#pragma once

struct cache_partitioning_t;
struct mod_t *mod;


extern struct str_map_t cache_partitioning_policy_map;
extern struct str_map_t thread_pairing_policy_map;


enum cache_partitioning_policy_t
{
	cache_partitioning_policy_none = 0,
	cache_partitioning_policy_static,
	cache_partitioning_policy_ucp,
	cache_partitioning_policy_fcp,
};

enum thread_pairing_policy_t
{
	thread_pairing_policy_none = 0,
	thread_pairing_policy_nearest,
	thread_pairing_policy_random,
	thread_pairing_policy_minmax,
	thread_pairing_policy_sec,
	thread_pairing_policy_mix,
};


struct cache_partitioning_t* cache_partitioning_create(
		struct mod_t *mod,
		void* (*create_callback)(struct mod_t *mod),
		void (*free_callback)(void *partitioning),
		void (*execute_callback)(void *partitioning));
void cache_partitioning_free(struct cache_partitioning_t* wrapper);
void cache_partitioning_schedule(struct cache_partitioning_t *wrapper);
