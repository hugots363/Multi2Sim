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

#ifndef NETWORK_MESSAGE_H
#define NETWORK_MESSAGE_H

#include <stdbool.h>

struct net_msg_t
{
	long long id;
	int size;

	/* List of fragments this message is composed of */
	struct list_t *fragments;

	/* Linked list for bucket chain in net->msg_table */
	struct net_msg_t *bucket_next;

	/* Return values */
	struct net_stack_t *ret_stack;
	int ret_event;

	/* Number of fragments that have arrived to the destination node */
	int arrived_frags_count;

	/* Cycle in which this message has entered the network */
	long long cycle_sent;
};

struct net_msg_frag_t
{
	long long id;
	int fragment;/* Fragment index in a VCT stream */
	int size;

	struct net_t *net;
	struct net_node_t *src_node;
	struct net_node_t *dst_node;

	struct net_msg_t *parent; /* Every fragment is part of a msg */

	long long cycle_sent; /* Cycle when it was sent */
	long long busy;       /* In transit until cycle */
	void *data;

	/* Message can't be extracted from the end node's input buffer until all the
	* fragments it is bomposed of have arrived */
	bool arrived;

	/* Current position in network */
	struct net_node_t *node;
	struct net_buffer_t *buffer;

	/* Information for in-transit messages. Messages always travel from
	 * buffers to buffers (or end-nodes) */
	struct net_buffer_t *src_buffer;	/* Original source buffer */
	struct net_buffer_t *dst_buffer;	/* Final destination buffer */
};


struct net_msg_t *net_msg_create(struct net_t *net,
	struct net_node_t *src_node, struct net_node_t *dst_node, int size);
void net_msg_free(struct net_msg_t *msg);

struct net_msg_frag_t *net_msg_frag_create(struct net_t *net, struct net_node_t *src_node, struct net_node_t *dst_node, int size);
void net_msg_frag_free(struct net_msg_frag_t *frag);

void net_msg_table_insert(struct net_t *net, struct net_msg_t *msg);
struct net_msg_t *net_msg_table_extract(struct net_t *net, long long id);
struct net_msg_t *net_msg_table_get(struct net_t *net, long long id);

#endif
