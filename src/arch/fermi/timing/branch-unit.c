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

#include <arch/common/arch.h>
#include <arch/fermi/emu/emu.h>
#include <arch/fermi/emu/warp.h>
#include <lib/esim/esim.h>
#include <lib/esim/trace.h>
#include <lib/util/list.h>

#include "branch-unit.h"
#include "sm.h"
#include "gpu.h"
#include "uop.h"
#include "warp-inst-queue.h"

void frm_branch_unit_complete(struct frm_branch_unit_t *branch_unit)
{
	struct frm_uop_t *uop;
	int list_entries;
	int list_index = 0;
	int i;

	list_entries = list_count(branch_unit->write_buffer);

	/* Sanity check the write buffer */
	assert(list_entries <= frm_gpu_branch_unit_write_latency * 
		frm_gpu_branch_unit_width);

	for (i = 0; i < list_entries; i++)
	{
		uop = list_get(branch_unit->write_buffer, list_index);
		assert(uop);

		if (arch_fermi->cycle < uop->write_ready)
		{
			list_index++;
			continue;
		}

		/* Access complete, remove the uop from the queue */
		list_remove(branch_unit->write_buffer, uop);

		frm_trace("si.end_inst id=%lld cu=%d\n", uop->id_in_sm,
			uop->sm->id);

		/* Allow next instruction to be fetched */
		uop->warp_inst_queue_entry->ready = 1;

		/* Free uop */
		frm_uop_free(uop);

		/* Statistics */
		branch_unit->inst_count++;
		frm_gpu->last_complete_cycle = arch_fermi->cycle;
	}
}

void frm_branch_unit_write(struct frm_branch_unit_t *branch_unit)
{
	struct frm_uop_t *uop;
	int instructions_processed = 0;
	int list_entries;
	int list_index = 0;
	int i;

	list_entries = list_count(branch_unit->exec_buffer);

	/* Sanity check the exec buffer */
	assert(list_entries <= frm_gpu_branch_unit_exec_buffer_size);

	for (i = 0; i < list_entries; i++)
	{
		uop = list_get(branch_unit->exec_buffer, list_index);
		assert(uop);

		instructions_processed++;

		/* Uop not ready yet */
		if (arch_fermi->cycle < uop->execute_ready)
		{
			list_index++;
			continue;
		}

		/* Stall if the width has been reached. */
		if (instructions_processed > frm_gpu_branch_unit_width)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		/* Sanity check the write buffer */
		assert(list_count(branch_unit->write_buffer) <= 
			frm_gpu_branch_unit_write_buffer_size);

		/* Stall if the write buffer is full. */
		if (list_count(branch_unit->write_buffer) == 
			frm_gpu_branch_unit_write_buffer_size)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		uop->write_ready = arch_fermi->cycle + 
			frm_gpu_branch_unit_write_latency;
		list_remove(branch_unit->exec_buffer, uop);
		list_enqueue(branch_unit->write_buffer, uop);

		frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
			"stg=\"bu-w\"\n", uop->id_in_sm, 
			branch_unit->sm->id, uop->warp->id, 
			uop->id_in_warp);
	}
}

void frm_branch_unit_execute(struct frm_branch_unit_t *branch_unit)
{
	struct frm_uop_t *uop;
	int list_entries;
	int instructions_processed = 0;
	int list_index = 0;
	int i;

	list_entries = list_count(branch_unit->read_buffer);

	/* Sanity check the read buffer */
	assert(list_entries <= frm_gpu_branch_unit_read_buffer_size);

	for (i = 0; i < list_entries; i++)
	{
		uop = list_get(branch_unit->read_buffer, list_index);
		assert(uop);

		instructions_processed++;

		/* Uop is not ready yet */
		if (arch_fermi->cycle < uop->read_ready)
		{
			list_index++;
			continue;
		}

		/* Stall if the issue width has been reached. */
		if (instructions_processed > frm_gpu_branch_unit_width)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		/* Sanity check the exec buffer */
		assert(list_count(branch_unit->exec_buffer) <= 
				frm_gpu_branch_unit_exec_buffer_size);

		/* Stall if the exec buffer is full. */
		if (list_count(branch_unit->exec_buffer) == 
				frm_gpu_branch_unit_exec_buffer_size)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		/* Branch */
		uop->execute_ready = arch_fermi->cycle + 
			frm_gpu_branch_unit_exec_latency;

		/* Transfer the uop to the outstanding execution buffer */
		list_remove(branch_unit->read_buffer, uop);
		list_enqueue(branch_unit->exec_buffer, uop);

		frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
			"stg=\"bu-e\"\n", uop->id_in_sm, 
			branch_unit->sm->id, uop->warp->id, 
			uop->id_in_warp);
	}
}

void frm_branch_unit_read(struct frm_branch_unit_t *branch_unit)
{
	struct frm_uop_t *uop;
	int instructions_processed = 0;
	int list_entries;
	int list_index = 0;
	int i;

	list_entries = list_count(branch_unit->decode_buffer);

	/* Sanity check the issue buffer */
	assert(list_entries <= frm_gpu_branch_unit_decode_buffer_size);

	for (i = 0; i < list_entries; i++)
	{
		uop = list_get(branch_unit->decode_buffer, list_index);
		assert(uop);

		instructions_processed++;

		/* Uop not ready yet */
		if (arch_fermi->cycle < uop->decode_ready)
		{
			list_index++;
			continue;
		}

		/* Stall if the issue width has been reached. */
		if (instructions_processed > frm_gpu_branch_unit_width)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		/* Sanity check the read buffer */
		assert(list_count(branch_unit->read_buffer) <= 
				frm_gpu_branch_unit_read_buffer_size);

		/* Stall if the read buffer is full. */
		if (list_count(branch_unit->read_buffer) == 
			frm_gpu_branch_unit_read_buffer_size)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		uop->read_ready = arch_fermi->cycle + 
			frm_gpu_branch_unit_read_latency;

		list_remove(branch_unit->decode_buffer, uop);
		list_enqueue(branch_unit->read_buffer, uop);

		frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
			"stg=\"bu-r\"\n", uop->id_in_sm, 
			branch_unit->sm->id, uop->warp->id, 
			uop->id_in_warp);
	}
}

void frm_branch_unit_decode(struct frm_branch_unit_t *branch_unit)
{
	struct frm_uop_t *uop;
	int instructions_processed = 0;
	int list_entries;
	int list_index = 0;
	int i;

	list_entries = list_count(branch_unit->issue_buffer);

	/* Sanity check the issue buffer */
	assert(list_entries <= frm_gpu_branch_unit_issue_buffer_size);

	for (i = 0; i < list_entries; i++)
	{
		uop = list_get(branch_unit->issue_buffer, list_index);
		assert(uop);

		instructions_processed++;

		/* Uop not ready yet */
		if (arch_fermi->cycle < uop->issue_ready)
		{
			list_index++;
			continue;
		}

		/* Stall if the issue width has been reached. */
		if (instructions_processed > frm_gpu_branch_unit_width)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		/* Sanity check the decode buffer */
		assert(list_count(branch_unit->decode_buffer) <= 
			frm_gpu_branch_unit_decode_buffer_size);

		/* Stall if the decode buffer is full. */
		if (list_count(branch_unit->decode_buffer) == 
			frm_gpu_branch_unit_decode_buffer_size)
		{
			frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
				"stg=\"s\"\n", uop->id_in_sm, 
				branch_unit->sm->id, 
				uop->warp->id, uop->id_in_warp);
			list_index++;
			continue;
		}

		uop->decode_ready = arch_fermi->cycle + 
			frm_gpu_branch_unit_decode_latency;

		list_remove(branch_unit->issue_buffer, uop);
		list_enqueue(branch_unit->decode_buffer, uop);

		frm_trace("si.inst id=%lld cu=%d wf=%d uop_id=%lld "
			"stg=\"bu-d\"\n", uop->id_in_sm, 
			branch_unit->sm->id, uop->warp->id, 
			uop->id_in_warp);
	}
}

void frm_branch_unit_run(struct frm_branch_unit_t *branch_unit)
{
	frm_branch_unit_complete(branch_unit);
	frm_branch_unit_write(branch_unit);
	frm_branch_unit_execute(branch_unit);
	frm_branch_unit_read(branch_unit);
	frm_branch_unit_decode(branch_unit);
}
