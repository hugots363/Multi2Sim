/*
 *  Multi2Sim
 *  Copyright (C) 2013  Rafael Ubal (ubal@ece.neu.edu)
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

#include <arch/southern-islands/asm/arg.h>
#include <arch/southern-islands/asm/bin-file.h>
#include <m2c/si2bin/arg.h>
#include <m2c/si2bin/inst.h>
#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/linked-list.h>
#include <lib/util/list.h>
#include <lib/util/string.h>
#include <llvm-c/Core.h>

#include "basic-block.h"
#include "function.h"
#include "symbol.h"
#include "symbol-table.h"


/*
 * Function Argument Object
 */

/* Return a Southern Islands argument type from an LLVM type. */
static enum si_arg_data_type_t llvm2si_function_arg_get_data_type(LLVMTypeRef lltype)
{
	LLVMTypeKind lltype_kind;
	int bit_width;

	lltype_kind = LLVMGetTypeKind(lltype);
	if (lltype_kind == LLVMIntegerTypeKind)
	{
		bit_width = LLVMGetIntTypeWidth(lltype);
		switch (bit_width)
		{
		case 1: return si_arg_i1;
		case 8: return si_arg_i8;
		case 16: return si_arg_i16;
		case 32: return si_arg_i32;
		case 64: return si_arg_i64;

		default:
			fatal("%s: invalid argument bit width (%d)",
				__FUNCTION__, bit_width);
			return si_arg_data_type_invalid;
		}
	}
	else
	{
		fatal("%s: unsupported argument type kind (%d)",
				__FUNCTION__, lltype_kind);
		return si_arg_data_type_invalid;
	}
}


struct llvm2si_function_arg_t *llvm2si_function_arg_create(LLVMValueRef llarg)
{
	struct llvm2si_function_arg_t *arg;
	struct si_arg_t *si_arg;

	LLVMTypeRef lltype;
	LLVMTypeKind lltype_kind;

	char *name;

	/* Get argument name */
	name = (char *) LLVMGetValueName(llarg);
	if (!*name)
		fatal("%s: anonymous arguments not allowed", __FUNCTION__);

	/* Initialize 'si_arg' object */
	lltype = LLVMTypeOf(llarg);
	lltype_kind = LLVMGetTypeKind(lltype);
	if (lltype_kind == LLVMPointerTypeKind)
	{
		lltype = LLVMGetElementType(lltype);
		si_arg = si_arg_create(si_arg_pointer, name);
		si_arg->pointer.scope = si_arg_uav;
		si_arg->pointer.data_type = llvm2si_function_arg_get_data_type(lltype);
	}
	else
	{
		si_arg = si_arg_create(si_arg_value, name);
		si_arg->value.data_type = llvm2si_function_arg_get_data_type(lltype);
	}

	/* Initialize 'arg' object */
	arg = xcalloc(1, sizeof(struct llvm2si_function_arg_t));
	arg->name = xstrdup(name);
	arg->llarg = llarg;
	arg->si_arg = si_arg;

	/* Return */
	return arg;
}


void llvm2si_function_arg_free(struct llvm2si_function_arg_t *arg)
{
	assert(arg->name);
	assert(arg->si_arg);
	str_free(arg->name);
	si_arg_free(arg->si_arg);
	free(arg);
}


void llvm2si_function_arg_dump(struct llvm2si_function_arg_t *arg, FILE *f)
{
	struct si_arg_t *si_arg = arg->si_arg;

	switch (si_arg->type)
	{
	case si_arg_pointer:

		switch (si_arg->pointer.scope)
		{

		case si_arg_uav:
			
			/* Type, name, offset, UAV */
			fprintf(f, "\t%s* %s %d uav%d\n",
				str_map_value(&si_arg_data_type_map, si_arg->pointer.data_type),
				si_arg->name, arg->index * 16, arg->uav_index + 10);
			break;

		default:

			fatal("%s: pointer scope not supported (%d)",
				__FUNCTION__, si_arg->pointer.scope);
		}

		break;

	default:
		fatal("%s: argument type not recognized (%d)",
				__FUNCTION__, si_arg->type);
	}
}




/*
 * Function UAV Object
 */

struct llvm2si_function_uav_t *llvm2si_function_uav_create(void)
{
	struct llvm2si_function_uav_t *uav;

	/* Initialize */
	uav = xcalloc(1, sizeof(struct llvm2si_function_uav_t));

	/* Return */
	return uav;
}


void llvm2si_function_uav_free(struct llvm2si_function_uav_t *uav)
{
	free(uav);
}




/*
 * Function Object
 */

/* Add a UAV to the UAV list. This function allocates a series of 4 aligned
 * scalar registers to the UAV, populating its 'index' and 'sreg' fields.
 * The UAV object will be freed automatically after calling this function.
 * Emit the code needed to load UAV into 'function->basic_block_uavs' */
static void llvm2si_function_add_uav(struct llvm2si_function_t *function,
		struct llvm2si_function_uav_t *uav)
{
	struct list_t *arg_list;
	struct si2bin_inst_t *inst;
	struct llvm2si_basic_block_t *basic_block;

	/* Associate UAV with function */
	assert(!uav->function);
	uav->function = function;

	/* Obtain basic block */
	basic_block = function->basic_block_uavs;

	/* Allocate 4 aligned scalar registers */
	uav->sreg = llvm2si_function_alloc_sreg(function, 4, 4);

	/* Insert to UAV list */
	uav->index = function->uav_list->count;
	list_add(function->uav_list, uav);

	/* Emit code to load UAV.
	 * s_load_dwordx4 s[uavX:uavX+3], s[uav_table:uav_table+1], x * 8
	 */
	arg_list = list_create();
	list_add(arg_list, si2bin_arg_create_scalar_register_series(
			uav->sreg, uav->sreg + 3));
	list_add(arg_list, si2bin_arg_create_scalar_register_series(
			function->sreg_uav_table, function->sreg_uav_table + 1));
	list_add(arg_list, si2bin_arg_create_literal((uav->index + 10) * 8));
	inst = si2bin_inst_create(SI_INST_S_LOAD_DWORDX4, arg_list);
	llvm2si_basic_block_add_inst(basic_block, inst);
}


/* Add argument 'arg' into the list of arguments of 'function', and emit code
 * to load it into 'function->basic_block_args'. */
static void llvm2si_function_add_arg(struct llvm2si_function_t *function,
		struct llvm2si_function_arg_t *arg)
{
	struct list_t *arg_list;
	struct si2bin_inst_t *inst;
	struct llvm2si_symbol_t *symbol;
	struct llvm2si_basic_block_t *basic_block;
	struct llvm2si_function_uav_t *uav;

	/* Check that argument does not belong to a function yet */
	if (arg->function)
		panic("%s: argument already added", __FUNCTION__);

	/* Select basic block */
	basic_block = function->basic_block_args;

	/* Add argument */
	list_add(function->arg_list, arg);
	arg->function = function;
	arg->index = function->arg_list->count - 1;

	/* Allocate 1 scalar and 1 vector register for the argument */
	arg->sreg = llvm2si_function_alloc_sreg(function, 1, 1);
	arg->vreg = llvm2si_function_alloc_vreg(function, 1, 1);

	/* Generate code to load argument into a scalar register.
	 * s_buffer_load_dword s[arg], s[cb1:cb1+3], idx*4
	 */
	arg_list = list_create();
	list_add(arg_list, si2bin_arg_create_scalar_register(arg->sreg));
	list_add(arg_list, si2bin_arg_create_scalar_register_series(function->sreg_cb1,
			function->sreg_cb1 + 3));
	list_add(arg_list, si2bin_arg_create_literal(arg->index * 4));
	inst = si2bin_inst_create(SI_INST_S_BUFFER_LOAD_DWORD, arg_list);
	llvm2si_basic_block_add_inst(basic_block, inst);

	/* Copy argument into a vector register. This vector register will be
	 * used for convenience during code emission, so that we don't have to
	 * worry at this point about different operand type encodings for
	 * instructions. Optimization passes will get rid later of redundant
	 * copies and scalar opportunities.
	 * v_mov_b32 v[arg], s[arg]
	 */
	arg_list = list_create();
	list_add(arg_list, si2bin_arg_create_vector_register(arg->vreg));
	list_add(arg_list, si2bin_arg_create_scalar_register(arg->sreg));
	inst = si2bin_inst_create(SI_INST_V_MOV_B32, arg_list);
	llvm2si_basic_block_add_inst(basic_block, inst);

	/* Insert argument name in symbol table, using its vector register. */
	symbol = llvm2si_symbol_create(arg->name,
		llvm2si_symbol_vector_register, arg->vreg);
	llvm2si_symbol_table_add_symbol(function->symbol_table, symbol);

	/* If argument is an object in global memory, create a UAV
	 * associated with it. */
	if (arg->si_arg->type == si_arg_pointer &&
			arg->si_arg->pointer.scope == si_arg_uav)
	{
		/* New UAV */
		uav = llvm2si_function_uav_create();
		llvm2si_function_add_uav(function, uav);

		/* Store UAV index in argument and symbol */
		llvm2si_symbol_set_uav_index(symbol, uav->index);
		arg->uav_index = uav->index;
	}
}


static void llvm2si_function_dump_data(struct llvm2si_function_t *function,
		FILE *f)
{
	/* Section header */
	fprintf(f, ".data\n");

	/* User elements */
	fprintf(f, "\tuserElements[0] = PTR_UAV_TABLE, 0, s[%d:%d]\n",
			function->sreg_uav_table, function->sreg_uav_table + 1);
	fprintf(f, "\tuserElements[1] = IMM_CONST_BUFFER, 0, s[%d:%d]\n",
			function->sreg_cb0, function->sreg_cb0 + 3);
	fprintf(f, "\tuserElements[2] = IMM_CONST_BUFFER, 1, s[%d:%d]\n",
			function->sreg_cb1, function->sreg_cb1 + 3);
	fprintf(f, "\n");

	/* Floating-point mode */
	fprintf(f, "\tFloatMode = 192\n");
	fprintf(f, "\tIeeeMode = 0\n");
	fprintf(f, "\n");

	/* Program resources */
	fprintf(f, "\tCOMPUTE_PGM_RSRC2:USER_SGPR = %d\n", function->sreg_wgid);
	fprintf(f, "\tCOMPUTE_PGM_RSRC2:TGID_X_EN = %d\n", 1);
	fprintf(f, "\tCOMPUTE_PGM_RSRC2:TGID_Y_EN = %d\n", 1);
	fprintf(f, "\tCOMPUTE_PGM_RSRC2:TGID_Z_EN = %d\n", 1);
	fprintf(f, "\n");
}


struct llvm2si_function_t *llvm2si_function_create(LLVMValueRef llfunction)
{
	struct llvm2si_function_t *function;

	/* Allocate */
	function = xcalloc(1, sizeof(struct llvm2si_function_t));
	function->llfunction = llfunction;
	function->name = xstrdup(LLVMGetValueName(llfunction));
	function->basic_block_list = linked_list_create();
	function->arg_list = list_create();
	function->uav_list = list_create();
	function->symbol_table = llvm2si_symbol_table_create();

	/* Standard basic blocks */
	function->basic_block_header = llvm2si_basic_block_create(NULL);
	function->basic_block_uavs = llvm2si_basic_block_create(NULL);
	function->basic_block_args = llvm2si_basic_block_create(NULL);
	llvm2si_function_add_basic_block(function, function->basic_block_header);
	llvm2si_function_add_basic_block(function, function->basic_block_uavs);
	llvm2si_function_add_basic_block(function, function->basic_block_args);

	/* Comments in basic blocks */
	llvm2si_basic_block_add_comment(function->basic_block_uavs,
			"Obtain UAV descriptors");
	llvm2si_basic_block_add_comment(function->basic_block_args,
			"Read kernel arguments from cb1");

	/* Return */
	return function;
}


void llvm2si_function_free(struct llvm2si_function_t *function)
{
	int index;

	/* Free list of basic blocks */
	LINKED_LIST_FOR_EACH(function->basic_block_list)
		llvm2si_basic_block_free(linked_list_get(function->basic_block_list));
	linked_list_free(function->basic_block_list);

	/* Free list of arguments */
	LIST_FOR_EACH(function->arg_list, index)
		llvm2si_function_arg_free(list_get(function->arg_list, index));
	list_free(function->arg_list);

	/* Free list of UAVs */
	LIST_FOR_EACH(function->uav_list, index)
		llvm2si_function_uav_free(list_get(function->uav_list, index));
	list_free(function->uav_list);

	/* Rest */
	llvm2si_symbol_table_free(function->symbol_table);
	free(function->name);
	free(function);
}


void llvm2si_function_dump(struct llvm2si_function_t *function, FILE *f)
{
	struct llvm2si_basic_block_t *basic_block;
	struct llvm2si_function_arg_t *function_arg;

	int index;

	/* Function name */
	fprintf(f, ".global %s\n\n", function->name);

	/* Arguments */
	fprintf(f, ".args\n");
	LIST_FOR_EACH(function->arg_list, index)
	{
		function_arg = list_get(function->arg_list, index);
		llvm2si_function_arg_dump(function_arg, f);
	}
	fprintf(f, "\n");

	/* Dump basic blocks */
	fprintf(f, ".text\n");
	LINKED_LIST_FOR_EACH(function->basic_block_list)
	{
		basic_block = linked_list_get(function->basic_block_list);
		llvm2si_basic_block_dump(basic_block, f);
	}
	fprintf(f, "\n");

	/* Dump section '.data' */
	llvm2si_function_dump_data(function, f);
	fprintf(f, "\n");
}


void llvm2si_function_add_basic_block(struct llvm2si_function_t *function,
		struct llvm2si_basic_block_t *basic_block)
{
	/* Check that basic block does not belong to any other function. */
	if (basic_block->function)
		panic("%s: basic block '%s' already added to a function",
				__FUNCTION__, basic_block->name);

	/* Add basic block */
	linked_list_add(function->basic_block_list, basic_block);
	basic_block->function = function;
}


void llvm2si_function_emit_header(struct llvm2si_function_t *function)
{
	struct llvm2si_basic_block_t *basic_block;
	struct si2bin_inst_t *inst;
	struct list_t *arg_list;

	char comment[MAX_STRING_SIZE];
	int index;

	/* Select basic block */
	basic_block = function->basic_block_header;

	/* Function must be empty at this point */
	assert(!function->num_sregs);
	assert(!function->num_vregs);

	/* Allocate 3 vector registers (v[0:2]) for local ID */
	function->vreg_lid = llvm2si_function_alloc_vreg(function, 3, 1);
	if (function->vreg_lid)
		panic("%s: vreg_lid is expented to be 0", __FUNCTION__);

	/* Allocate 2 scalar registers for UAV table. The value for these
	 * registers is assigned by the runtime based on info found in the
	 * 'userElements' metadata of the binary.*/
	function->sreg_uav_table = llvm2si_function_alloc_sreg(function, 2, 1);

	/* Allocate 4 scalar registers for CB0, and 4 more for CB1. The
	 * values for these registers will be assigned by the runtime based
	 * on info present in the 'userElements' metadata. */
	function->sreg_cb0 = llvm2si_function_alloc_sreg(function, 4, 1);
	function->sreg_cb1 = llvm2si_function_alloc_sreg(function, 4, 1);

	/* Allocate 3 scalar registers for the work-group ID. The content of
	 * these register will be populated by the runtime based on info found
	 * in COMPUTE_PGM_RSRC2 metadata. */
	function->sreg_wgid = llvm2si_function_alloc_sreg(function, 3, 1);

	/* Obtain global size in s[gsize:gsize+2].
	 * s_buffer_load_dword s[gsize], s[cb0:cb0+3], 0x00
	 * s_buffer_load_dword s[gsize+1], s[cb0:cb0+3], 0x01
	 * s_buffer_load_dword s[gsize+2], s[cb0:cb0+3], 0x02
	 */
	llvm2si_basic_block_add_comment(basic_block, "Obtain global size");
	function->sreg_gsize = llvm2si_function_alloc_sreg(function, 3, 1);
	for (index = 0; index < 3; index++)
	{
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_gsize + index));
		list_add(arg_list, si2bin_arg_create_scalar_register_series(
				function->sreg_cb0, function->sreg_cb0 + 3));
		list_add(arg_list, si2bin_arg_create_literal(index));
		inst = si2bin_inst_create(SI_INST_S_BUFFER_LOAD_DWORD, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);
	}

	/* Obtain local size in s[lsize:lsize+2].
	 *
	 * s_buffer_load_dword s[lsize], s[cb0:cb0+3], 0x04
	 * s_buffer_load_dword s[lsize+1], s[cb0:cb0+3], 0x05
	 * s_buffer_load_dword s[lsize+2], s[cb0:cb0+3], 0x06
	 */
	llvm2si_basic_block_add_comment(basic_block, "Obtain local size");
	function->sreg_lsize = llvm2si_function_alloc_sreg(function, 3, 1);
	for (index = 0; index < 3; index++)
	{
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_lsize + index));
		list_add(arg_list, si2bin_arg_create_scalar_register_series(
				function->sreg_cb0, function->sreg_cb0 + 3));
		list_add(arg_list, si2bin_arg_create_literal(4 + index));
		inst = si2bin_inst_create(SI_INST_S_BUFFER_LOAD_DWORD, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);
	}

	/* Obtain global offset in s[offs:offs+2].
	 *
	 * s_buffer_load_dword s[offs], s[cb0:cb0+3], 0x18
	 * s_buffer_load_dword s[offs], s[cb0:cb0+3], 0x19
	 * s_buffer_load_dword s[offs], s[cb0:cb0+3], 0x1a
	 */
	llvm2si_basic_block_add_comment(basic_block, "Obtain global offset");
	function->sreg_offs = llvm2si_function_alloc_sreg(function, 3, 1);
	for (index = 0; index < 3; index++)
	{
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_offs + index));
		list_add(arg_list, si2bin_arg_create_scalar_register_series(
				function->sreg_cb0, function->sreg_cb0 + 3));
		list_add(arg_list, si2bin_arg_create_literal(0x18 + index));
		inst = si2bin_inst_create(SI_INST_S_BUFFER_LOAD_DWORD, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);
	}

	/* Calculate global ID in dimensions [0:2] and store it in v[3:5].
	 *
	 * v_mov_b32 v[gid+dim], s[lsize+dim]
	 * v_mul_i32_i24 v[gid+dim], s[wgid+dim], v[gid+dim]
	 * v_add_i32 v[gid+dim], vcc, v[gid+dim], v[lid+dim]
	 * v_add_i32 v[gid+dim], vcc, v[gid+dim], s[offs+dim]
	 */
	function->vreg_gid = llvm2si_function_alloc_vreg(function, 3, 1);
	for (index = 0; index < 3; index++)
	{
		/* Comment */
		snprintf(comment, sizeof comment, "Calculate global ID "
				"in dimension %d", index);
		llvm2si_basic_block_add_comment(basic_block, comment);

		/* v_mov_b32 */
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_lsize + index));
		inst = si2bin_inst_create(SI_INST_V_MOV_B32, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);

		/* v_mul_i32_i24 */
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_wgid + index));
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		inst = si2bin_inst_create(SI_INST_V_MUL_I32_I24, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);

		/* v_add_i32 */
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		list_add(arg_list, si2bin_arg_create_special_register(si_inst_special_reg_vcc));
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_lid + index));
		inst = si2bin_inst_create(SI_INST_V_ADD_I32, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);

		/* v_add_i32 */
		arg_list = list_create();
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		list_add(arg_list, si2bin_arg_create_special_register(si_inst_special_reg_vcc));
		list_add(arg_list, si2bin_arg_create_scalar_register(
				function->sreg_offs + index));
		list_add(arg_list, si2bin_arg_create_vector_register(
				function->vreg_gid + index));
		inst = si2bin_inst_create(SI_INST_V_ADD_I32, arg_list);
		llvm2si_basic_block_add_inst(basic_block, inst);
	}
}


void llvm2si_function_emit_args(struct llvm2si_function_t *function)
{
	LLVMValueRef llfunction;
	LLVMValueRef llarg;

	struct llvm2si_function_arg_t *arg;

	/* Emit code for each argument individually */
	llfunction = function->llfunction;
	for (llarg = LLVMGetFirstParam(llfunction); llarg;
			llarg = LLVMGetNextParam(llarg))
	{
		/* Create function argument and add it */
		arg = llvm2si_function_arg_create(llarg);

		/* Add the argument to the list. This call will cause the
		 * corresponding code to be emitted. */
		llvm2si_function_add_arg(function, arg);
	}
}


void llvm2si_function_emit_body(struct llvm2si_function_t *function,
		struct llvm2si_basic_block_t *basic_block)
{
	LLVMValueRef llfunction;
	LLVMBasicBlockRef llbb;

	/* Iterate through the function basic blocks */
	llfunction = function->llfunction;
	for (llbb = LLVMGetFirstBasicBlock(llfunction); llbb;
			llbb = LLVMGetNextBasicBlock(llbb))
	{
		/* Create an SI basic block and add it to the function */
		basic_block = llvm2si_basic_block_create(llbb);
		llvm2si_function_add_basic_block(function, basic_block);

		/* Emit code for the basic block */
		llvm2si_basic_block_emit(basic_block);
	}
}


static struct si2bin_arg_t *llvm2si_function_translate_const_value(
		struct llvm2si_function_t *function,
		LLVMValueRef llvalue)
{
	LLVMTypeRef lltype;
	LLVMTypeKind lltype_kind;

	lltype = LLVMTypeOf(llvalue);
	lltype_kind = LLVMGetTypeKind(lltype);
	
	/* Check constant type */
	switch (lltype_kind)
	{
	
	case LLVMIntegerTypeKind:
	{
		int bit_width;
		int value;

		/* Only 32-bit constants supported for now. We need to figure
		 * out what to do with the sign extension otherwise. */
		bit_width = LLVMGetIntTypeWidth(lltype);
		if (bit_width != 32)
			fatal("%s: only 32-bit integer constant supported "
				" (%d-bit found)", __FUNCTION__, bit_width);

		/* Create argument */
		value = LLVMConstIntGetZExtValue(llvalue);
		return si2bin_arg_create_literal(value);
	}

	default:
		
		fatal("%s: constant type not supported (%d)",
			__FUNCTION__, lltype_kind);
		return NULL;
	}
}


struct si2bin_arg_t *llvm2si_function_translate_value(
		struct llvm2si_function_t *function,
		LLVMValueRef llvalue,
		struct llvm2si_symbol_t **symbol_ptr)
{
	struct llvm2si_symbol_t *symbol;
	struct si2bin_arg_t *arg;

	char *name;

	/* Returned symbol is NULL by default */
	PTR_ASSIGN(symbol_ptr, NULL);

	/* Treat constants separately */
	if (LLVMIsConstant(llvalue))
		return llvm2si_function_translate_const_value(function, llvalue);

	/* Get name */
	name = (char *) LLVMGetValueName(llvalue);
	if (!name || !*name)
		fatal("%s: anonymous values not supported", __FUNCTION__);

	/* Look up symbol */
	symbol = llvm2si_symbol_table_lookup(function->symbol_table, name);
	if (!symbol)
		fatal("%s: %s: symbol not found", __FUNCTION__, name);

	/* Create argument based on symbol type */
	switch (symbol->type)
	{

	case llvm2si_symbol_vector_register:

		arg = si2bin_arg_create_vector_register(symbol->reg);
		break;

	case llvm2si_symbol_scalar_register:

		arg = si2bin_arg_create_scalar_register(symbol->reg);
		break;

	default:

		arg = NULL;
		fatal("%s: invalid symbol type (%d)", __FUNCTION__,
				symbol->type);
	}
	
	/* Return argument and symbol */
	PTR_ASSIGN(symbol_ptr, symbol);
	return arg;
}


int llvm2si_function_alloc_sreg(struct llvm2si_function_t *function,
		int count, int align)
{
	function->num_sregs = (function->num_sregs + align - 1)
			/ align * align;
	function->num_sregs += count;
	return function->num_sregs - count;
}


int llvm2si_function_alloc_vreg(struct llvm2si_function_t *function,
		int count, int align)
{
	function->num_vregs = (function->num_vregs + align - 1)
			/ align * align;
	function->num_vregs += count;
	return function->num_vregs - count;
}
