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
#include <lib/util/list.h>

#include "buffer.h"
#include "bus.h"
#include "link.h"
#include "message.h"
#include "net-system.h"
#include "network.h"
#include "node.h"
#include "routing-table.h"

long long msg_id_counter;

/*
 * Message
 */


struct net_msg_t *net_msg_create(struct net_t *net, struct net_node_t *src_node, struct net_node_t *dst_node, int size)
{
	struct net_msg_t *msg;
	struct net_msg_frag_t *frag;
	long long cycle;


	/* Get current cycle */
	cycle = esim_domain_cycle(net_domain_index);

	/* Create a msg which will be composed of 1 or more fragments */
	msg = xcalloc(1, sizeof(struct net_msg_t));
	msg->size = size;
	msg->id = ++msg_id_counter;
	msg->arrived_frags_count = 0;

	switch (net->switching)
	{
		/* SAF - Store and Forward: every msg is composed of a single fragment */
		case network_switching_saf:
			msg->fragments = list_create_with_size(1);
			frag = xcalloc(1, sizeof(struct net_msg_frag_t));
			frag->net = net;
			frag->src_node = src_node;
			frag->dst_node = dst_node;
			frag->size = size;
			frag->parent = msg;
			frag->cycle_sent = cycle;
			list_add(msg->fragments, frag);

			net_debug("\n%lld: MSG -> "
					"a=create "
					"net=%s "
					"msg=%lld "
					"switching=SAF \n",
					cycle,
					net->name,
					msg->id);
			break;

		/* VCT - Virtual Cut-Through: one msg is composed of size/bandwidth fragments */
		case network_switching_vct:
		{
			int id;
			int num_fragments = (size - 1) / net->def_bandwidth + 1;
			int remaining = size % net->def_bandwidth; /* */

			msg->fragments = list_create_with_size(num_fragments + 1);

			/* Deal with all the fragments but the last */
			for (id = 0; id < num_fragments - 1; id++)
			{
				frag = xcalloc(1, sizeof(struct net_msg_frag_t));
				frag->net = net;
				frag->src_node = src_node;
				frag->dst_node = dst_node;
				frag->size = net->def_bandwidth;
				frag->parent = msg;
				frag->id = id;
				frag->cycle_sent = cycle;
				list_add(msg->fragments, frag);
			}

			/* Deal with last fragment wich may not be full sized */
			frag = xcalloc(1, sizeof(struct net_msg_frag_t));
			frag->net = net;
			frag->src_node = src_node;
			frag->dst_node = dst_node;
			frag->size = remaining ? remaining : net->def_bandwidth;
			frag->parent = msg;
			frag->id = id;
			frag->cycle_sent = cycle;
			list_add(msg->fragments, frag);

			net_debug("\n%lld: MSG -> "
					"a=create "
					"net=%s "
					"msg=%lld "
					"frags=%d "
					"switching=VCT \n",
					cycle,
					net->name,
					msg->id,
					list_count(msg->fragments));
			break;
		}
    }

	if(size < 1)
		panic("%s: bad size",  __FUNCTION__);

	return msg;
}

void net_msg_free(struct net_msg_t *msg)
{
	while (list_count(msg->fragments))
		free(list_remove_at(msg->fragments, 0));
	list_free(msg->fragments);

	free(msg);
}


/*
 * Event-driven Simulation
 */

struct net_stack_t *net_stack_create(struct net_t *net, int retevent, void *retstack)
{
	struct net_stack_t *stack;

	/* Initialize */
	stack = xcalloc(1, sizeof(struct net_stack_t));
	stack->net = net;
	stack->ret_event = retevent;
	stack->ret_stack = retstack;

	/* Return */
	return stack;
}


void net_stack_return(struct net_stack_t *stack)
{
	int retevent = stack->ret_event;
	struct net_stack_t *retstack = stack->ret_stack;

	free(stack);
	esim_schedule_event(retevent, retstack, 0);
}


void net_event_handler(int event, void *data)
{
	struct net_stack_t *stack = data;
	struct net_t *net = stack->net;
	struct net_routing_table_t *routing_table = net->routing_table;
	struct net_msg_t *msg = stack->msg;
	struct net_msg_frag_t *frag = stack->frag;

	struct net_node_t *src_node = frag->src_node;
	struct net_node_t *dst_node = frag->dst_node;

	struct net_node_t *node = frag->node;
	struct net_buffer_t *buffer = frag->buffer;

	/* Get current cycle */
	long long cycle;
	cycle = esim_domain_cycle(net_domain_index);

	if (event == EV_NET_SEND)
	{
		struct net_routing_table_entry_t *entry;
		struct net_buffer_t *output_buffer;

		/* Debug */
		net_debug("%lld: MSG -> "
				"a=\"send\" "
				"net=\"%s\" "
				"msg=%lld "
				"frag=%lld "
				"size=%d  "
				"src=\"%s "
				"dst=\"%s\"\n",
				cycle,
				net->name,
				msg->id,
				frag->id,
				frag->size,
				src_node->name,
				dst_node->name);

		/* Get output buffer */
		entry = net_routing_table_lookup(routing_table, src_node, dst_node);
		output_buffer = entry->output_buffer;
		if (!output_buffer)
			fatal("%s: no route from %s to %s.\n%s", net->name,
				src_node->name, dst_node->name,
				net_err_no_route);

		if (output_buffer->write_busy >= cycle)
			panic("%s: output buffer busy.\n%s", __FUNCTION__, net_err_can_send);

		/* Full msg must fit in buffers in both SAF and VCT */
		if (frag->parent->size > output_buffer->size)
			panic("%s: message does not fit in buffer.\n%s", __FUNCTION__, net_err_can_send);

		if (output_buffer->count + frag->size > output_buffer->size)
			panic("%s: output buffer full.\n%s", __FUNCTION__, net_err_can_send);

		/* Insert in output buffer (1 cycle latency) */
		net_buffer_insert(output_buffer, frag);
		output_buffer->write_busy = cycle;
		frag->node = src_node;
		frag->buffer = output_buffer;
		frag->busy = cycle;

		if (frag->id == 0)
			frag->parent->cycle_sent = cycle;

		/* Schedule next event for this fragment */
		esim_schedule_event(EV_NET_OUTPUT_BUFFER, stack, 1);

		/* Schedule the send event for the next fragment of this message */
		if (frag->id < list_count(msg->fragments)-1)
		{
			stack = net_stack_create(net, ESIM_EV_NONE, NULL);
			stack->msg = msg;
			stack->frag = list_get(msg->fragments, (frag->id) + 1);
			esim_schedule_event(EV_NET_SEND, stack, 1);
		}
	}

	else if (event == EV_NET_OUTPUT_BUFFER)
	{
		struct net_buffer_t *input_buffer;
		int lat;

		/* Debug */
		net_debug("%lld: MSG -> "
				"a=\"obuf\" "
				"net=\"%s\" "
				"msg=%lld "
				"frag=%lld "
				"node=\"%s\" "
				"buf=\"%s\"\n",
				cycle,
				net->name,
				msg->id,
				frag->id,
				node->name,
				buffer->name);

		/* If message is not at buffer head, process later */
		assert(list_count(buffer->frag_list));
		if (list_get(buffer->frag_list, 0) != frag)
		{
			net_buffer_wait(buffer, event, stack);
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"not output buffer head\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);
			return;
		}

		/* If source output buffer is busy, wait */
		if (buffer->read_busy >= cycle)
		{
			esim_schedule_event(event, stack, buffer->read_busy - cycle + 1);
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"output buffer busy\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);
			return;
		}

		if (buffer->kind == net_buffer_link)
		{
			struct net_link_t *link;

			assert(buffer->link);
			link = buffer->link;

			/* If link is busy, wait */
			if (link->busy >= cycle)
			{
				esim_schedule_event(event, stack, link->busy - cycle + 1);
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"link busy\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);
				return;
			}

			/* If buffer contains the message but doesn't have the
			 * shared link in control, wait */
			if (link->virtual_channel > 1)
			{
				struct net_buffer_t *temp_buffer;
				temp_buffer = net_link_arbitrator_vc(link, node);

				if (temp_buffer != buffer)
				{
					net_debug("%lld: MSG -> "
							"a=\"stall\" "
							"net=\"%s\" "
							"msg=%lld "
							"frag=%lld "
							"why=\"arbitrator sched\"\n",
							cycle,
							net->name,
							msg->id,
							frag->id);
					esim_schedule_event(event, stack, 1);
					return;
				}
			}

			/* If destination input buffer is busy, wait */
			assert(buffer == link->src_buffer);
			input_buffer = link->dst_buffer;
			if (input_buffer->write_busy >= cycle)
			{
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"input buffer busy\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);

				esim_schedule_event(event, stack, input_buffer->write_busy - cycle + 1);
				return;
			}

			/* If message doesn't fit in buffer, fatal */
			if (frag->parent->size > input_buffer->size)
				fatal("%s: message does not fit in buffer.\n%s", net->name, net_err_large_message);

			/* If destination input buffer is full, wait */
			if (input_buffer->count + frag->size > input_buffer->size)
			{
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"input buffer full\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);
				net_buffer_wait(input_buffer, event, stack);
				return;
			}

			/* Calculate latency and occupy resources */
			lat = (frag->size - 1) / link->bandwidth + 1;
			assert(lat > 0);
			buffer->read_busy = cycle + lat - 1;
			link->busy = cycle + lat - 1;
			input_buffer->write_busy = cycle + lat - 1;

			/* Transfer message to next input buffer */
			assert(frag->busy < cycle);
			net_buffer_extract(buffer, frag);
			net_buffer_insert(input_buffer, frag);
			frag->node = input_buffer->node;
			frag->buffer = input_buffer;
			frag->busy = cycle + lat - 1;

			/* Stats */
			link->busy_cycles += lat;
			link->transferred_bytes += frag->size;
			link->transferred_frags++;
			node->bytes_sent += frag->size;
			node->frags_sent++;
			input_buffer->node->bytes_received += frag->size;
			input_buffer->node->frags_received++;

			/* A complete msg has been processed */
			if (frag->id == list_count(msg->fragments) - 1)
			{
				link->transferred_msgs++;
				node->msgs_sent++;
				input_buffer->node->msgs_received++;
			}
		}
		else if (buffer->kind == net_buffer_bus)
		{
			struct net_bus_t *bus, *updated_bus;
			struct net_node_t *bus_node;

			assert(!buffer->link);
			assert(buffer->bus);
			bus = buffer->bus;
			bus_node = bus->node;

			/* before 1 and 2 we have to figure out what is the
			 * next input buffer since it is not clear from the
			 * output buffer */
			int input_buffer_detection = 0;
			struct net_routing_table_entry_t *entry;

			entry = net_routing_table_lookup(routing_table, frag->node, frag->dst_node);

			for (int i = 0; i < list_count(bus_node->dst_buffer_list); i++)
			{
				input_buffer = list_get(bus_node->dst_buffer_list, i);
				if (entry->next_node == input_buffer->node)
				{
					input_buffer_detection = 1;
					break;
				}
			}
			if (input_buffer_detection == 0)
				fatal("%s: Something went wrong so there is no appropriate input"
					"buffer for the route between %s and %s \n", net->name,
					frag->node->name,entry->next_node->name);

			/* 1. Check the destination buffer is busy or not */
			if (input_buffer->write_busy >= cycle)
			{
				esim_schedule_event(event, stack, input_buffer->write_busy - cycle + 1);
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"input busy\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);
				return;
			}

			/* 2. Check the destination buffer is full or not */
			/* If message doesn't fit in buffer, fatal */
			if (frag->parent->size > input_buffer->size)
				fatal("%s: message does not fit in buffer.\n%s", net->name, net_err_large_message);

			if (input_buffer->count + frag->size > input_buffer->size)
			{
				net_buffer_wait(input_buffer, event, stack);
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"input full\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);
				return;
			}

			/* 3. Make sure if any bus is available; return one
			 * that is available the fastest */
			updated_bus = net_bus_arbitration(bus_node, buffer);
			if (updated_bus == NULL)
			{
				esim_schedule_event(event, stack, 1);
				net_debug("%lld: MSG -> "
						"a=\"stall\" "
						"net=\"%s\" "
						"msg=%lld "
						"frag=%lld "
						"why=\"bus arbiter\"\n",
						cycle,
						net->name,
						msg->id,
						frag->id);
				return;
			}

			/* 4. assign the bus to the buffer. update the
			 * necessary data ; before here, the bus is not
			 * assign to anything and is not updated so it can be
			 * assign to other buffers as well. If this certain
			 * buffer wins that specific bus_lane the appropriate
			 * fields will be updated. Contains: bus_lane
			 * cin_buffer and cout_buffer and busy time as well as
			 * buffer data itself */
			assert(updated_bus);
			buffer->bus = updated_bus;
			input_buffer->bus = updated_bus;
			bus = buffer->bus;
			assert(bus);

			/* Calculate latency and occupy resources */
			lat = (frag->size - 1) / bus->bandwidth + 1;
			assert(lat > 0);
			buffer->read_busy = cycle + lat - 1;
			bus->busy = cycle + lat - 1;
			input_buffer->write_busy = cycle + lat - 1;

			/* Transfer message to next input buffer */
			assert(frag->busy < cycle);
			net_buffer_extract(buffer, frag);
			net_buffer_insert(input_buffer, frag);
			frag->node = input_buffer->node;
			frag->buffer = input_buffer;
			frag->busy = cycle + lat - 1;

			/* Stats */
			bus->busy_cycles += lat;
			bus->transferred_bytes += frag->size;
			bus->transferred_frags++;
			node->bytes_sent += frag->size;
			node->frags_sent++;
			input_buffer->node->bytes_received += frag->size;
			input_buffer->node->frags_received++;

			/* A complete msg has been processed*/
			if (frag->id == list_count(msg->fragments) - 1)
			{
				node->msgs_sent++;
				input_buffer->node->msgs_received++;
			}
		}

		/* Schedule next event */
		esim_schedule_event(EV_NET_INPUT_BUFFER, stack, lat);
	}

	else if (event == EV_NET_INPUT_BUFFER)
	{
		struct net_routing_table_entry_t *entry;
		struct net_buffer_t *output_buffer;
		struct net_msg_frag_t *frag_aux;

		int lat;

		frag_aux = list_get(buffer->frag_list, 0);

		/* Debug */
		net_debug("%lld: MSG -> "
				"a=\"ibuf\" "
				"net=\"%s\" "
				"msg=%lld "
				"frag=%lld "
				"node=\"%s\" "
				"buf=\"%s\"\n",
				cycle,
				net->name,
				msg->id,
				frag->id,
				node->name,
				buffer->name);

		/* If this is the destination node, finish */
		if (node == frag->dst_node)
		{
			esim_schedule_event(EV_NET_RECEIVE, stack, 0);
			return;
		}

		/* Test if we are at the head */
		assert(list_count(buffer->frag_list));
		if (frag_aux != frag)
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"not-head\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);
			net_buffer_wait(buffer, event, stack);
			return;
		}

		/* If source input buffer is busy, wait */
		if (buffer->read_busy >= cycle)
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"src-busy\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);

			esim_schedule_event(event, stack, buffer->read_busy - cycle + 1);
			return;
		}

		/* Get output buffer */
		entry = net_routing_table_lookup(routing_table, node, dst_node);
		output_buffer = entry->output_buffer;
		if (!output_buffer)
			fatal("%s: no route from %s to %s.\n%s", net->name, node->name, dst_node->name, net_err_no_route);

		/* If message doesn't fit in buffer, fatal */
		if (frag->parent->size > output_buffer->size)
			fatal("%s: message does not fit in buffer.\n%s", net->name, net_err_large_message);

		/* If destination output buffer is busy, wait */
		if (output_buffer->write_busy >= cycle)
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"dst-busy\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);

			esim_schedule_event(event, stack, output_buffer->write_busy - cycle + 1);
			return;
		}

		/* If destination output buffer is full, wait */
		if (output_buffer->count + frag->size > output_buffer->size)
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"dst-full\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);

			net_buffer_wait(output_buffer, event, stack);
			return;
		}

		/* If scheduler says that it is not our turn, try later */
		if (net_node_schedule(node, output_buffer) != buffer)
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"sched\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);

			esim_schedule_event(event, stack, 1);
			return;
		}

		/* Calculate latency and occupy resources */
		assert(node->kind != net_node_end);
		assert(node->bandwidth > 0);
		lat = (frag->size - 1) / node->bandwidth + 1;
		assert(lat > 0);
		buffer->read_busy = cycle + lat - 1;
		output_buffer->write_busy = cycle + lat - 1;

		/* Transfer message to next output buffer */
		assert(frag->busy < cycle);
		net_buffer_extract(buffer, frag);
		net_buffer_insert(output_buffer, frag);
		frag->buffer = output_buffer;
		frag->busy = cycle + lat - 1;

		/* Schedule next event */
		esim_schedule_event(EV_NET_OUTPUT_BUFFER, stack, lat);
	}

	else if (event == EV_NET_RECEIVE)
	{
		/* Debug */
		net_debug("%lld: MSG -> "
				"a=\"receive\" "
				"net=\"%s\" "
				"msg=%lld "
				"frag=%lld "
				"node=\"%s\"\n",
				cycle,
				net->name,
				msg->id,
				frag->id,
				dst_node->name);

		/* Receive fragment */
		net_receive_frag(net, node, frag);

		/* If all msg fragments have been received then return control to who inserted the message */
		if (frag->parent->arrived_frags_count == list_count(frag->parent->fragments))
		{
			/* Stats */
			net->transfers++;
			net->msg_size_acc += frag->parent->size;
			net->lat_acc += cycle - frag->parent->cycle_sent;

			net_debug("%lld: MSG -> "
					"a=\"finish\" "
					"net=\"%s\" "
					"msg=%lld "
					"lat=%lld "
					"node=\"%s\"\n",
					cycle,
					net->name,
					msg->id,
					cycle - frag->parent->cycle_sent,
					dst_node->name);

			/* Prepare return values */
			stack->ret_stack = frag->parent->ret_stack;
			stack->ret_event = frag->parent->ret_event;
		}

		else
		{
			net_debug("%lld: MSG -> "
					"a=\"stall\" "
					"net=\"%s\" "
					"msg=%lld "
					"frag=%lld "
					"why=\"arrived but fragments pending\"\n",
					cycle,
					net->name,
					msg->id,
					frag->id);
		}

		/* This frees the stack always and if all the fragments have been
		 * received returns control to whom inserted the message in the network */
		net_stack_return(stack);
	}

	else
	{
		panic("%s: unknown event", __FUNCTION__);
	}
}
