/*    This file is part of nitlib.
 *
 *    Nitlib is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Foobar is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIT_SHORT_NAMES
#include "list.h"
#include "hashmap.h"

#define BIN_MAX_DENSITY 5
#define HASH_SEED 37

static const int hashmap_primes[] = {
	8 + 3, 16 + 3, 32 + 5, 64 + 3, 128 + 3, 256 + 27, 512 + 9,
	1024 + 9, 2048 + 5, 4096 + 3, 8192 + 27, 16384 + 43, 32768 + 3,
	65536 + 45, 131072 + 29, 262144 + 3, 524288 + 21, 1048576 + 7,
	2097152 + 17, 4194304 + 15, 8388608 + 9, 16777216 + 43, 33554432 + 35,
	67108864 + 15, 134217728 + 29, 268435456 + 3, 536870912 + 11,
	1073741824 + 85, 0
};

void rehash(struct nit_hashmap *map);

#define ROT32(x, y) ((x << y) | (x >> (32 - y)))
uint32_t
murmur3_32(const char *key, uint32_t len, uint32_t seed)
{
	static const uint32_t c1 = 0xcc9e2d51;
	static const uint32_t c2 = 0x1b873593;
	static const uint32_t r1 = 15;
	static const uint32_t r2 = 13;
	static const uint32_t m = 5;
	static const uint32_t n = 0xe6546b64;

	uint32_t hash = seed;

	const int nblocks = len / 4;
	const uint32_t *blocks = (const uint32_t *) key;
	int i;
	uint32_t k;

	for (i = 0; i < nblocks; i++) {
		k = blocks[i];
		k *= c1;
		k = ROT32(k, r1);
		k *= c2;

		hash ^= k;
		hash = ROT32(hash, r2) * m + n;
	}

	const uint8_t *tail = (const uint8_t *) (key + nblocks * 4);
	uint32_t k1 = 0;

	switch (len & 3) {
	case 3:
		k1 ^= tail[2] << 16;
	case 2:
		k1 ^= tail[1] << 8;
	case 1:
		k1 ^= tail[0];

		k1 *= c1;
		k1 = ROT32(k1, r1);
		k1 *= c2;
		hash ^= k1;
	}

	hash ^= len;
	hash ^= (hash >> 16);
	hash *= 0x85ebca6b;
	hash ^= (hash >> 13);
	hash *= 0xc2b2ae35;
	hash ^= (hash >> 16);

	return hash;
}

struct nit_hashentry *
hashentry_new(void *key, uint32_t key_size, void *storage)
{
	struct nit_hashentry *entry = malloc(sizeof(*entry));

	entry->key = key;
	entry->key_size = key_size;
	entry->storage = storage;
        LIST_CONS(entry, NULL);
	return entry;
}

struct nit_hashmap *
hashmap_new(unsigned int sequence,
	    int (*compare)(const void *entry_key, uint32_t entry_key_size,
			   const void *key, uint32_t key_size),
	    void (*free_contents)(void *key, void *storage))
{
	int i = 0;
	struct nit_hashmap *map = malloc(sizeof(*map));
	struct nit_hashbin *bin;

	map->compare = compare;
	map->free_contents = free_contents;
	map->bin_num = hashmap_primes[sequence];
	map->bins = malloc(sizeof(*map->bins) * hashmap_primes[sequence]);
	map->entry_num = 0;
	map->primes_pointer = hashmap_primes;
	bin = map->bins;


	for (; i != hashmap_primes[sequence]; ++i) {
		bin->first = NULL;
		++bin;
	}

	return map;
}

void
hashmap_free(struct nit_hashmap *map)
{
	struct nit_hashbin *bin = map->bins;

	unsigned int i;

	for (i = 0; i != map->bin_num; ++i, ++bin) {
		struct nit_hashentry *entry = bin->first;
		struct nit_hashentry *tmp;

		delayed_foreach (tmp, entry) {
			map->free_contents(tmp->key, tmp->storage);
			free(tmp);
		}
	}

	free(map->bins);
	free(map);
}

struct nit_hashentry **
hashmap_entry(struct nit_hashmap *map, void *key, uint32_t key_size)
{
	struct nit_hashentry *entry;
	unsigned int row;

	row = murmur3_32(key, key_size, HASH_SEED) % map->bin_num;
	entry = map->bins[row].first;

	if (!entry || map->compare(entry->key, entry->key_size, key, key_size))
		return &map->bins[row].first;

        foreach (entry) {
		struct nit_hashentry *next = LIST_NEXT(entry);

		if (!next || map->compare(next->key, next->key_size,
					 key, key_size))
			return NEXT_REF(entry);
	}

	fprintf(stderr,
		"Error in nitlib hashmap: NULL not at end of entries!\n");
	exit(EXIT_FAILURE);
}

enum nit_map_occured
hashmap_add(struct nit_hashmap *map, void *key, uint32_t key_size,
	    void *storage)
{
	struct nit_hashentry **entry = hashmap_entry(map, key, key_size);

	if (*entry)
		return NIT_HASHMAP_ALREADY_PRESENT;

	*entry = hashentry_new(key, key_size, storage);

	if (++map->entry_num / map->bin_num >= BIN_MAX_DENSITY)
		hashmap_rehash(map);

	return NIT_HASHMAP_ADDED;
}

void
hashmap_remove(struct nit_hashmap *map, void *key, uint32_t key_size)
{
	unsigned int row = murmur3_32(key, key_size, HASH_SEED) % map->bin_num;
	struct nit_hashentry *entry = map->bins[row].first;

	if (!entry)
		return;

	if (map->compare(entry->key, entry->key_size, key, key_size)) {
		map->bins[row].first = LIST_NEXT(entry);
		map->free_contents(entry->key, entry->storage);
		free(entry);
		--map->entry_num;
		return;
	}

	struct nit_hashentry *prev = entry;

	entry = LIST_NEXT(entry);

	foreach (entry) {
		if (map->compare(entry->key, entry->key_size, key, key_size)) {
		        LIST_CONS(prev, LIST_NEXT(entry));
			map->free_contents(entry->key, entry->storage);
			free(entry);
			--map->entry_num;
			return;
		}
		prev = entry;
	}
}

void *
hashmap_get(const struct nit_hashmap *map, const void *key, uint32_t key_size)
{
	unsigned int row = murmur3_32(key, key_size, HASH_SEED) % map->bin_num;
	struct nit_hashentry *entry = map->bins[row].first;

        foreach (entry)
		if (map->compare(entry->key, entry->key_size, key, key_size))
			return entry->storage;

	return NULL;
}

/* Adds to a bin something already in the hashmap during a rehash */
static void
rehash_add(struct nit_hashbin *bin, struct nit_hashentry *entry)
{
	struct nit_hashentry *tmp = bin->first;

        LIST_CONS(entry, NULL);

	if (!tmp) {
		bin->first = entry;
		return;
	}

	/* Finds end of list */
	while (LIST_NEXT(tmp))
		tmp = LIST_NEXT(tmp);

        LIST_CONS(tmp, entry);
}

void
hashmap_rehash(struct nit_hashmap *map)
{
	unsigned int i;
	unsigned int new_bin_num = map->primes_pointer[1];
	struct nit_hashbin *new_bins = malloc(sizeof(*new_bins) * new_bin_num);
	struct nit_hashbin *bin = map->bins;

	for (i = 0; i != new_bin_num; ++i)
		new_bins[i].first = NULL;

	for (i = 0; i != map->bin_num; ++i, ++bin) {
		struct nit_hashentry *entry = bin->first;
		struct nit_hashentry *tmp;

		delayed_foreach (tmp, entry) {
			uint32_t row = murmur3_32(tmp->key, tmp->key_size,
						  HASH_SEED) % new_bin_num;

			rehash_add(new_bins + row, tmp);
		}
	}

	free(map->bins);
	map->bins = new_bins;
	++map->primes_pointer;
	map->bin_num = new_bin_num;
}
