/*
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
 *  You should have received stack copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>
#include <limits.h>
#include <math.h>

#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>

#include "bloom.h"


static
unsigned int sdbm_hash(void *key, int len)
{
	unsigned char *p = key;
	unsigned int h = 0;
	int i;
	for (i = 0; i < len; i++)
		h = p[i] + (h << 6) + (h << 16) - h;
	return h;
}


static
unsigned int sax_hash(void *key, int len)
{
	unsigned char *p = key;
	unsigned int h = 0;
	int i;
	for (i = 0; i < len; i++)
		h ^= (h << 5) + (h >> 2) + p[i];
	return h;
}


static
unsigned int elf_hash(void *key, int len)
{
	unsigned char *p = key;
	unsigned h = 0, g;
	int i;
	for (i = 0; i < len; i++) {
		h = (h << 4) + p[i];
		g = h & 0xf0000000L;
		if (g != 0)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}


inline static
void set_bit(unsigned char *bitarray, int n)
{
	bitarray[n / CHAR_BIT] |= (1 << (n % CHAR_BIT));
}


inline static
int get_bit(unsigned char *bitarray, int n)
{
	return bitarray[n / CHAR_BIT] & (1 << (n % CHAR_BIT));
}


inline static
size_t required_size_in_bits(size_t num_elements, double max_false_pos_prob)
{
	return -1 * ceil(num_elements * log(max_false_pos_prob) / pow(log(2), 2));
}


inline static
size_t maximum_capacity(size_t size_in_bits, double max_false_pos_prob)
{
	return -1 *  floor(size_in_bits *  pow(log(2), 2) / log(max_false_pos_prob));
}


inline static
size_t optimal_number_hash_funcs(size_t size_in_bits, size_t capacity)
{
	/* Number of bits per element */
	double bits_per_elem = size_in_bits / capacity;

	/* Optimal number of hash functions nedded to get the bit pattern that is stored in the filter */
	return ceil(bits_per_elem * log(2));
}


inline static
double false_positive_prob(size_t size_in_bits, size_t num_elements)
{
	/* Number of bits per element */
	double bits_per_elem = (double) size_in_bits / num_elements;

	/* Expected false positive probability with this size and capacity */
	return exp(-bits_per_elem * pow(log(2), 2));
}


struct bloom_t *bloom_create(size_t size_in_bits, size_t capacity, double max_false_pos_prob)
{
	struct bloom_t *bloom;
	size_t min_size_in_bits = 0;

	if (size_in_bits && capacity && max_false_pos_prob)
		min_size_in_bits = required_size_in_bits(capacity, max_false_pos_prob);

	else if (size_in_bits && max_false_pos_prob)
		capacity = maximum_capacity(size_in_bits, max_false_pos_prob);

	else if (capacity && max_false_pos_prob)
		size_in_bits = min_size_in_bits = required_size_in_bits(capacity, max_false_pos_prob);

	else if (size_in_bits && capacity)
		max_false_pos_prob = false_positive_prob(size_in_bits, capacity);

	else
		fatal("%s: Bad parameters", __FUNCTION__);

	assert(min_size_in_bits >= size_in_bits);
	assert(max_false_pos_prob <= 1);
	assert(size_in_bits >= capacity);

	bloom = xcalloc(1, sizeof(struct bloom_t));
	bloom->bitarray = xcalloc((size_in_bits + CHAR_BIT - 1) / CHAR_BIT, sizeof(char));
	bloom->funcs = xcalloc(NUM_HASH_FUNCTIONS, sizeof(hashfunc_t));

	bloom->funcs[0] = sdbm_hash;
	bloom->funcs[1] = sax_hash;
	bloom->funcs[2] = elf_hash;

	bloom->size_in_bits = size_in_bits;
	bloom->max_false_pos_prob = max_false_pos_prob;
	bloom->capacity = capacity;
	bloom->required_hashes = optimal_number_hash_funcs(bloom->size_in_bits, bloom->capacity);

	return bloom;
}


void bloom_free(struct bloom_t *bloom)
{
	if (!bloom) return;
	free(bloom->bitarray);
	free(bloom->funcs);
	free(bloom);
}


void bloom_clear(struct bloom_t *bloom)
{
	if (!bloom) return;
	memset(bloom->bitarray, 0, (bloom->size_in_bits + CHAR_BIT - 1) / CHAR_BIT);
	bloom->num_elements = 0;
}


double bloom_add(struct bloom_t *bloom, void *key, int len)
{
	size_t hashes = 0;

	/* The next three code blocks combine the provided hash functions,
	 * applying one after another, to simulate having more and match the requisites. */

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken one by one */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		set_bit(bloom->bitarray, result % bloom->size_in_bits);
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_done;
	}

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken two by two */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	for (int b = 0; b < NUM_HASH_FUNCTIONS; b++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		result = bloom->funcs[b]((void*) &result, sizeof(result));
		set_bit(bloom->bitarray, result % bloom->size_in_bits);
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_done;
	}

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken three by three */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	for (int b = 0; b < NUM_HASH_FUNCTIONS; b++)
	for (int c = 0; c < NUM_HASH_FUNCTIONS; c++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		result = bloom->funcs[b]((void*) &result, sizeof(result));
		result = bloom->funcs[c]((void*) &result, sizeof(result));
		set_bit(bloom->bitarray, result % bloom->size_in_bits);
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_done;
	}

	fatal("%s: Not enough hash functions", __FUNCTION__);

pattern_done:

	bloom->num_elements++;
	if (bloom->num_elements > bloom->capacity)
		return false_positive_prob(bloom->size_in_bits, bloom->num_elements); /* Max capacity reached */
	else
		return 0; /* Normal situation */
}


int bloom_find(struct bloom_t *bloom, void *key, int len)
{
	size_t hashes = 0;

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken one by one */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		if(!(get_bit(bloom->bitarray, result % bloom->size_in_bits)))
			goto pattern_not_found;
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_found;
	}

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken two by two */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	for (int b = 0; b < NUM_HASH_FUNCTIONS; b++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		result = bloom->funcs[b]((void*) &result, sizeof(result));
		if(!(get_bit(bloom->bitarray, result % bloom->size_in_bits)))
			goto pattern_not_found;
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_found;
	}

	/* Variations with repetition of NUM_HASH_FUNCTIONS taken three by three */
	for (int a = 0; a < NUM_HASH_FUNCTIONS; a++)
	for (int b = 0; b < NUM_HASH_FUNCTIONS; b++)
	for (int c = 0; c < NUM_HASH_FUNCTIONS; c++)
	{
		unsigned int result;
		result = bloom->funcs[a](key, len);
		result = bloom->funcs[b]((void*) &result, sizeof(result));
		result = bloom->funcs[c]((void*) &result, sizeof(result));
		if(!(get_bit(bloom->bitarray, result % bloom->size_in_bits)))
			goto pattern_not_found;
		hashes++;
		if (hashes == bloom->required_hashes)
			goto pattern_found;
	}

	fatal("%s: Not enough hash functions", __FUNCTION__);

pattern_found:
	return 1;
pattern_not_found:
	return 0;
}


double get_false_positive_prob(struct bloom_t *bloom)
{
	return false_positive_prob(bloom->size_in_bits, bloom->num_elements);
}
