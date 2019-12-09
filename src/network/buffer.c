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

#include <assert.h>

#include <lib/esim/esim.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>

#include "buffer.h"
#include "net-system.h"
#include "network.h"
#include "node.h"


/*
 * Public Functions
 */

struct net_buffer_t *net_buffer_create(struct net_t *net,
	struct net_node_t *node, int size, char *name)
{
	struct net_buffer_t *buffer;

	/* Fields */
	buffer = xcalloc(1, sizeof(struct net_buffer_t));
	buffer->frag_list = list_create();
	buffer->msg_list = list_create();
	buffer->wakeup_list = linked_list_create();
	buffer->net = net;
	buffer->node = node;
	buffer->name = xstrdup(name);
	buffer->size = size;
	if (size < 1)
		panic("%s: invalid size", __FUNCTION__);

	/* Return */
	return buffer;
}


void net_buffer_free(struct net_buffer_t *buffer)
{
	/* Free wakeup list */
	LINKED_LIST_FOR_EACH(buffer->wakeup_list)
		free(linked_list_get(buffer->wakeup_list));
	linked_list_free(buffer->wakeup_list);

	/* Free rest */
	list_free(buffer->frag_list);
	list_free(buffer->msg_list);
	free(buffer->name);
	free(buffer);
}


void net_buffer_dump(struct net_buffer_t *buffer, FILE *f)
{
	struct net_msg_frag_t *frag;
	int i;

	fprintf(f, "Buffer '%s':", buffer->name);
	for (i = 0; i < list_count(buffer->frag_list); i++)
	{
		frag = list_get(buffer->frag_list, i);
		fprintf(f, " %lld", frag->id);
	}
	fprintf(f, "\n");
}


void net_buffer_dump_report(struct net_buffer_t *buffer, FILE *f)
{
	long long cycle;

	/* Get current cycle */
	cycle = esim_domain_cycle(net_domain_index);

	/* Update stats */
	net_buffer_update_occupancy(buffer);

	/* Report */
	fprintf(f, "%s.Size = %d \n", buffer->name, buffer->size);
	fprintf(f, "%s.MessageOccupancy = %.2f\n", buffer->name, cycle ?
		(double) buffer->occupancy_msgs_acc / cycle : 0.0);
	fprintf(f, "%s.FragmentOccupancy = %.2f\n", buffer->name, cycle ?
		(double) buffer->occupancy_frags_acc / cycle : 0.0);
	fprintf(f, "%s.ByteOccupancy = %.2f\n", buffer->name, cycle ?
		(double) buffer->occupancy_bytes_acc / cycle : 0.0);
	fprintf(f, "%s.Utilization = %.4f\n", buffer->name, cycle ?
		(double) buffer->occupancy_bytes_acc / cycle /
		buffer->size : 0.0);
}


void net_buffer_insert(struct net_buffer_t *buffer, struct net_msg_frag_t *frag)
{
	struct net_t *net = buffer->net;
	struct net_node_t *node = buffer->node;

	long long cycle;
	cycle = esim_domain_cycle(net_domain_index);

	if (buffer->count + frag->size > buffer->size)
		panic("%s: not enough space in buffer", __FUNCTION__);
	buffer->count += frag->size;
	list_enqueue(buffer->frag_list, frag);

	/* If this is a header fragment or we are using Store And Forward, add parent to msg_list */
	if (frag->id == 0)
		list_enqueue(buffer->msg_list, frag->parent);

	/* Update occupancy stat */
	net_buffer_update_occupancy(buffer);

	/* Debug */
	net_debug("%lld: BUF -> "
			"a=\"insert\" "
			"net=\"%s\" "
			"msg=%lld "
			"frag=%lld "
			"node=\"%s\" "
			"buf=\"%s\"\n",
			cycle,
			net->name,
			frag->parent->id,
			frag->id,
			node->name,
			buffer->name);
}


void net_buffer_extract(struct net_buffer_t *buffer, struct net_msg_frag_t *frag)
{
	struct net_t *net = buffer->net;
	struct net_node_t *node = buffer->node;

	long long cycle;
	cycle = esim_domain_cycle(net_domain_index);

	assert(buffer->count >= frag->size);
	buffer->count -= frag->size;

	/* Extract message from list */

	if (!list_count(buffer->frag_list))
		panic("%s: empty fragment list", __FUNCTION__);

	if (!list_count(buffer->msg_list))
		panic("%s: empty message list", __FUNCTION__);

	if (!list_remove(buffer->frag_list, frag))
		panic("%s: fragment is not at buffer", __FUNCTION__);

	if (frag->id == list_count(frag->parent->fragments) - 1)
		if (!list_remove(buffer->msg_list, frag->parent))
			panic("%s: message is not in buffer", __FUNCTION__);

	/* Update occupancy stat */
	net_buffer_update_occupancy(buffer);

	/* Debug */
	net_debug("%lld: BUF -> "
		"a=\"extract\" "
		"net=\"%s\" "
		"msg=%lld "
		"frag=%lld "
		"node=\"%s\" "
		"buf=\"%s\"\n",
		cycle,
		net->name,
		frag->parent->id,
		frag->id,
		node->name,
		buffer->name);

	/* Schedule events waiting for space in buffer. */
	net_buffer_wakeup(buffer);
}


/* Schedule an event to be called when the buffer releases some space. */
void net_buffer_wait(struct net_buffer_t *buffer, int event, void *stack)
{
	struct net_buffer_wakeup_t *wakeup;

	/* No event */
	if (event == ESIM_EV_NONE)
		return;

	/* Create new event-stack element */
	assert(buffer->count > 0);
	wakeup = xmalloc(sizeof(struct net_buffer_wakeup_t));

	/* Add it to wakeup list */
	wakeup->event = event;
	wakeup->stack = stack;
	linked_list_add(buffer->wakeup_list, wakeup);
}


/* Schedule all events waiting in the wakeup list */
void net_buffer_wakeup(struct net_buffer_t *buffer)
{
	struct net_buffer_wakeup_t *wakeup;

	while (linked_list_count(buffer->wakeup_list))
	{
		/* Get event/stack */
		linked_list_head(buffer->wakeup_list);
		wakeup = linked_list_get(buffer->wakeup_list);
		linked_list_remove(buffer->wakeup_list);

		/* Schedule event */
		esim_schedule_event(wakeup->event, wakeup->stack, 0);
		free(wakeup);
	}
}


/* Update occupancy statistic */
void net_buffer_update_occupancy(struct net_buffer_t *buffer)
{
	long long cycles;
	long long cycle;

	/* Get current cycle */
	cycle = esim_domain_cycle(net_domain_index);

	/* Accumulate previous values */
	cycles = cycle - buffer->occupancy_measured_cycle;
	buffer->occupancy_bytes_acc += buffer->occupancy_bytes_value * cycles;
	buffer->occupancy_frags_acc += buffer->occupancy_frags_value * cycles;
	buffer->occupancy_msgs_acc += buffer->occupancy_msgs_value * cycles;

	/* Store new sample */
	buffer->occupancy_bytes_value = buffer->count;
	buffer->occupancy_frags_value = list_count(buffer->frag_list);
	buffer->occupancy_msgs_value = list_count(buffer->msg_list);
	buffer->occupancy_measured_cycle = cycle;
}
