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

#ifndef BLOOM_H
#define BLOOM_H


#define NUM_HASH_FUNCTIONS 3

#define typeof __typeof__

#define BLOOM_ADD(bloom, key)                                                        \
	({                                                                               \
		typeof(key) __bloom_add_key = (key);                                         \
		bloom_add((bloom), (void*) &(__bloom_add_key), sizeof(__bloom_add_key));     \
	})

#define BLOOM_FIND(bloom, key)                                                       \
	({                                                                               \
		typeof(key) __bloom_find_key = (key);                                        \
		bloom_find((bloom), (void*) &(__bloom_find_key), sizeof(__bloom_find_key));  \
	})


/** Hash function */
typedef unsigned int (*hashfunc_t)(void *key, int len);


/** Bloom filter object */
struct bloom_t
{
	/* Public */
	size_t num_elements;       /** Number of elements stored in the filter */
	size_t capacity;           /** Capacyty of the filter */
	size_t size_in_bits;       /** Size in bits */
	double max_false_pos_prob; /** Maximum false positive probability */
	size_t required_hashes;    /** Optimal number of hash functions to minimize the probability of false positives */

	/* Private */
	unsigned char *bitarray;   /** Array of bits */
	hashfunc_t *funcs;         /** Hash functions avaible (NUM_HASH_FUNCTIONS) */
};


/** Create a bloom filter
 *
 * The size of the filter must be at least enough to match the capacity and maximum false positive probability specified.
 * If the size specified is 0, the minimum required to match the capacity and probability is used.
 *
 * @param size_in_bits
 *  Size of the array used by the filter in bits
 * @param capacity
 *  Minimum number of elements that can be safely stored in the filter without rebasing the maximum false positive probability specified
 * @param max_false_pos_prob
 *  Guaranteed maximum false positive probability
 *
 * @return
 *  Bloom filter object
 */
struct bloom_t *bloom_create(size_t size_in_bits, size_t capacity, double max_false_pos_prob);


/** Free bloom filter
 */
void bloom_free(struct bloom_t *bloom);


/** Remove all the elements stored in the bloom filter
 *
 * @param bloom
 *  Pointer to the bloom filter object
 */
void bloom_clear(struct bloom_t *bloom);


/** Add an element to the bloom filter
 *
 * @param bloom
 *  Pointer to the bloom filter object
 * @param key
 *  Pointer to the key of the element
 * @param len
 *  Length of the key
 *
 * @return
 *  The function returns 0 on success
 *
 *  The expected probability of false positives is returned if too much elements
 * are added to the filter and it rebases the maximum value that was set on creation.
 */
double bloom_add(struct bloom_t *bloom, void *key, int len);


/** Find an element in the bloom filter
 *
 * @param bloom
 *  Pointer to the bloom filter object
 * @param key
 *  Pointer to the key of the element
 * @param len
 *  Length of the key
 *
 * @return
 *  The function returns 1 if the element is in the filter and 0 otherwise
 */
int bloom_find(struct bloom_t *bloom, void *key, int len);


/** Returns the current false positive probability for the bloom filter
 *
 * @param bloom
 *  Pointer to the bloom filter object
 *
 * @return
 *  The probability
 */
double get_false_positive_prob(struct bloom_t *bloom);

#endif /* BLOOM_H */
