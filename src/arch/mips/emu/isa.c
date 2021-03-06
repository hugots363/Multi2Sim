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

#include <arch/common/arch.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/misc.h>
#include <mem-system/memory.h>

#include "context.h"
#include "isa.h"
#include "machine.h"
#include "regs.h"
#include "syscall.h"


/* Debug categories */
int mips_isa_call_debug_category;
int mips_isa_inst_debug_category;

/*
 * Instruction statistics
 */
static long long mips_inst_freq[MIPS_INST_COUNT];

/* Table including references to functions in machine.c
 * that implement machine instructions. */
/* Instruction execution table */
static mips_isa_inst_func_t mips_isa_inst_func[MIPS_INST_COUNT] =
{
	NULL /* for op_none */
#define DEFINST(_name, _fmt_str, _op0, _op1, _op2, _op3) , mips_isa_##_name##_impl
#include <arch/mips/asm/asm.dat>
#undef DEFINST
};

/* FIXME - merge with ctx_execute */
void mips_isa_execute_inst(struct mips_ctx_t *ctx)
{
//	struct mips_regs_t *regs = ctx->regs;
	ctx->next_ip = ctx->n_next_ip;
	ctx->n_next_ip += 4;

	/* Debug */
	if (debug_status(mips_isa_inst_debug_category))
	{
		mips_isa_inst_debug("%d %8lld %x: ", ctx->pid,
			arch_mips->inst_count, ctx->regs->pc);
		mips_inst_debug_dump(&ctx->inst, debug_file(mips_isa_inst_debug_category));
	}

	/* Call instruction emulation function */
//	regs->pc = regs->pc + ctx->inst.info->size;
	if (ctx->inst.info->opcode)
		mips_isa_inst_func[ctx->inst.info->opcode](ctx);
	/* Statistics */
	mips_inst_freq[ctx->inst.info->opcode]++;

	/* Debug */
	mips_isa_inst_debug("\n");
//	if (debug_status(mips_isa_call_debug_category))
//		mips_isa_debug_call(ctx);
}
