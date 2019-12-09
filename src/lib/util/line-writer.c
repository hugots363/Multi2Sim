/*
 * Table Writer
 * Copyright (C) 2013 Vicent Selfa (vtselfa@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <lib/mhandle/mhandle.h>

#include "debug.h"
#include "list.h"
#include "line-writer.h"


struct line_writer_t* line_writer_create(const char *separator)
{
	assert(separator);

	struct line_writer_t *lw = xcalloc(1, sizeof(struct line_writer_t));
	lw->separator = xstrdup(separator);
	lw->columns = list_create();
	lw->col_max_size = list_create();
	return lw;
}


void line_writer_free(struct line_writer_t *lw)
{
	char *column;
	while((column = list_pop(lw->columns)))
		free(column);
	list_free(lw->columns);

	while(list_pop(lw->col_max_size));
	list_free(lw->col_max_size);

	free(lw->separator);
	free(lw);
}


void line_writer_clear(struct line_writer_t *lw)
{
	char *column;
	while((column = list_pop(lw->columns)))
		free(column);
}


int line_writer_add_column(struct line_writer_t *lw, int min_size, enum line_writer_align_t align, const char *fmt, ...)
{
	char *tmp;
	char *aux;
	int len;
	int spaces;
	va_list args;
	int size;

	assert(min_size > 0);

	va_start (args, fmt);
	len = vsnprintf(NULL, 0, fmt, args);
	va_end (args);

	size = (int) (size_t) list_get(lw->col_max_size, list_count(lw->columns));
	if(lw->heuristic_size_enabled && size > min_size)
		min_size = size;

	if (lw->col_max_size->error_code == LIST_ERR_BOUNDS)
		list_add(lw->col_max_size, (void*) (size_t) (min_size > len ? min_size : len));
	else if (size < min_size)
		list_set(lw->col_max_size, list_count(lw->columns), (void*) (size_t) (min_size > len ? min_size : len));

	if (len < min_size)
		spaces = min_size - len;
	else
		spaces = 0;

	tmp = xcalloc(len + spaces + 1, sizeof(char));

	switch(align)
	{
		case line_writer_align_right:
			aux = tmp + spaces;
		break;

		case line_writer_align_left:
			aux = tmp;
		break;

		case line_writer_align_center:
			aux = tmp + spaces / 2;
		break;

		default:
			panic("Invalid alignment");
		break;
	}
	tmp[len + spaces] = '\0';

	va_start(args, fmt);
	vsnprintf(aux, len + 1, fmt, args);
	va_end(args);

	switch(align)
	{
		case line_writer_align_right:
			for(aux = tmp; aux != tmp + spaces; aux++)
				*aux = ' ';
		break;

		case line_writer_align_left:
			for(aux = tmp + len; aux != tmp + len + spaces; aux++)
				*aux = ' ';
		break;

		case line_writer_align_center:
			for(aux = tmp; aux != tmp + spaces / 2; aux++)
				*aux = ' ';
			for(aux = tmp + spaces / 2 + len; aux != tmp + len + spaces; aux++)
				*aux = ' ';
		break;

		default:
		break;
	}

	list_add(lw->columns, tmp);
	return len + spaces; /* Efective size */
}


int line_writer_write(struct line_writer_t *lw, FILE *out)
{
	int i;
	int size = strlen(lw->separator) * (list_count(lw->columns) - 1);
	char *str;
	char *aux;

	LIST_FOR_EACH(lw->columns, i)
	{
		char *column = list_get(lw->columns, i);
		size += strlen(column);
	}
	size += 2; /* Newline and null termination */

	str = xcalloc(size, sizeof(char));
	aux = str;

	LIST_FOR_EACH(lw->columns, i)
	{
		char *column = list_get(lw->columns, i);
		int len = sprintf(aux, "%s%s", i ? lw->separator : "", column);
		aux += len;
	}
	*aux = '\n';

	fputs(str, out);
	fflush(out);
	free(str);

	return size - 1;
}
