/*
 *  Libstruct
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
 *  Copyright (C) 2014  Vicent Selfa (viselol@disca.upv.es)
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

#ifndef LIB_UTIL_HASH_TABLE_GEN_H
#define LIB_UTIL_HASH_TABLE_GEN_H


/** Iterate through all elements of the hash table.
 *
 * @param table
 * @param key
 * @param data
 */
#define HASH_TABLE_GEN_FOR_EACH(table, key, key_len, data) \
	for (hash_table_gen_find_first((table), (void **) &(key), &(key_len), (void **) &(data)); \
		(key); \
		(key) = hash_table_gen_find_next((table), (void **) &(key), &(key_len), (void **) &(data)))


/* Creation and destruction */
struct hash_table_gen_t *hash_table_gen_create(unsigned int size);
void hash_table_gen_free(struct hash_table_gen_t *table);

/* Delete all elements */
void hash_table_gen_clear(struct hash_table_gen_t *table);

/* Insert a new element.
 * The key is strdup'ped, so it can be freely modified by the caller.
 * Return value: 0=success, non-0=key already exists/data=NULL
 */
int hash_table_gen_insert(struct hash_table_gen_t *table, char *key, int key_len, void *data);

/* Change element data.
 * Return value: 0=success, non-0=key does not exist/data=NULL
 */
int hash_table_gen_set(struct hash_table_gen_t *table, char *key, int key_len, void *data);

/* Return number of elements in hash_table_gen. */
unsigned int hash_table_gen_count(struct hash_table_gen_t *table);

/* Get data associated to a key.
 * Return value: NULL=key does not exist, ptr=data */
void *hash_table_gen_get(struct hash_table_gen_t *table, char *key, int key_len);

/* Remove data associated to a key; the key is freed.
 * Return value: NULL=key does not exist, ptr=data removed */
void *hash_table_gen_remove(struct hash_table_gen_t *table, char *key, int key_len);

/* Find elements in hash table sequentially.
 * key_ptr == NULL=no more elements,
 *   non-NULL key (data returned in 'data' if not NULL) */
void hash_table_gen_find_first(struct hash_table_gen_t *table, void **key_ptr, int *key_len_ptr, void **data);
void hash_table_gen_find_next(struct hash_table_gen_t *table, void **key_ptr, int *key_len_ptr, void **data);

#endif /* LIB_UTIL_HASH_TABLE_GEN_H */

