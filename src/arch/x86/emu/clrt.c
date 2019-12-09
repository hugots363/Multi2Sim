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

#include <lib/util/debug.h>
#include <mem-system/memory.h>

#include "clrt.h"
#include "context.h"
#include "regs.h"


static char *x86_clrt_err_call =
	"\tAn invalid function code was generated by the your application as an\n"
	"\targument of a system call reserved for the Multi2Sim OpenCL Runtime\n"
	"\tlibrary. Please recompile your application and try again.\n";


/* List of OpenCL Runtime calls */
enum x86_clrt_call_t
{
	x86_clrt_call_invalid = 0,
#define X86_CLRT_DEFINE_CALL(name, code) x86_clrt_call_##name = code,
#include "clrt.dat"
#undef X86_CLRT_DEFINE_CALL
	x86_clrt_call_count
};


/* List of OpenCL Runtime call names */
char *x86_clrt_call_name[x86_clrt_call_count + 1] =
{
	NULL,
#define X86_CLRT_DEFINE_CALL(name, code) #name,
#include "clrt.dat"
#undef X86_CLRT_DEFINE_CALL
	NULL
};

/* Forward declarations of OpenCL Runtime functions */
#define X86_CLRT_DEFINE_CALL(name, code) \
	static int x86_clrt_func_##name(struct x86_ctx_t *ctx);
#include "clrt.dat"
#undef X86_CLRT_DEFINE_CALL


/* List of OpenCL Runtime functions */
typedef int (*x86_clrt_func_t)(struct x86_ctx_t *ctx);
static x86_clrt_func_t x86_clrt_func_table[x86_clrt_call_count + 1] =
{
	NULL,
#define X86_CLRT_DEFINE_CALL(name, code) x86_clrt_func_##name,
#include "clrt.dat"
#undef X86_CLRT_DEFINE_CALL
	NULL
};


/* Debug */
int x86_clrt_debug_category;



int x86_clrt_call(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;

	/* Variables */
	int code;
	int ret;

	/* Function code */
	code = regs->ebx;
	if (code <= x86_clrt_call_invalid || code >= x86_clrt_call_count)
		fatal("%s: invalid OpenCL Runtime call (code %d).\n%s",
			__FUNCTION__, code, x86_clrt_err_call);

	/* Debug */
	x86_clrt_debug("OpenCL Runtime call '%s' (code %d)\n",
		x86_clrt_call_name[code], code);

	/* Call OpenCL Runtime function */
	assert(x86_clrt_func_table[code]);
	ret = x86_clrt_func_table[code](ctx);

	/* Return value */
	return ret;
}




/*
 * OpenCL Runtime call #1 - init
 *
 * @return
 *	The function always returns 0
 */

#define X86_CLRT_VERSION_MAJOR  1
#define X86_CLRT_VERSION_MINOR  752

struct x86_clrt_version_t
{
	int major;
	int minor;
};

static int x86_clrt_func_init(struct x86_ctx_t *ctx)
{
	struct x86_regs_t *regs = ctx->regs;
	struct mem_t *mem = ctx->mem;

	unsigned int version_ptr;
	struct x86_clrt_version_t version;

	/* Arguments */
	version_ptr = regs->ecx;
	x86_clrt_debug("\tversion_ptr=0x%x\n", version_ptr);

	/* Return version */
	assert(sizeof(struct x86_clrt_version_t) == 8);
	version.major = X86_CLRT_VERSION_MAJOR;
	version.minor = X86_CLRT_VERSION_MINOR;
	mem_write(mem, version_ptr, sizeof version, &version);
	x86_clrt_debug("\tMulti2Sim OpenCL implementation in host: v. %d.%d.\n",
		X86_CLRT_VERSION_MAJOR, X86_CLRT_VERSION_MINOR);
	x86_clrt_debug("\tMulti2Sim OpenCL Runtime in guest: v. %d.%d.\n",
		version.major, version.minor);

	/* Return success */
	return 0;
}

