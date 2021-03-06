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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <m2s-clrt.h>


/* Error messages */

char *m2s_clrt_err_not_impl =
	"\tThis error message is reported by the Multi2Sim OpenCL Runtime library linked\n"
	"\tto your OpenCL application. The runtime only supports partial implementation\n"
	"\tof OpenCL. To request support for this feature, please email\n"
	"\t'development@multi2sim.org'.\n";

char *m2s_clrt_err_note =
	"\tThis error message is generated by the Multi2Sim OpenCL Runtime library linked\n"
	"\twith your OpenCL host application.\n"
	"\tThis implementation only provides a subset of the OpenCL specification. Please\n"
	"\temail 'development@multi2sim.org' for further support.\n";

char *m2s_clrt_err_param_note =
	"\tThis error message is generated by the Multi2Sim OpenCL Runtime library linked\n"
	"\twith your OpenCL host application.\n"
	"\tWhile a complete OpenCL implementation would return an error code to your\n"
	"\tapplication, the Multi2Sim OpenCL library will make your program fail with an\n"
	"\terror code.\n";


/* Native mode */
int m2s_clrt_native_mode;



/*
 * Debug
 *
 * If environment variable 'M2S_CLRT_DEBUG' is set, the Multi2Sim OpenCL Runtime
 * library will dump debug information about OpenCL calls, argument values,
 * intermeidate actions, and return values.
 */

static int m2s_clrt_debug_initialized;
static int m2s_clrt_debugging;

void m2s_clrt_debug(char *fmt, ...)
{
	va_list va;
	char *value;

	/* Initialize debug */
	if (!m2s_clrt_debug_initialized)
	{
		m2s_clrt_debug_initialized = 1;
		value = getenv("M2S_CLRT_DEBUG");
		if (value && !strcmp(value, "1"))
			m2s_clrt_debugging = 1;
	}

	/* Exit if not debugging */
	if (!m2s_clrt_debugging)
		return;

	/* Dump debug message */
	va_start(va, fmt);
	fprintf(stderr, "m2s-clrt:\t");
	vfprintf(stderr, fmt, va);
	fprintf(stderr, "\n");
}





/*
 * OpenCL Interface Functions
 */

void *clGetExtensionFunctionAddress(
	const char *func_name)
{
	__M2S_CLRT_NOT_IMPL__
	return NULL;
}

