/*
 * Copyright (C) 2002,2009 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Red Hat Author(s): Nalin Dahyabhai, Behdad Esfahbod
 */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include "debug.h"
#include "ring.h"


#define VTE_POOL_BYTES	(1024*1024 - 4 * sizeof (void *)) /* hopefully we get some nice mmapped region */
#define VTE_RING_CHUNK_COMPACT_MAX_FREE		10


/*
 * VtePool: Global, alloc-only, allocator for VteCells
 */

typedef struct _VtePool VtePool;
struct _VtePool {
	VtePool *next_pool;
	guint bytes_left;
	char *cursor;
	char data[1];
};

static VtePool *current_pool;

static void *
_vte_pool_alloc (guint size)
{
	void *ret;

	if (G_UNLIKELY (!current_pool || current_pool->bytes_left < size)) {
		guint alloc_size = MAX (VTE_POOL_BYTES, size + G_STRUCT_OFFSET (VtePool, data));
		VtePool *pool = g_malloc (alloc_size);

		_vte_debug_print(VTE_DEBUG_RING, "Allocating new pool of size %d \n", alloc_size);

		pool->next_pool = current_pool;
		pool->bytes_left = alloc_size - G_STRUCT_OFFSET (VtePool, data);
		pool->cursor = pool->data;

		current_pool = pool;
	}

	_vte_debug_print(VTE_DEBUG_RING, "Allocating %d bytes from pool\n", size);

	ret = current_pool->cursor;
	current_pool->bytes_left -= size;
	current_pool->cursor += size;

	return ret;
}

static void
_vte_pool_free_all (void)
{
	_vte_debug_print(VTE_DEBUG_RING, "Freeing all pools\n");

	/* Free all cells pools */
	while (current_pool) {
		VtePool *pool = current_pool;
		current_pool = pool->next_pool;
		g_free (pool);
	}
}


/*
 * Free all pools if all rings have been destructed.
 */

static guint ring_count;

static void
_ring_created (void)
{
	ring_count++;
	_vte_debug_print(VTE_DEBUG_RING, "Rings++: %d\n", ring_count);
}

static void
_ring_destroyed (void)
{
	g_assert (ring_count > 0);
	ring_count--;
	_vte_debug_print(VTE_DEBUG_RING, "Rings--: %d\n", ring_count);

	if (!ring_count)
		_vte_pool_free_all ();
}



/*
 * VteCells: A row's cell array
 */

typedef struct _VteCells VteCells;
struct _VteCells {
	guint32 rank;
	guint32 alloc_len; /* (1 << rank) - 1 */
	union {
		VteCells *next;
		VteCell cells[1];
	} p;
};

/* Cache of freed VteCells by rank */
static VteCells *free_cells[32];

static inline VteCells *
_vte_cells_for_cell_array (VteCell *cells)
{
	if (!cells)
		return NULL;

	return (VteCells *) (((char *) cells) - G_STRUCT_OFFSET (VteCells, p));
}

static VteCells *
_vte_cells_alloc (guint len)
{
	VteCells *ret;
	guint rank = g_bit_storage (MAX (len, 80));

	g_assert (rank < 32);

	if (G_LIKELY (free_cells[rank])) {
		_vte_debug_print(VTE_DEBUG_RING, "Allocating array of %d cells (rank %d) from cache\n", len, rank);
		ret = free_cells[rank];
		free_cells[rank] = ret->p.next;

	} else {
		guint alloc_len = (1 << rank) - 1;
		_vte_debug_print(VTE_DEBUG_RING, "Allocating new array of %d cells (rank %d)\n", len, rank);

		ret = _vte_pool_alloc (G_STRUCT_OFFSET (VteCells, p) + alloc_len * sizeof (ret->p.cells[0]));

		ret->rank = rank;
		ret->alloc_len = alloc_len;
	}

	return ret;
}

static void
_vte_cells_free (VteCells *cells)
{
	_vte_debug_print(VTE_DEBUG_RING, "Freeing cells (rank %d) to cache\n", cells->rank);

	cells->p.next = free_cells[cells->rank];
	free_cells[cells->rank] = cells;
}

static inline VteCells *
_vte_cells_realloc (VteCells *cells, guint len)
{
	if (G_UNLIKELY (!cells || len > cells->alloc_len)) {
		VteCells *new_cells = _vte_cells_alloc (len);

		if (cells) {
			_vte_debug_print(VTE_DEBUG_RING, "Moving cells (rank %d to %d)\n", cells->rank, new_cells->rank);

			memcpy (new_cells->p.cells, cells->p.cells, sizeof (cells->p.cells[0]) * cells->alloc_len);
			_vte_cells_free (cells);
		}

		cells = new_cells;
	}

	return cells;
}

/* Convenience */

static inline VteCell *
_vte_cell_array_realloc (VteCell *cells, guint len)
{
	return _vte_cells_realloc (_vte_cells_for_cell_array (cells), len)->p.cells;
}

static void
_vte_cell_array_free (VteCell *cells)
{
	_vte_cells_free (_vte_cells_for_cell_array (cells));
}


/*
 * VteRowData: A row's data
 */

static VteRowData *
_vte_row_data_init (VteRowData *row)
{
	row->len = 0;
	row->soft_wrapped = 0;
	return row;
}

static void
_vte_row_data_fini (VteRowData *row)
{
	if (row->cells)
		_vte_cell_array_free (row->cells);
	row->cells = NULL;
}

static inline void
_vte_row_data_ensure (VteRowData *row, guint len)
{
	if (G_LIKELY (row->len < len))
		row->cells = _vte_cell_array_realloc (row->cells, len);
}

void
_vte_row_data_insert (VteRowData *row, guint col, const VteCell *cell)
{
	guint i;

	_vte_row_data_ensure (row, row->len + 1);

	for (i = row->len; i > col; i--)
		row->cells[i] = row->cells[i - 1];

	row->cells[col] = *cell;
	row->len++;
}

void _vte_row_data_append (VteRowData *row, const VteCell *cell)
{
	_vte_row_data_ensure (row, row->len + 1);
	row->cells[row->len] = *cell;
	row->len++;
}

void _vte_row_data_remove (VteRowData *row, guint col)
{
	guint i;

	for (i = col + 1; i < row->len; i++)
		row->cells[i - 1] = row->cells[i];

	if (G_LIKELY (row->len))
		row->len--;
}

void _vte_row_data_fill (VteRowData *row, const VteCell *cell, guint len)
{
	if (row->len < len) {
		guint i = len - row->len;

		_vte_row_data_ensure (row, len);

		for (i = row->len; i < len; i++)
			row->cells[i] = *cell;

		row->len = len;
	}
}

void _vte_row_data_shrink (VteRowData *row, guint max_len)
{
	if (max_len < row->len)
		row->len = max_len;
}



/*
 * VteRingChunk: A chunk of the scrollback buffer ring
 */

static void
_vte_ring_chunk_init (VteRingChunk *chunk)
{
	memset (chunk, 0, sizeof (*chunk));
}


/* Writable chunk type */

static void
_vte_ring_chunk_init_writable (VteRingChunk *chunk)
{
	_vte_ring_chunk_init (chunk);

	chunk->type = VTE_RING_CHUNK_TYPE_WRITABLE;
	chunk->offset = 0;
	chunk->mask = 63;
	chunk->array = g_malloc0 (sizeof (chunk->array[0]) * (chunk->mask + 1));
}

static void
_vte_ring_chunk_fini_writable (VteRingChunk *chunk)
{
	guint i;
	g_assert (chunk->type == VTE_RING_CHUNK_TYPE_WRITABLE);

	for (i = 0; i <= chunk->mask; i++)
		_vte_row_data_fini (&chunk->array[i]);

	g_free(chunk->array);
	chunk->array = NULL;
}


/* Compact chunk type */

typedef struct _VteRingChunkCompact {
	VteRingChunk base;

	guint total_bytes;
	guint bytes_left;
	char *cursor; /* move backward */
	union {
		VteRowData rows[1];
		char data[1];
	} p;
} VteRingChunkCompact;

static VteRingChunkCompact *free_chunk_compact;
static guint num_free_chunk_compact;

static VteRingChunk *
_vte_ring_chunk_new_compact (void)
{
	VteRingChunkCompact *chunk;

	if (G_LIKELY (free_chunk_compact)) {
		chunk = free_chunk_compact;
		free_chunk_compact = (VteRingChunkCompact *) chunk->base.next_chunk;
		num_free_chunk_compact--;
	} else {
		chunk = malloc (VTE_POOL_BYTES);
		chunk->total_bytes = VTE_POOL_BYTES - G_STRUCT_OFFSET (VteRingChunkCompact, p);
	}
	
	_vte_ring_chunk_init (&chunk->base);
	chunk->base.type = VTE_RING_CHUNK_TYPE_COMPACT;
	chunk->bytes_left = chunk->total_bytes;
	chunk->cursor = chunk->p.data + chunk->bytes_left;

	return &chunk->base;
}

static void
_vte_ring_chunk_free_compact (VteRingChunk *bchunk)
{
	VteRingChunkCompact *chunk = (VteRingChunkCompact *) bchunk;
	g_assert (bchunk->type == VTE_RING_CHUNK_TYPE_COMPACT);

	if (num_free_chunk_compact >= VTE_RING_CHUNK_COMPACT_MAX_FREE) {
		g_free (bchunk);
		return;
	}

	chunk->base.next_chunk = (VteRingChunk *) free_chunk_compact;
	free_chunk_compact = chunk;
	num_free_chunk_compact++;
}


/*
 * VteRing: A buffer ring
 */

#ifdef VTE_DEBUG
static void
_vte_ring_validate (VteRing * ring)
{
	VteRingChunk *chunk;

	g_assert(ring != NULL);
	_vte_debug_print(VTE_DEBUG_RING,
			" Delta = %u, Length = %u, Max = %u, Writable = %u.\n",
			ring->tail->start, ring->head->end - ring->tail->start, ring->max, ring->head->end - ring->head->start);
	g_assert(ring->head->end - ring->tail->start <= ring->max);

	chunk = ring->head;
	while (chunk) {
		g_assert(chunk->start <= chunk->end);
		if (chunk->prev_chunk)
			g_assert(chunk->start == chunk->prev_chunk->end);
		chunk = chunk->prev_chunk;
	}
}
#else
#define _vte_ring_validate(ring) G_STMT_START {} G_STMT_END
#endif

void
_vte_ring_init (VteRing *ring, guint max_rows)
{
	ring->max = MAX(max_rows, 2);

	ring->tail = ring->cursor = ring->head;

	_vte_ring_chunk_init_writable (ring->head);

	_vte_debug_print(VTE_DEBUG_RING, "New ring %p.\n", ring);
	_vte_ring_validate(ring);

	_ring_created ();
}

void
_vte_ring_fini (VteRing *ring)
{
	VteRingChunk *chunk;

	for (chunk = ring->head->prev_chunk; chunk; chunk = chunk->prev_chunk)
		_vte_ring_chunk_free_compact (chunk);

	_vte_ring_chunk_fini_writable (ring->head);

	_ring_destroyed ();
}

static VteRingChunk *
_vte_ring_find_chunk (VteRing *ring, guint position)
{
	g_assert (_vte_ring_contains (ring, position));

	while (position < ring->cursor->start)
		ring->cursor = ring->cursor->prev_chunk;
	while (position >= ring->cursor->end)
		ring->cursor = ring->cursor->next_chunk;

	return ring->cursor;
}

VteRowData *
_vte_ring_index (VteRing *ring, guint position)
{
	VteRingChunk *chunk;

	if (G_LIKELY (position >= ring->head->start))
		chunk = ring->head;
	else
		chunk = _vte_ring_find_chunk (ring, position);


	return &chunk->array[(position - chunk->offset) & chunk->mask];
}


/**
 * _vte_ring_resize:
 * @ring: a #VteRing
 * @max_rows: new maximum numbers of rows in the ring
 *
 * Changes the number of lines the ring can contain.
 */
void
_vte_ring_resize (VteRing *ring, guint max_rows)
{
#if 0
	guint position, old_max, old_mask;
	VteRowData *old_array;

	max_rows = MAX(max_rows, 2);

	if (ring->max == max_rows)
		return;

	_vte_debug_print(VTE_DEBUG_RING, "Resizing ring.\n");
	_vte_ring_validate(ring);

	old_max = ring->max;
	old_mask = ring->mask;
	old_array = ring->array;

	ring->max = max_rows;
	ring->mask = VTE_RING_MASK_FOR_MAX_ROWS (ring->max);
	if (ring->mask != old_mask) {
		ring->array = g_malloc0 (sizeof (ring->array[0]) * (ring->mask + 1));

		for (position = ring->delta; position < ring->next; position++) {
			_vte_row_data_fini (_vte_ring_index(ring, position));
			*_vte_ring_index(ring, position) = old_array[position & old_mask];
			old_array[position & old_mask].cells = NULL;
		}

		for (position = 0; position <= old_mask; position++)
			_vte_row_data_fini (&old_array[position]);

		g_free (old_array);
	}

	if (ring->next - ring->delta > ring->max) {
	  ring->delta = ring->next - ring->max;
	}

	_vte_ring_validate(ring);
#endif
}

void
_vte_ring_shrink (VteRing *ring, guint max_len)
{
#if 0
	if (ring->next - ring->delta > max_len)
		ring->next = ring->delta + max_len;
#endif
}

/**
 * _vte_ring_insert_internal:
 * @ring: a #VteRing
 * @position: an index
 *
 * Inserts a new, empty, row into @ring at the @position'th offset.
 * The item at that position and any items after that are shifted down.
 *
 * Return: the newly added row.
 */
static VteRowData *
_vte_ring_insert_internal (VteRing * ring, guint position)
{
#if 0
	guint i;
	VteRowData *row, tmp;

	g_return_val_if_fail(position >= ring->delta, NULL);
	g_return_val_if_fail(position <= ring->next, NULL);

	_vte_debug_print(VTE_DEBUG_RING, "Inserting at position %u.\n", position);
	_vte_ring_validate(ring);

	tmp = *_vte_ring_index (ring, ring->next);
	for (i = ring->next; i > position; i--)
		*_vte_ring_index (ring, i) = *_vte_ring_index (ring, i - 1);
	*_vte_ring_index (ring, position) = tmp;

	row = _vte_row_data_init(_vte_ring_index(ring, position));
	ring->next++;
	if (ring->next - ring->delta > ring->max)
		ring->delta++;

	_vte_ring_validate(ring);
	return row;
#endif
}

/**
 * _vte_ring_remove:
 * @ring: a #VteRing
 * @position: an index
 *
 * Removes the @position'th item from @ring.
 */
void
_vte_ring_remove (VteRing * ring, guint position)
{
#if 0
	guint i;
	VteRowData tmp;

	if (G_UNLIKELY (!_vte_ring_contains (ring, position)))
		return;

	_vte_debug_print(VTE_DEBUG_RING, "Removing item at position %u.\n", position);
	_vte_ring_validate(ring);

	tmp = *_vte_ring_index (ring, position);
	for (i = position; i < ring->next - 1; i++)
		*_vte_ring_index (ring, i) = *_vte_ring_index (ring, i + 1);
	*_vte_ring_index (ring, ring->next - 1) = tmp;

	if (ring->next > ring->delta)
		ring->next--;

	_vte_ring_validate(ring);
#endif
}



/**
 * _vte_ring_insert:
 * @ring: a #VteRing
 * @data: the new item
 *
 * Inserts a new, empty, row into @ring at the @position'th offset.
 * The item at that position and any items after that are shifted down.
 * It pads enough lines if @position is after the end of the ring.
 *
 * Return: the newly added row.
 */
VteRowData *
_vte_ring_insert (VteRing *ring, guint position)
{
	while (G_UNLIKELY (_vte_ring_next (ring) < position))
		_vte_ring_append (ring);
	return _vte_ring_insert_internal (ring, position);
}

/**
 * _vte_ring_append:
 * @ring: a #VteRing
 * @data: the new item
 *
 * Appends a new item to the ring.
 *
 * Return: the newly added row.
 */
VteRowData *
_vte_ring_append (VteRing * ring)
{
	return _vte_ring_insert_internal (ring, _vte_ring_next (ring));
}

