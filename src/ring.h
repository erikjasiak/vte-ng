/*
 * Copyright (C) 2002 Red Hat, Inc.
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
 */

/* The interfaces in this file are subject to change at any time. */

#ifndef vte_ring_h_included
#define vte_ring_h_included


#include <glib.h>

G_BEGIN_DECLS

typedef struct _VteRowData {
	GArray *cells;
	guchar soft_wrapped: 1;
} VteRowData;

typedef struct _VteRing VteRing;

struct _VteRing {
	glong delta, length, max;
	glong cached_item;
	gpointer cached_data;
	VteRowData **array;
};

#define _vte_ring_contains(__ring, __position) \
	(((__position) >= (__ring)->delta) && \
	 ((__position) < (__ring)->delta + (__ring)->length))
#define _vte_ring_delta(__ring) ((__ring)->delta)
#define _vte_ring_length(__ring) ((__ring)->length)
#define _vte_ring_next(__ring) ((__ring)->delta + (__ring)->length)
#define _vte_ring_max(__ring) ((__ring)->max)
#define _vte_ring_is_cached(__ring, __v) ((__ring)->cached_item == (__v))
#define _vte_ring_get_cached_data(__ring) ((__ring)->cached_data)
#define _vte_ring_set_cache(__ring, __v, __data) ((__ring)->cached_item = (__v), (__ring)->cached_data = (__data))
#ifdef VTE_DEBUG
#define _vte_ring_index(__ring, __position) \
	((__ring)->array[(__position) % (__ring)->max] ? \
	 (__ring)->array[(__position) % (__ring)->max] : \
	 (g_critical("NULL at %ld(->%ld) delta %ld, length %ld, max %ld next %ld" \
		  " at %d\n", \
		  (__position), (__position) % (__ring)->max, \
		  (__ring)->delta, (__ring)->length, (__ring)->max, \
		  (__ring)->delta + (__ring)->length, \
		  __LINE__), (gpointer) NULL))
#else
#define _vte_ring_index(__ring, __position) \
	((__ring)->array[(__position) % (__ring)->max])
#endif

VteRing *_vte_ring_new(glong max_elements);
VteRing *_vte_ring_new_with_delta(glong max_elements, glong delta);
void _vte_ring_insert(VteRing *ring, glong position, gpointer data);
void _vte_ring_insert_preserve(VteRing *ring, glong position, gpointer data);
void _vte_ring_remove(VteRing *ring, glong position, gboolean free_element);
void _vte_ring_append(VteRing *ring, gpointer data);
void _vte_ring_free(VteRing *ring, gboolean free_elements);

G_END_DECLS

#endif
