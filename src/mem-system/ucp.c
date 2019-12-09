#include <assert.h>

#include <arch/x86/timing/cpu.h>
#include <arch/x86/emu/context.h>
#include <lib/esim/esim.h>
#include <lib/mhandle/mhandle.h>

#include "atd.h"
#include "module.h"
#include "ucp.h"


/* Fraction of the cache that needs to be used before UCP is enabled.
 * This is to ensure cache is warmed up. */
#define UCP_CACHE_TH 0.99


/*
* Private structures
*/


struct ucp_t
{
	struct atd_t **atds;    /* Array of ATDs, one per thread */
	struct mod_t *mod;
	int *assigned_ways;     /* Ways are allocated per thread */
	int *ways_req;          /* Ways required per thread */
	double *max_mu;         /* Maximum marginal utility per thread */
};


/*
 * Private functions
 */


/* Marginal utility for the thread when the number of ways assigned to it increasses from old_alloc to new_alloc */
double ucp_mu(struct ucp_t *ucp, int thread_id, int alloc, int new_alloc)
{
	long long extra_hits = 0;

	assert(alloc < new_alloc);

	for (int i = alloc; i < new_alloc; i++)
		extra_hits += ucp->atds[thread_id]->stack_distance_counters[i];

	return (double) extra_hits / (new_alloc - alloc);
}


/* Compute maximum marginal utility */
void ucp_max_mu(struct ucp_t *ucp, int thread_id, int alloc, int balance, double *max_mu, int *ways_req)
{
	*max_mu = 0;   /* Maximum marginal utility */
	*ways_req = 0; /* Ways required for this mu */
	for (int i = 1; i <= balance; i++)
	{
		double mu = ucp_mu(ucp, thread_id, alloc, alloc + i);
		if (mu > *max_mu)
		{
			*max_mu = mu;
			*ways_req = i;
		}
	}
}


/*
 * Public functions
 */


/* ATDs and assigned_ways are references to already allocated data structures */
void* ucp_create(struct mod_t *mod)
{
	struct ucp_t *ucp;
	struct atd_t **atds;
	int *assigned_ways;
	int assoc;
	int total_num_threads;

	assert(mod);

	ucp = xcalloc(1, sizeof(struct ucp_t));
	atds = mod->atd_per_thread;
	assigned_ways = mod->cache->assigned_ways;
	assoc = mod->cache->assoc;
	total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;

	assert(atds);
	assert(assigned_ways);
	assert(assoc);

	ucp->mod = mod;
	ucp->atds = atds;
	ucp->assigned_ways = assigned_ways;
	ucp->ways_req = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(int));
	ucp->max_mu = xcalloc(x86_cpu_num_cores * x86_cpu_num_threads, sizeof(double));

	/* Initialize to invalid value */
	for (int i = 0; i < total_num_threads; i++)
		assigned_ways[i] = -1;
	return ucp;
}


void ucp_free(void *ucp_ptr)
{
	struct ucp_t *ucp = (struct ucp_t *) ucp_ptr;
	if (!ucp) return;
	free(ucp->ways_req);
	free(ucp->max_mu);
	free(ucp);
}


/* The number of ways per thread are stored in the assigned_ways vector, which has x86_cpu_num_cores * x86_cpu_num_threads size */
void ucp_repartition(void *ucp_ptr)
{

	struct ucp_t *ucp = (struct ucp_t *) ucp_ptr;
	struct mod_t *mod = ucp->mod;
	struct cache_t *cache = mod->cache;
	double cache_usage = 0; /* Percentage of the cache that is full */
	int total_num_threads = x86_cpu_num_cores * x86_cpu_num_threads;
	int balance;
	int last_balance;
	int core;
	int thread;

	assert(ucp);

	balance = cache->assoc;
	last_balance = balance;

	/* Compute cache usage */
	for (int i = 0; i < total_num_threads; i++)
		if (mod->reachable_threads[i])
			cache_usage += cache->used_ways[i];
	cache_usage = cache_usage / cache->num_sets / cache->assoc;

	/* Start when caches are warmed up */
	if (cache_usage < UCP_CACHE_TH)
		return;

	/* Set assigned_ways to 1 for the threads that can reach this cache */
	for (int i = 0; i < total_num_threads; i++)
	{
		if (mod->reachable_threads[i])
		{
			ucp->assigned_ways[i] = 1;
			balance--;
		}
	}

	while(balance)
	{
		int winner = -1;
		double max = -1;

		/* Compute max marginal utilities per thread */
		for (int thread_id = 0; thread_id < total_num_threads; thread_id++)
		{
			int alloc;
			if (!ucp->mod->reachable_threads[thread_id]) continue;
			alloc = ucp->assigned_ways[thread_id];
			ucp_max_mu(ucp, thread_id, alloc, balance, &ucp->max_mu[thread_id], &ucp->ways_req[thread_id]);
		}

		/* Choose winner */
		for (int thread_id = 0; thread_id < total_num_threads; thread_id++)
		{
			if (ucp->max_mu[thread_id] > max)
			{
				max = ucp->max_mu[thread_id];
				winner = thread_id;
			}
		}

		assert(winner != -1);

		/* Allocate and update balance */
		ucp->assigned_ways[winner] += ucp->ways_req[winner];
		balance -= ucp->ways_req[winner];

		/* Break potential infinite loop */
		if (balance == last_balance)
			break;
		else
			last_balance = balance;
	}

	X86_CORE_FOR_EACH X86_THREAD_FOR_EACH
	{
		int thread_id = core * x86_cpu_num_threads + thread;
		if (ucp->atds[thread_id])
		{
			/* Halve values for stack distance counters, in order to
			 * retain past information while giving importance to recent information */
			for (int i = 0; i < total_num_threads; i++)
				ucp->atds[thread_id]->stack_distance_counters[i] /= 2;
			/* Trigger a report for the ctx, to report the changes in number of allocated ways */
			if (X86_THREAD.ctx)
				x86_ctx_interval_report(X86_THREAD.ctx);
		}
	}
}
