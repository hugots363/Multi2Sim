/*
 * Line Writer
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

#ifndef LINE_WRITER_H
#define LINE_WRITER_H

#include <stdio.h>


enum line_writer_align_t
{
    line_writer_align_left = 0,
    line_writer_align_center,
    line_writer_align_right,
};


struct line_writer_t
{
	char *separator;
	struct list_t *columns;
	struct list_t *col_max_size;
	int heuristic_size_enabled;
};


struct line_writer_t* line_writer_create(const char *separator);
void line_writer_free(struct line_writer_t *lw);
void line_writer_clear(struct line_writer_t *lw);
int line_writer_add_column(struct line_writer_t *lw, int min_size, enum line_writer_align_t align, const char *fmt, ...) __attribute__ ((format (printf, 4, 5)));
int line_writer_write(struct line_writer_t *lw, FILE *out);

#endif
