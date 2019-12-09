#include <assert.h>
#include <float.h> /* DBL_MAX */
#include <math.h> /* round() */
#include <stdbool.h>

#include <arch/x86/emu/context.h>
#include <lib/esim/esim.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/list.h>
#include <lib/util/misc.h>

#include "atd.h"
#include "cache-partitioning.h"
#include "module.h"
#include "fcp.h"


/* Max number of historical values for unfairness that can be stored */
#define FCP_HISTORY_SIZE 3

/* If unfairness increases in FCP_MAX_INC_INT consecutive intervals and
 * FCP is enabled, then transition to BACKOFF state. */
#define FCP_MAX_INC_INT 2

/* Fraction of the cache that needs to be used before FCP is enabled.
 * This is to ensure cache is warmed up. */
#define FCP_CACHE_TH 0.99

/* Time in picoseconds after which FCP will be enabled.
 * This is to ensure cache is warmed up. */
#define FCP_TIME_TH 1e11

/* Minimum relative difference in order to consider unfairness is increasing. */
#define FCP_UNF_INC_TH 1.02

/* Duration of backoff state. */
#define FCP_BACKOFF_INTERVALS 5

/* Minimum number of ways per thread. */
#define FCP_MIN_WAYS 1


/*
* Private structures
*/


/* Pair of threads that will exchange ways */
struct fcp_pair_t
{
	int id1;
	int id2;
	int ways; /* Total number of ways assigned to this pair */
};

struct fcp_thread_t
{
	int id;
	int *assigned_ways;
	double individual_speedup;
	struct fcp_pair_t *pair;
};

enum fcp_state_t
{
	OFF = 0,
	ON,
	BACKOFF
};

struct fcp_t
{
	struct mod_t *mod;
	struct fcp_thread_t *data; /* One position per thread in the machine, non reachable threads are null */
	struct list_t *threads; /* List of non null camps in data data array */
	struct
	{
		struct list_t *pairs;
		enum thread_pairing_policy_t policy;         /* Policy to make pairs */
		long long interval;                          /* Number of calls to the cache repartitioning handler before changing thread pairs */
	} pairing;

	enum fcp_state_t state;                          /* Current state for the FSM */

	double last_unfairness_values[FCP_HISTORY_SIZE]; /* Circular buffer of past unfairness values */

	int counter;                                     /* Number of calls to the repartitioning handler */
	int state_counter;                               /* Number of intervals in the same state */
	int backoff;                                     /* Remaining backoff cycles */

	long long last_esim_cycle;
};


/*
 * Public functions
 */


void* fcp_create(struct mod_t *mod)
{
	struct fcp_t *fcp;
	struct cache_t *cache = mod->cache;
	int total_num_threads;

	assert(mod);
	assert(cache);
	assert(cache->assigned_ways);
	assert(FCP_HISTORY_SIZE >= FCP_MAX_INC_INT);

	total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;

	fcp = xcalloc(1, sizeof(struct fcp_t));
	fcp->mod = mod;
	fcp->pairing.policy = cache->pairing.policy;
	fcp->pairing.interval = cache->pairing.interval;
	fcp->data = xcalloc(total_num_threads, sizeof(struct fcp_thread_t));

	/* Initialize assigned ways to invalid value */
	for (int i = 0; i < total_num_threads; i++)
		cache->assigned_ways[i] = -1;

	/* List of threads that can access this cache */
	fcp->threads = list_create();
	for (int i = 0; i < total_num_threads; i++)
	{
		if (mod->reachable_threads[i])
		{
			fcp->data[i].id = i;
			fcp->data[i].assigned_ways = &cache->assigned_ways[i];
			list_add(fcp->threads, &fcp->data[i]);
		}
	}

	/* List of pairs */
	fcp->pairing.pairs = list_create();

	return fcp;
}


void fcp_free(void *fcp_ptr)
{
	int i;
	struct fcp_t *fcp = (struct fcp_t *) fcp_ptr;
	if (!fcp) return;
	free(fcp->data);
	list_free(fcp->threads);
	LIST_FOR_EACH(fcp->pairing.pairs, i)
		free(list_get(fcp->pairing.pairs, i));
	list_free(fcp->pairing.pairs);
	free(fcp);
}


struct fcp_pair_t* fcp_pair_create(int id1, int id2, int ways)
{
	struct fcp_pair_t *pair = xcalloc(1, sizeof(struct fcp_pair_t));
	pair->id1 = id1;
	pair->id2 = id2;
	pair->ways = ways;
	return pair;
}


/* Organize tasks in pairs */
void fcp_pairing(struct fcp_t *fcp)
{
	struct cache_t *cache = fcp->mod->cache;
	const int count = list_count(fcp->threads);
	const int ways_per_pair = cache->assoc * 2 / count;

	assert(fcp);
	assert(count % 2 == 0);
	assert(ways_per_pair > 2); /* At least one per member */

	/* Clear previous pairing */
	while(list_count(fcp->pairing.pairs))
		free(list_pop(fcp->pairing.pairs));

	/* Make pairs */
	switch (fcp->pairing.policy)
	{
		/* No pairing */
		case thread_pairing_policy_none:
			return;

		/* Pair tasks by nearest id, 0-1, 2-3,... */
		case thread_pairing_policy_nearest:
		{
			for (int i = 0; i < count; i += 2)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, i + 1);
				struct fcp_pair_t *pair = fcp_pair_create(a->id, b->id, ways_per_pair);
				a->pair = pair;
				b->pair = pair;
				list_add(fcp->pairing.pairs, pair);
			}
			break;
		}

		case thread_pairing_policy_random:
			fatal("%s: Not implemented", __FUNCTION__);
			break;

		/* Pair, iteratively, the most progressing with the least progressing thread */
		case thread_pairing_policy_minmax:
		{
			/* Ensure that the IS are ordered */
			for (int i = 0; i < count - 1; i++)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, i + 1);
				assert(a->individual_speedup <= b->individual_speedup);
			}

			/* Pair tasks taking the most progressing and the least progressing iteratively */
			for (int i = 0; i < count / 2; i++)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, count - i - 1);
				struct fcp_pair_t *pair = fcp_pair_create(a->id, b->id, ways_per_pair);
				a->pair = pair;
				b->pair = pair;
				list_add(fcp->pairing.pairs, pair);
			}
			break;
		}

		/* Pair by is */
		case thread_pairing_policy_sec:
		{
			/* Ensure that the IS are ordered */
			for (int i = 0; i < count - 1; i++)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, i + 1);
				assert(a->individual_speedup <= b->individual_speedup);
			}

			/* Pair tasks taking the most progressing and the least progressing iteratively */
			for (int i = 0; i < count -1; i +=2)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, i + 1);
				struct fcp_pair_t *pair = fcp_pair_create(a->id, b->id, ways_per_pair);
				a->pair = pair;
				b->pair = pair;
				list_add(fcp->pairing.pairs, pair);
			}
			break;
		}

		case thread_pairing_policy_mix:
		{
			/* Ensure that the IS are ordered */
			for (int i = 0; i < count - 1; i++)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, i + 1);
				assert(a->individual_speedup <= b->individual_speedup);
			}

			/* Pair tasks taking the most progressing and the least progressing iteratively */
			for (int i = 0; i < count / 2; i++)
			{
				struct fcp_thread_t *a = list_get(fcp->threads, i);
				struct fcp_thread_t *b = list_get(fcp->threads, count / 2 + i);
				struct fcp_pair_t *pair = fcp_pair_create(a->id, b->id, ways_per_pair);
				a->pair = pair;
				b->pair = pair;
				list_add(fcp->pairing.pairs, pair);
			}
			break;
		}

		default:
			fatal("%s: Wrong value", __FUNCTION__);
			break;
	}

	assert(count / 2 == list_count(fcp->pairing.pairs));
}


/* Compute is */
void fcp_individual_speedup(struct fcp_t *fcp)
{
	int core;
	int thread;

	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int thread_id = core * x86_cpu_num_threads + thread;
		if (fcp->mod->reachable_threads[thread_id])
		{
			/* IPC */
			double ipc = esim_cycle() ? (double)  X86_THREAD.num_committed_uinst / esim_cycle() : 0;

			/* IPC alone int */
			double ipc_alone = esim_cycle() - X86_THREAD.interthread_penalty_cycles ? (double) X86_THREAD.num_committed_uinst / (esim_cycle() - X86_THREAD.interthread_penalty_cycles) : 0;

			/* Individual Speedup */
			fcp->data[thread_id].individual_speedup = ipc / ipc_alone;
		}
	}
}


int compare_is(const void *thread1, const void *thread2)
{
	struct fcp_thread_t *t1 = (struct fcp_thread_t *) thread1;
	struct fcp_thread_t *t2 = (struct fcp_thread_t *) thread2;
	return (t1->individual_speedup < t2->individual_speedup) ?
			-1 : (t1->individual_speedup > t2->individual_speedup);
}


/* Enforce the number of ways per pair specified in the pair struct. */
void fcp_pairing_adjust_ways(struct fcp_t *fcp)
{
	int i;
	int ways = 0;
	struct cache_t *cache = fcp->mod->cache;

	/* Recover ways from the pairs that have too many.
	 * The ways are equitatively taken from all the pair members, ensuring that each has at least one way.*/
	LIST_FOR_EACH(fcp->pairing.pairs, i)
	{
		struct fcp_pair_t *pair = list_get(fcp->pairing.pairs, i);
		struct fcp_thread_t *a = &fcp->data[pair->id1];
		struct fcp_thread_t *b = &fcp->data[pair->id2];
		int status = *a->assigned_ways + *b->assigned_ways - pair->ways;

		/* The number of ways assigned to the pair must be at least 2 (one way per member) and guarantee 2 ways to every other pair */
		assert(pair->ways >= 2 && pair->ways <= cache->assoc - (list_count(fcp->pairing.pairs) - 1) * 2);

		assert(*a->assigned_ways > 0);
		assert(*b->assigned_ways > 0);

		if (status > 0)
		{
			int *assigned_ways[2] = {a->assigned_ways, b->assigned_ways};
			int count = 0;
			ways += status;
			while (status > 0)
			{
				if (*assigned_ways[count] > 1)
				{
					(*assigned_ways[count])--;
					status--;
				}
				count = (count + 1) % 2;
			}
		}
	}

	/* Reassign recovered ways. The same process aplied for recovering ways is applied reversed. */
	if (ways > 0)
	{
		LIST_FOR_EACH(fcp->pairing.pairs, i)
		{
			struct fcp_pair_t *pair = list_get(fcp->pairing.pairs, i);
			struct fcp_thread_t *a = &fcp->data[pair->id1];
			struct fcp_thread_t *b = &fcp->data[pair->id2];
			int status = *a->assigned_ways + *b->assigned_ways - pair->ways;

			if (status < 0)
			{
				int* assigned_ways[2] = {a->assigned_ways, b->assigned_ways};
				int count = 0;
				ways += status; /* Status is negative */
				while (status < 0)
				{
					if (*assigned_ways[count] < pair->ways)
					{
						(*assigned_ways[count])++;
						status++;
					}
					count = (count + 1) % 2;
				}
			}

			assert(ways >= 0);

			if (ways == 0)
				break;
		}
	}
}


/* The number of ways per thread are stored in the assigned_ways vector, which has x86_cpu_num_cores * x86_cpu_num_threads size */
void fcp_repartition(void *fcp_ptr)
{
	struct fcp_t *fcp = (struct fcp_t *) fcp_ptr;
	struct mod_t *mod = fcp->mod;
	struct cache_t *cache = fcp->mod->cache;
	struct fcp_thread_t *min;
	struct fcp_thread_t *max;

	enum fcp_state_t last_state;

	long long cycles_int;

	double cache_usage = 0; /* Percentage of the cache that is full */
	double unfairness;

	int total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;

	bool pairs_modified = false;

	assert(fcp);
	cycles_int = esim_cycle() - fcp->last_esim_cycle;
	assert(cycles_int > 0);

	/* Compute cache usage */
	for (int i = 0; i < total_num_threads; i++)
		if (mod->reachable_threads[i])
			cache_usage += cache->used_ways[i];
	cache_usage = cache_usage / cache->num_sets / cache->assoc;

	/* Caches not warmed up */
	if (esim_time <= FCP_TIME_TH || cache_usage <= FCP_CACHE_TH)
		return;

	/* Fill fcp->is structure */
	fcp_individual_speedup(fcp);
	list_sort(fcp->threads, compare_is);

	/* Compute unfairness */
	min = list_head(fcp->threads);
	max = list_tail(fcp->threads);
	assert(max->individual_speedup >= min->individual_speedup);
	unfairness = max->individual_speedup / min->individual_speedup;


	/*
	 * Pairing
	 */


	if (fcp->pairing.policy)
	{
		/* If pairing interval is 0 then pairs are made only the first time
		 * and not modified after that */
		if (fcp->pairing.interval == 0)
		{
			if (list_count(fcp->pairing.pairs) == 0)
			{
				fcp_pairing(fcp);
				pairs_modified = true;
			}
		}

		/* Redo pairs */
		else if (fcp->counter % fcp->pairing.interval == 0)
		{
			fcp_pairing(fcp);
			pairs_modified = true;
		}
	}


	/*
	 * Transition and perform actions associated to this transition
	 */


	last_state = fcp->state;
	switch (fcp->state)
	{
		case BACKOFF:
			/* BACKOFF -> BACKOFF */
			if (fcp->backoff > 0)
				break;
			assert(fcp->backoff == 0);
			/* Intended fall trough */

		case OFF:
		{
			/* OFF -> ON */
			/* BACKOFF -> ON */

			int ways = cache->assoc;

			/* (Re)enable partitioning. Assign ways based on the state of the cache if previous value was invalid.
			 * The number of ways assigned must be >=1, but when we start to enforce fairness
			 * the first assignation of ways reflects the state of the cache, so for a thread,
			 * theoretically, the number of assigned ways could be 0. If for any of the threads
			 * it is 0, we add an extra way and substract it from the first thread that has more
			 * than one way. */

			assert(esim_time > FCP_TIME_TH && cache_usage > FCP_CACHE_TH); /* Caches are propperly warmed up */

			for (int i = 0; i < total_num_threads; i++)
			{
				if (mod->reachable_threads[i])
				{
					cache->assigned_ways[i] = round((double) cache->used_ways[i] / cache->num_sets);

					if (cache->assigned_ways[i] == 0)
					{
						cache->assigned_ways[i] = 1;
						ways -= 1;
					}
					else
					{
						ways -= cache->assigned_ways[i];
					}
				}
			}

			/* Address extra or remaining ways */
			for (int i = 0; ways != 0; i++)
			{
				int thread_id = i % total_num_threads;
				if (!mod->reachable_threads[thread_id])
					continue;

				if (ways > 0)
				{
					cache->assigned_ways[thread_id]++;
					ways--;
				}

				if (ways < 0 && cache->assigned_ways[thread_id] > 1)
				{
					cache->assigned_ways[thread_id]--;
					ways++;
				}
			}

			fcp->state = ON;

			break;
		}


		case ON:
		{
			double current = unfairness;
			bool unfairness_increasing;

			/* Decide if unfairness is steadly increasing. We asume unfairness is increasig
			 * if it increases for FCP_MAX_INC_INT consecutive intervals. */

			/* We have enough historical values to decide */
			if (fcp->state_counter >= FCP_MAX_INC_INT)
			{
				unfairness_increasing = true;
				for (int i = 1; i <= FCP_MAX_INC_INT; i++)
				{
					double prev = fcp->last_unfairness_values[(fcp->state_counter - i) % FCP_HISTORY_SIZE];
					if (current <= prev * FCP_UNF_INC_TH)
					{
						unfairness_increasing = false;
						break;
					}
					current = prev;
				}
			}

			/* We do not have enough data to decide */
			else
			{
				unfairness_increasing = false;
			}

			/* ON -> BACKOFF */
			if (unfairness_increasing)
			{
				fcp->backoff = FCP_BACKOFF_INTERVALS;
				fcp->state = BACKOFF;
			}

			/* Disable partitioning if leaving ON state */
			if (fcp->state != ON)
				for (int i = 0; i < total_num_threads; i++)
					if (mod->reachable_threads[i])
						cache->assigned_ways[i] = -1;

			break;
		}


		default:
			fatal("Unknown FCP state: %s", __FUNCTION__);

	}


	/*
	 * Perform actions associated to the state
	 */


	switch (fcp->state)
	{
		case OFF:
			for (int i = 0; i < total_num_threads; i++)
				if (mod->reachable_threads[i])
					assert(cache->assigned_ways[i] == -1);
			break;

		case ON:
		{
			/* Transfer of pairs without pairing */
			if (!fcp->pairing.policy)
			{
				int ways;

				/* Adjust assigned ways */
				if (*max->assigned_ways > FCP_MIN_WAYS)
				{
					(*max->assigned_ways)--;
					(*min->assigned_ways)++;
				}

				/* Assertions */
				ways = 0;
				for (int i = 0; i < total_num_threads; i++)
				{
					if (mod->reachable_threads[i])
					{
						assert(cache->assigned_ways[i] > 0);
						ways += cache->assigned_ways[i];
					}
				}
				assert(ways == cache->assoc);
				assert(*min->assigned_ways > 0);
				assert(*max->assigned_ways > 0);
				assert(*min->assigned_ways <= cache->assoc - mod->num_reachable_threads + 1);
				assert(*max->assigned_ways <= cache->assoc - mod->num_reachable_threads + 1);
			}

			/* Intrapair transfer of ways */
			else
			{
				int i;

				assert(list_count(fcp->pairing.pairs) > 0);

				LIST_FOR_EACH(fcp->pairing.pairs, i)
				{
					struct fcp_pair_t *pair = list_get(fcp->pairing.pairs, i);

					assert(pair->ways >= 2); /* At least one way per task */

					if (fcp->data[pair->id1].individual_speedup > fcp->data[pair->id2].individual_speedup)
					{
						min = &fcp->data[pair->id2];
						max = &fcp->data[pair->id1];
					}

					else if (fcp->data[pair->id1].individual_speedup < fcp->data[pair->id2].individual_speedup)
					{
						min = &fcp->data[pair->id1];
						max = &fcp->data[pair->id2];
					}

					/* Adjust assigned ways */
					if (*max->assigned_ways > FCP_MIN_WAYS)
					{
						(*max->assigned_ways)--;
						(*min->assigned_ways)++;
					}
				}
			}

			if (pairs_modified)
				fcp_pairing_adjust_ways(fcp);

			break;
		}

		case BACKOFF:
			assert(fcp->backoff > 0);
			for (int i = 0; i < total_num_threads; i++)
				if (mod->reachable_threads[i])
					assert(cache->assigned_ways[i] == -1);
			fcp->backoff--;
			break;

		default:
			fatal("Unknown FCP state: %s", __FUNCTION__);
	}

	/* If FCP changes state, then clear historical data */
	if (fcp->state != last_state)
	{
		fcp->state_counter = 0;
	}

	/* FCP remains in the same state so record data */
	else
	{
		/* Mantain an history of unfairness values */
		fcp->last_unfairness_values[fcp->state_counter % FCP_HISTORY_SIZE] = unfairness;

		/* Count the number of intervals FCP has remained in the same state.
		 * This counter is reset when there is a state change. */
		fcp->state_counter++;
	}

	/* Number of calls */
	fcp->counter++;

	/* Store general values */
	fcp->last_esim_cycle = esim_cycle();
}
