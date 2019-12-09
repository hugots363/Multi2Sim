#include <assert.h>

#include <arch/x86/timing/cpu.h>
#include <arch/x86/emu/context.h>
#include <lib/esim/esim.h>
#include <lib/util/debug.h>
#include <lib/util/misc.h>
#include <lib/util/string.h>

#include "module.h"


/*
 * Private structures
*/


struct cache_partitioning_t
{
	/* Pointer to the struct describing the partitioner e.g. fcp_t or ucp_t */
	void *partitioning;

	/* Pointers to the relevant functions of the partitioning policy */
	void (*execute_callback)(void *partitioning);
	void (*free_callback)(void *partitioning);

	/* Module / cache partitioned */
	struct mod_t *mod;

	/* Auxiliary data for scheduling purposes */
	long long last_esim_cycle;
	long long last_uinsts;
	long long last_evictions;
	int backoff;
};


/*
 * Private variables
*/


int EV_CACHE_PARTITIONING;
int cache_partitioning_domain_index;

struct str_map_t cache_partitioning_policy_map =
{
	4, {
		{ "None", cache_partitioning_policy_none },
		{ "Static", cache_partitioning_policy_static },
		{ "UCP", cache_partitioning_policy_ucp },
		{ "FCP", cache_partitioning_policy_fcp }
	}
};

struct str_map_t thread_pairing_policy_map =
{
	6, {
		{"None", thread_pairing_policy_none},
		{"Nearest", thread_pairing_policy_nearest},
		{"Random", thread_pairing_policy_random},
		{"MinMax", thread_pairing_policy_minmax},
		{"Sec", thread_pairing_policy_sec},
		{"Mix", thread_pairing_policy_mix},
	}
};


/*
 * Private functions
*/


void cache_partitioning_handler(int event, void *data)
{
	int core;
	int thread;

	const int def_backoff = 10000;
	const int min_backoff = 100;
	const int max_backoff = 500000;

	struct cache_partitioning_t *wrapper = data;
	struct mod_t *mod = wrapper->mod;
	struct cache_t *cache = mod->cache;

	long long cycles_int = esim_cycle() - wrapper->last_esim_cycle;
	long long uinsts = 0;

	assert(data);
	assert(event == EV_CACHE_PARTITIONING);

	/* Find out if an interval has finished */
	switch(cache->partitioning.interval_kind)
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
			uinsts_int = uinsts - wrapper->last_uinsts;

			#define METRIC uinsts_int
			#define METRIC_PER_CYCLE ipc_int

			/* Mean IPC for all the threads accessing this module */
			METRIC_PER_CYCLE = cycles_int ? (double) METRIC / cycles_int : 0.0;

			/* Try to predict when the next interval will begin */
			wrapper->backoff = METRIC_PER_CYCLE ? 0.75 * (cache->partitioning.interval - METRIC) / METRIC_PER_CYCLE : def_backoff;
			wrapper->backoff = MAX(wrapper->backoff, min_backoff);
			wrapper->backoff = MIN(wrapper->backoff, max_backoff);

			/* Interval has not finished yet */
			if (METRIC < cache->partitioning.interval)
				goto schedule_next_event;

			#undef METRIC
			#undef METRIC_PER_CYCLE

			break;
		}

		case interval_kind_evictions:
		{
			/* Evictions in this interval */
			long long evictions_int = mod->evictions - wrapper->last_evictions;
			double epc_int;

			#define METRIC evictions_int
			#define METRIC_PER_CYCLE epc_int

			/* Mean IPC for all the threads accessing this module */
			METRIC_PER_CYCLE = cycles_int ? (double) METRIC / cycles_int : 0.0;

			/* Try to predict when the next interval will begin */
			wrapper->backoff = METRIC_PER_CYCLE ? 0.75 * (cache->partitioning.interval - METRIC) / METRIC_PER_CYCLE : def_backoff;
			wrapper->backoff = MAX(wrapper->backoff, min_backoff);
			wrapper->backoff = MIN(wrapper->backoff, max_backoff);

			/* Interval has not finished yet */
			if (METRIC < cache->partitioning.interval)
				goto schedule_next_event;

			#undef METRIC
			#undef METRIC_PER_CYCLE

			break;
		}

		default:
			fatal("%s: Invalid interval kind", __FUNCTION__);
			break;
	}

	/* Partition */
	wrapper->execute_callback(wrapper->partitioning);

	/* Report */
	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int thread_id = core * x86_cpu_num_threads + thread;
		if (mod->reachable_threads[thread_id])
		{
			/* Trigger a report for the ctx, to report the changes in number of allocated ways */
			if (X86_THREAD.ctx)
				x86_ctx_interval_report(X86_THREAD.ctx);
		}
	}
	/* Trigger a report for the module, to report the changes in the number of allocated ways per thread */
	mod_interval_report(mod);

	/* Store values for the next interval */
	switch(cache->partitioning.interval_kind)
	{
		case interval_kind_cycles:
			wrapper->last_esim_cycle = esim_cycle();
			break;

		case interval_kind_instructions:
			wrapper->last_uinsts = uinsts;
			break;

		case interval_kind_evictions:
			wrapper->last_evictions = mod->evictions;
			break;

		default:
			fatal("%s: Invalid interval kind", __FUNCTION__);
			break;
	}

schedule_next_event:
	if (!esim_finish)
	{
		esim_schedule_event(EV_CACHE_PARTITIONING, wrapper, cache->partitioning.interval_kind == interval_kind_cycles ?
				cache->partitioning.interval :
				wrapper->backoff);
	}
	else
	{
		wrapper->free_callback(wrapper->partitioning);
		free(wrapper);
	}
}


/*
 * Public functions
*/


struct cache_partitioning_t* cache_partitioning_create(
		struct mod_t *mod,
		void* (*create_callback)(struct mod_t *mod),
		void (*free_callback)(void *partitioning),
		void (*execute_callback)(void *partitioning))
{
	struct cache_partitioning_t *wrapper = xcalloc(1, sizeof(struct cache_partitioning_t));
	assert(create_callback);
	assert(free_callback);
	assert(execute_callback);
	wrapper->mod = mod;
	wrapper->partitioning = create_callback(mod);
	wrapper->execute_callback = execute_callback;
	wrapper->free_callback = free_callback;
	return wrapper;
}


void cache_partitioning_free(struct cache_partitioning_t* wrapper)
{
	wrapper->free_callback(wrapper->partitioning);
	free(wrapper);
}


void cache_partitioning_schedule(struct cache_partitioning_t *wrapper)
{
	struct cache_t *cache;

	assert(wrapper);
	assert(wrapper->mod->cache->policy == cache_policy_partitioned_lru);

	cache = wrapper->mod->cache;

	/* New domain and event for cache partitioning.
	*  This is only done the first time this function is executed,
	*  since it will be executed for every cache module that uses a partitioning policy. */
	if (!EV_CACHE_PARTITIONING && !cache_partitioning_domain_index)
	{
		cache_partitioning_domain_index = esim_new_domain(esim_frequency);
		EV_CACHE_PARTITIONING = esim_register_event_with_name(cache_partitioning_handler, cache_partitioning_domain_index, "cache_partitioning");
	}

	assert(EV_CACHE_PARTITIONING);
	assert(cache_partitioning_domain_index);
	assert(cache->partitioning.interval_kind);
	assert(cache->partitioning.interval);

	/* Schedule first event */
	esim_schedule_event(EV_CACHE_PARTITIONING, wrapper, cache->partitioning.interval_kind == interval_kind_cycles ?
			cache->partitioning.interval : wrapper->backoff);
}
