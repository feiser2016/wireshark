/* proto.c
 * Routines for protocol tree
 *
 * $Id: proto.c,v 1.101 2003/10/29 23:48:13 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@ethereal.com>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <float.h>

#ifdef NEED_SNPRINTF_H
# include "snprintf.h"
#endif

#include "packet.h"
#include "strutil.h"
#include "resolv.h"
#include "plugins.h"
#include "ipv6-utils.h"
#include "proto.h"
#include "int-64bit.h"
#include "epan_dissect.h"

#define cVALS(x) (const value_string*)(x)

static gboolean
proto_tree_free_node(GNode *node, gpointer data);

static void fill_label_boolean(field_info *fi, gchar *label_str);
static void fill_label_uint(field_info *fi, gchar *label_str);
static void fill_label_uint64(field_info *fi, gchar *label_str);
static void fill_label_int64(field_info *fi, gchar *label_str);
static void fill_label_enumerated_uint(field_info *fi, gchar *label_str);
static void fill_label_enumerated_bitfield(field_info *fi, gchar *label_str);
static void fill_label_numeric_bitfield(field_info *fi, gchar *label_str);
static void fill_label_int(field_info *fi, gchar *label_str);
static void fill_label_enumerated_int(field_info *fi, gchar *label_str);

int hfinfo_bitwidth(header_field_info *hfinfo);
static char* hfinfo_uint_vals_format(header_field_info *hfinfo);
static char* hfinfo_uint_format(header_field_info *hfinfo);
static char* hfinfo_int_vals_format(header_field_info *hfinfo);
static char* hfinfo_int_format(header_field_info *hfinfo);

static proto_item*
proto_tree_add_node(proto_tree *tree, field_info *fi);

static field_info *
alloc_field_info(proto_tree *tree, int hfindex, tvbuff_t *tvb,
        gint start, gint *length);

static proto_item *
proto_tree_add_pi(proto_tree *tree, int hfindex, tvbuff_t *tvb,
        gint start, gint *length, field_info **pfi);

static void
proto_tree_set_representation(proto_item *pi, const char *format, va_list ap);

static void
proto_tree_set_protocol_tvb(field_info *fi, tvbuff_t *tvb);
static void
proto_tree_set_uint64(field_info *fi, const guint8 *value_ptr, gboolean little_endian);
static void
proto_tree_set_uint64_tvb(field_info *fi, tvbuff_t *tvb, gint start, gboolean little_endian);
static void
proto_tree_set_bytes(field_info *fi, const guint8* start_ptr, gint length);
static void
proto_tree_set_bytes_tvb(field_info *fi, tvbuff_t *tvb, gint offset, gint length);
static void
proto_tree_set_time(field_info *fi, nstime_t *value_ptr);
static void
proto_tree_set_string(field_info *fi, const char* value, gboolean);
static void
proto_tree_set_string_tvb(field_info *fi, tvbuff_t *tvb, gint start, gint length);
static void
proto_tree_set_ether(field_info *fi, const guint8* value);
static void
proto_tree_set_ether_tvb(field_info *fi, tvbuff_t *tvb, gint start);
static void
proto_tree_set_ipxnet(field_info *fi, guint32 value);
static void
proto_tree_set_ipv4(field_info *fi, guint32 value);
static void
proto_tree_set_ipv6(field_info *fi, const guint8* value_ptr);
static void
proto_tree_set_ipv6_tvb(field_info *fi, tvbuff_t *tvb, gint start);
static void
proto_tree_set_boolean(field_info *fi, guint32 value);
static void
proto_tree_set_float(field_info *fi, float value);
static void
proto_tree_set_double(field_info *fi, double value);
static void
proto_tree_set_uint(field_info *fi, guint32 value);
static void
proto_tree_set_int(field_info *fi, gint32 value);

static int proto_register_field_init(header_field_info *hfinfo, int parent);

/* Comparision function for tree insertion. A wrapper around strcmp() */
static int g_strcmp(gconstpointer a, gconstpointer b);

/* special-case header field used within proto.c */
int hf_text_only = -1;

/* Structure for information about a protocol */
typedef struct {
	char	*name;		/* long description */
	char	*short_name;	/* short description */
	char	*filter_name;	/* name of this protocol in filters */
	int	proto_id;	/* field ID for this protocol */
	GList	*fields;	/* fields for this protocol */
	GList	*last_field;	/* pointer to end of list of fields */
	gboolean is_enabled;	/* TRUE if protocol is enabled */
	gboolean can_disable;	/* TRUE if protocol can be disabled */
} protocol_t;

static protocol_t *find_protocol_by_id(int proto_id);

/* List of all protocols */
static GList *protocols;

#define INITIAL_NUM_PROTOCOL_HFINFO     200
#define INITIAL_NUM_FIELD_INFO          100
#define INITIAL_NUM_PROTO_NODE          100
#define INITIAL_NUM_ITEM_LABEL          100


/* Contains information about protocols and header fields. Used when
 * dissectors register their data */
static GMemChunk *gmc_hfinfo = NULL;

/* Contains information about a field when a dissector calls
 * proto_tree_add_item.  */
static GMemChunk *gmc_field_info = NULL;

/* Contains the space for proto_nodes. */
static GMemChunk *gmc_proto_node = NULL;

/* String space for protocol and field items for the GUI */
static GMemChunk *gmc_item_labels = NULL;

/* List which stores protocols and fields that have been registered */
static GPtrArray *gpa_hfinfo = NULL;

/* Balanced tree of abbreviations and IDs */
static GTree *gpa_name_tree = NULL;

/* Points to the first element of an array of Booleans, indexed by
   a subtree item type; that array element is TRUE if subtrees of
   an item of that type are to be expanded. */
gboolean	*tree_is_expanded;

/* Number of elements in that array. */
int		num_tree_types;

/* initialize data structures and register protocols and fields */
void
proto_init(const char *plugin_dir
#ifndef HAVE_PLUGINS
				 _U_
#endif
	   ,
	   void (register_all_protocols)(void),
	   void (register_all_protocol_handoffs)(void))
{
	static hf_register_info hf[] = {
		{ &hf_text_only,
		{ "",	"", FT_NONE, BASE_NONE, NULL, 0x0,
			NULL, HFILL }},
	};

	proto_cleanup();

	gmc_hfinfo = g_mem_chunk_new("gmc_hfinfo",
		sizeof(header_field_info),
        INITIAL_NUM_PROTOCOL_HFINFO * sizeof(header_field_info),
        G_ALLOC_ONLY);

	gmc_field_info = g_mem_chunk_new("gmc_field_info",
		sizeof(field_info),
        INITIAL_NUM_FIELD_INFO * sizeof(field_info),
		G_ALLOC_AND_FREE);

	gmc_proto_node = g_mem_chunk_new("gmc_proto_node",
		sizeof(proto_node),
        INITIAL_NUM_PROTO_NODE * sizeof(proto_node),
		G_ALLOC_AND_FREE);

	gmc_item_labels = g_mem_chunk_new("gmc_item_labels",
		ITEM_LABEL_LENGTH,
        INITIAL_NUM_ITEM_LABEL* ITEM_LABEL_LENGTH,
		G_ALLOC_AND_FREE);

	gpa_hfinfo = g_ptr_array_new();
	gpa_name_tree = g_tree_new(g_strcmp);

	/* Initialize the ftype subsystem */
	ftypes_initialize();

	/* Register one special-case FT_TEXT_ONLY field for use when
	   converting ethereal to new-style proto_tree. These fields
	   are merely strings on the GUI tree; they are not filterable */
	proto_register_field_array(-1, hf, array_length(hf));

	/* Have each built-in dissector register its protocols, fields,
	   dissector tables, and dissectors to be called through a
	   handle, and do whatever one-time initialization it needs to
	   do. */
	register_all_protocols();

#ifdef HAVE_PLUGINS
	/* Now scan for plugins and load all the ones we find, calling
	   their register routines to do the stuff described above. */
	init_plugins(plugin_dir);
#endif

	/* Now call the "handoff registration" routines of all built-in
	   dissectors; those routines register the dissector in other
	   dissectors' handoff tables, and fetch any dissector handles
	   they need. */
	register_all_protocol_handoffs();

#ifdef HAVE_PLUGINS
	/* Now do the same with plugins. */
	register_all_plugin_handoffs();
#endif

	/* We've assigned all the subtree type values; allocate the array
	   for them, and zero it out. */
	tree_is_expanded = g_malloc(num_tree_types*sizeof (gint *));
	memset(tree_is_expanded, 0, num_tree_types*sizeof (gint *));
}

/* String comparison func for dfilter_token GTree */
static int
g_strcmp(gconstpointer a, gconstpointer b)
{
	return strcmp((const char*)a, (const char*)b);
}

void
proto_cleanup(void)
{
	/* Free the abbrev/ID GTree */
	if (gpa_name_tree) {
		g_tree_destroy(gpa_name_tree);
		gpa_name_tree = NULL;
	}

	if (gmc_hfinfo)
		g_mem_chunk_destroy(gmc_hfinfo);
	if (gmc_field_info)
		g_mem_chunk_destroy(gmc_field_info);
	if (gmc_proto_node)
		g_mem_chunk_destroy(gmc_proto_node);
	if (gmc_item_labels)
		g_mem_chunk_destroy(gmc_item_labels);
	if (gpa_hfinfo)
		g_ptr_array_free(gpa_hfinfo, TRUE);
	if (tree_is_expanded != NULL)
		g_free(tree_is_expanded);

	/* Cleanup the ftype subsystem */
	ftypes_cleanup();
}

/* frees the resources that the dissection a proto_tree uses */
void
proto_tree_free(proto_tree *tree)
{
	/* Free all the data pointed to by the tree. */
	g_node_traverse((GNode*)tree, G_IN_ORDER, G_TRAVERSE_ALL, -1,
		proto_tree_free_node, NULL);

	/* Then free the tree. */
	g_node_destroy((GNode*)tree);
}

/* We accept a void* instead of a field_info* to satisfy CLEANUP_POP */
static void
free_field_info(void *fi)
{
	g_mem_chunk_free(gmc_field_info, (field_info*)fi);
}

static void
free_GPtrArray_value(gpointer key _U_, gpointer value, gpointer user_data _U_)
{
	GPtrArray   *ptrs = value;

	g_ptr_array_free(ptrs, TRUE);
}

static void
free_node_tree_data(tree_data_t *tree_data)
{
        /* Free all the GPtrArray's in the interesting_hfids hash. */
        g_hash_table_foreach(tree_data->interesting_hfids,
            free_GPtrArray_value, NULL);

        /* And then destroy the hash. */
        g_hash_table_destroy(tree_data->interesting_hfids);

        /* And finally the tree_data_t itself. */
        g_free(tree_data);
}

static void
free_node_field_info(field_info* finfo)
{
	if (finfo->representation) {
		g_mem_chunk_free(gmc_item_labels, finfo->representation);
	}
	fvalue_free(finfo->value);
	free_field_info(finfo);
}

static gboolean
proto_tree_free_node(GNode *node, gpointer data _U_)
{
	field_info *finfo = PITEM_FINFO(node);

	if (finfo == NULL) {
		/* This is the root GNode. Destroy the per-tree data.
		 * There is no field_info to destroy. */
		free_node_tree_data(PTREE_DATA(node));
	}
	else {
		/* This is a child GNode. Don't free the per-tree data, but
		 * do free the field_info data. */
		free_node_field_info(finfo);
	}

	/* Free the proto_node. */
	g_mem_chunk_free(gmc_proto_node, GNODE_PNODE(node));

	return FALSE; /* FALSE = do not end traversal of GNode tree */
}

/* Is the parsing being done for a visible proto_tree or an invisible one?
 * By setting this correctly, the proto_tree creation is sped up by not
 * having to call vsnprintf and copy strings around.
 */
void
proto_tree_set_visible(proto_tree *tree, gboolean visible)
{
	PTREE_DATA(tree)->visible = visible;
}

/* Finds a record in the hf_info_records array by id. */
header_field_info*
proto_registrar_get_nth(int hfindex)
{
	g_assert(hfindex >= 0 && (guint) hfindex < gpa_hfinfo->len);
	return g_ptr_array_index(gpa_hfinfo, hfindex);
}

/* Finds a record in the hf_info_records array by name.
 */
header_field_info*
proto_registrar_get_byname(char *field_name)
{
	g_assert(field_name != NULL);
	return g_tree_lookup(gpa_name_tree, field_name);
}

/* Add a text-only node, leaving it to our caller to fill the text in */
static proto_item *
proto_tree_add_text_node(proto_tree *tree, tvbuff_t *tvb, gint start, gint length)
{
	proto_item	*pi;

	pi = proto_tree_add_pi(tree, hf_text_only, tvb, start, &length, NULL);
	if (pi == NULL)
		return(NULL);

	return pi;
}

/* Add a text-only node to the proto_tree */
proto_item *
proto_tree_add_text(proto_tree *tree, tvbuff_t *tvb, gint start, gint length,
	const char *format, ...)
{
	proto_item	*pi;
	va_list		ap;

	pi = proto_tree_add_text_node(tree, tvb, start, length);
	if (pi == NULL)
		return(NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Add a text-only node to the proto_tree (va_list version) */
proto_item *
proto_tree_add_text_valist(proto_tree *tree, tvbuff_t *tvb, gint start,
	gint length, const char *format, va_list ap)
{
	proto_item	*pi;

	pi = proto_tree_add_text_node(tree, tvb, start, length);
	if (pi == NULL)
		return(NULL);

	proto_tree_set_representation(pi, format, ap);

	return pi;
}

/* Add a text-only node for debugging purposes. The caller doesn't need
 * to worry about tvbuff, start, or length. Debug message gets sent to
 * STDOUT, too */
proto_item *
proto_tree_add_debug_text(proto_tree *tree, const char *format, ...)
{
	proto_item	*pi;
	va_list		ap;

	pi = proto_tree_add_text_node(tree, NULL, 0, 0);
	if (pi == NULL)
		return(NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	vprintf(format, ap);
	va_end(ap);
	printf("\n");

	return pi;
}


static guint32
get_uint_value(tvbuff_t *tvb, gint offset, gint length, gboolean little_endian)
{
	guint32 value;

	switch (length) {

	case 1:
		value = tvb_get_guint8(tvb, offset);
		break;

	case 2:
		value = little_endian ? tvb_get_letohs(tvb, offset)
				      : tvb_get_ntohs(tvb, offset);
		break;

	case 3:
		value = little_endian ? tvb_get_letoh24(tvb, offset)
				      : tvb_get_ntoh24(tvb, offset);
		break;

	case 4:
		value = little_endian ? tvb_get_letohl(tvb, offset)
				      : tvb_get_ntohl(tvb, offset);
		break;

	default:
		g_assert_not_reached();
		value = 0;
		break;
	}
	return value;
}

static gint32
get_int_value(tvbuff_t *tvb, gint offset, gint length, gboolean little_endian)
{
	gint32 value;

	switch (length) {

	case 1:
		value = (gint8)tvb_get_guint8(tvb, offset);
		break;

	case 2:
		value = (gint16) (little_endian ? tvb_get_letohs(tvb, offset)
						: tvb_get_ntohs(tvb, offset));
		break;

	case 3:
		value = little_endian ? tvb_get_letoh24(tvb, offset)
				      : tvb_get_ntoh24(tvb, offset);
		if (value & 0x00800000) {
			/* Sign bit is set; sign-extend it. */
			value |= 0xFF000000;
		}
		break;

	case 4:
		value = little_endian ? tvb_get_letohl(tvb, offset)
				      : tvb_get_ntohl(tvb, offset);
		break;

	default:
		g_assert_not_reached();
		value = 0;
		break;
	}
	return value;
}

/* Add an item to a proto_tree, using the text label registered to that item;
   the item is extracted from the tvbuff handed to it. */
proto_item *
proto_tree_add_item(proto_tree *tree, int hfindex, tvbuff_t *tvb,
    gint start, gint length, gboolean little_endian)
{
	field_info	*new_fi;
	proto_item	*pi;
	guint32		value, n;
	char		*string;
	GHashTable	*hash;
	GPtrArray	*ptrs;

	if (!tree)
		return(NULL);

	new_fi = alloc_field_info(tree, hfindex, tvb, start, &length);

	if (new_fi == NULL)
		return(NULL);

	/* Register a cleanup function in case on of our tvbuff accesses
	 * throws an exception. We need to clean up new_fi. */
	CLEANUP_PUSH(free_field_info, new_fi);

	switch(new_fi->hfinfo->type) {
		case FT_NONE:
			/* no value to set for FT_NONE */
			break;

		case FT_PROTOCOL:
			proto_tree_set_protocol_tvb(new_fi, tvb);
			break;

		case FT_BYTES:
			proto_tree_set_bytes_tvb(new_fi, tvb, start, length);
			break;

		case FT_UINT_BYTES:
			n = get_uint_value(tvb, start, length, little_endian);
			proto_tree_set_bytes_tvb(new_fi, tvb, start + length, n);

			/* Instead of calling proto_item_set_len(), since we don't yet
			 * have a proto_item, we set the field_info's length ourselves. */
			new_fi->length = n + length;
			break;

		case FT_BOOLEAN:
			proto_tree_set_boolean(new_fi,
			    get_uint_value(tvb, start, length, little_endian));
			break;

		/* XXX - make these just FT_UINT? */
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
			proto_tree_set_uint(new_fi,
			    get_uint_value(tvb, start, length, little_endian));
			break;

		case FT_INT64:
		case FT_UINT64:
			g_assert(length == 8);
			proto_tree_set_uint64_tvb(new_fi, tvb, start, little_endian);
			break;

		/* XXX - make these just FT_INT? */
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
			proto_tree_set_int(new_fi,
			    get_int_value(tvb, start, length, little_endian));
			break;

		case FT_IPv4:
			g_assert(length == 4);
			tvb_memcpy(tvb, (guint8 *)&value, start, 4);
			proto_tree_set_ipv4(new_fi, value);
			break;

		case FT_IPXNET:
			g_assert(length == 4);
			proto_tree_set_ipxnet(new_fi,
			    get_uint_value(tvb, start, 4, FALSE));
			break;

		case FT_IPv6:
			g_assert(length == 16);
			proto_tree_set_ipv6_tvb(new_fi, tvb, start);
			break;

		case FT_ETHER:
			g_assert(length == 6);
			proto_tree_set_ether_tvb(new_fi, tvb, start);
			break;

		case FT_STRING:
			/* This g_strdup'ed memory is freed in proto_tree_free_node() */
			proto_tree_set_string_tvb(new_fi, tvb, start, length);
			break;

		case FT_STRINGZ:
			if (length != 0) {  /* XXX - Should we throw an exception instead? */
				/* Instead of calling proto_item_set_len(),
				 * since we don't yet have a proto_item, we
				 * set the field_info's length ourselves.
				 *
				 * XXX - our caller can't use that length to
				 * advance an offset unless they arrange that
				 * there always be a protocol tree into which
				 * we're putting this item.
				 */
				if (length == -1) {
					/* This can throw an exception */
					length = tvb_strsize(tvb, start);

					/* This g_malloc'ed memory is freed
					   in proto_tree_free_node() */
					string = g_malloc(length);

					tvb_memcpy(tvb, string, start, length);
					new_fi->length = length;
				}
				else {
					/* In this case, length signifies
					 * the length of the string.
					 *
					 * This could either be a null-padded
					 * string, which doesn't necessarily
					 * have a '\0' at the end, or a
					 * null-terminated string, with a
					 * trailing '\0'.  (Yes, there are
					 * cases where you have a string
					 * that's both counted and null-
					 * terminated.)
					 *
					 * In the first case, we must
					 * allocate a buffer of length
					 * "length+1", to make room for
					 * a trailing '\0'.
					 *
					 * In the second case, we don't
					 * assume that there is a trailing
					 * '\0' there, as the packet might
					 * be malformed.  (XXX - should we
					 * throw an exception if there's no
					 * trailing '\0'?)  Therefore, we
					 * allocate a buffer of length
					 * "length+1", and put in a trailing
					 * '\0', just to be safe.
					 *
					 * (XXX - this would change if
					 * we made string values counted
					 * rather than null-terminated.)
					 */

					/* This g_malloc'ed memory is freed
					 * in proto_tree_free_node() */
					string = tvb_get_string(tvb, start,
					    length);
					new_fi->length = length;
				}
				proto_tree_set_string(new_fi, string, TRUE);
			}
			break;

		case FT_UINT_STRING:
			/* This g_strdup'ed memory is freed in proto_tree_free_node() */
			n = get_uint_value(tvb, start, length, little_endian);
			proto_tree_set_string_tvb(new_fi, tvb, start + length, n);

			/* Instead of calling proto_item_set_len(), since we
			 * don't yet have a proto_item, we set the
			 * field_info's length ourselves.
			 *
			 * XXX - our caller can't use that length to
			 * advance an offset unless they arrange that
			 * there always be a protocol tree into which
			 * we're putting this item.
			 */
			new_fi->length = n + length;
			break;

		default:
			g_error("new_fi->hfinfo->type %d (%s) not handled\n",
					new_fi->hfinfo->type,
					ftype_name(new_fi->hfinfo->type));
			g_assert_not_reached();
			break;
	}

	/* Don't add new node to proto_tree until now so that any exceptions
	 * raised by a tvbuff access method doesn't leave junk in the proto_tree. */
	pi = proto_tree_add_node(tree, new_fi);

	CLEANUP_POP;

	/* If the proto_tree wants to keep a record of this finfo
	 * for quick lookup, then record it. */
	hash = PTREE_DATA(tree)->interesting_hfids;
	ptrs = g_hash_table_lookup(hash, GINT_TO_POINTER(hfindex));
	if (ptrs) {
		g_ptr_array_add(ptrs, new_fi);
	}

	return pi;
}

proto_item *
proto_tree_add_item_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb,
    gint start, gint length, gboolean little_endian)
{
	proto_item	*pi;
	field_info	*fi;

	pi = proto_tree_add_item(tree, hfindex, tvb, start, length, little_endian);
	if (pi == NULL)
		return(NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}


/* Add a FT_NONE to a proto_tree */
proto_item *
proto_tree_add_none_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_NONE);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	/* no value to set for FT_NONE */
	return pi;
}


static void
proto_tree_set_protocol_tvb(field_info *fi, tvbuff_t *tvb)
{
	fvalue_set(fi->value, tvb, TRUE);
}

/* Add a FT_PROTOCOL to a proto_tree */
proto_item *
proto_tree_add_protocol_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;
	header_field_info	*hfinfo;
	field_info		*new_fi;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_PROTOCOL);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	if (start == 0) {
		proto_tree_set_protocol_tvb(new_fi, tvb);
	}
	else {
		proto_tree_set_protocol_tvb(new_fi, NULL);
	}
	return pi;
}


/* Add a FT_BYTES to a proto_tree */
proto_item *
proto_tree_add_bytes(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const guint8 *start_ptr)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_BYTES);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_bytes(new_fi, start_ptr, length);

	return pi;
}

proto_item *
proto_tree_add_bytes_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const guint8 *start_ptr)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_bytes(tree, hfindex, tvb, start, length, start_ptr);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_bytes_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const guint8 *start_ptr, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_bytes(tree, hfindex, tvb, start, length, start_ptr);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

static void
proto_tree_set_bytes(field_info *fi, const guint8* start_ptr, gint length)
{
	GByteArray		*bytes;

	bytes = g_byte_array_new();
	if (length > 0) {
		g_byte_array_append(bytes, start_ptr, length);
	}
	fvalue_set(fi->value, bytes, TRUE);
}


static void
proto_tree_set_bytes_tvb(field_info *fi, tvbuff_t *tvb, gint offset, gint length)
{
	proto_tree_set_bytes(fi, tvb_get_ptr(tvb, offset, length), length);
}

/* Add a FT_*TIME to a proto_tree */
proto_item *
proto_tree_add_time(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		nstime_t *value_ptr)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_ABSOLUTE_TIME ||
				hfinfo->type == FT_RELATIVE_TIME);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_time(new_fi, value_ptr);

	return pi;
}

proto_item *
proto_tree_add_time_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		nstime_t *value_ptr)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_time(tree, hfindex, tvb, start, length, value_ptr);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_time_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		nstime_t *value_ptr, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_time(tree, hfindex, tvb, start, length, value_ptr);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_*TIME value */
static void
proto_tree_set_time(field_info *fi, nstime_t *value_ptr)
{
	fvalue_set(fi->value, value_ptr, FALSE);
}

/* Add a FT_IPXNET to a proto_tree */
proto_item *
proto_tree_add_ipxnet(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_IPXNET);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_ipxnet(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_ipxnet_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_ipxnet(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_ipxnet_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_ipxnet(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_IPXNET value */
static void
proto_tree_set_ipxnet(field_info *fi, guint32 value)
{
	fvalue_set_integer(fi->value, value);
}

/* Add a FT_IPv4 to a proto_tree */
proto_item *
proto_tree_add_ipv4(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_IPv4);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_ipv4(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_ipv4_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_ipv4(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_ipv4_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_ipv4(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_IPv4 value */
static void
proto_tree_set_ipv4(field_info *fi, guint32 value)
{
	fvalue_set_integer(fi->value, value);
}

/* Add a FT_IPv6 to a proto_tree */
proto_item *
proto_tree_add_ipv6(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value_ptr)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_IPv6);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_ipv6(new_fi, value_ptr);

	return pi;
}

proto_item *
proto_tree_add_ipv6_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value_ptr)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_ipv6(tree, hfindex, tvb, start, length, value_ptr);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_ipv6_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value_ptr, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_ipv6(tree, hfindex, tvb, start, length, value_ptr);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_IPv6 value */
static void
proto_tree_set_ipv6(field_info *fi, const guint8* value_ptr)
{
	fvalue_set(fi->value, (gpointer) value_ptr, FALSE);
}

static void
proto_tree_set_ipv6_tvb(field_info *fi, tvbuff_t *tvb, gint start)
{
	proto_tree_set_ipv6(fi, tvb_get_ptr(tvb, start, 16));
}

static void
proto_tree_set_uint64(field_info *fi, const guint8 *value_ptr, gboolean little_endian)
{
	if(little_endian){
		unsigned char buffer[8];
		int i;

		for(i=0;i<8;i++){
			buffer[i]=value_ptr[7-i];
		}
		fvalue_set(fi->value, (gpointer)buffer, FALSE);
	} else {
		fvalue_set(fi->value, (gpointer)value_ptr, FALSE);
	}
}

static void
proto_tree_set_uint64_tvb(field_info *fi, tvbuff_t *tvb, gint start, gboolean little_endian)
{
	proto_tree_set_uint64(fi, tvb_get_ptr(tvb, start, 8), little_endian);
}

/* Add a FT_STRING or FT_STRINGZ to a proto_tree. Creates own copy of string,
 * and frees it when the proto_tree is destroyed. */
proto_item *
proto_tree_add_string(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const char* value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_STRING || hfinfo->type == FT_STRINGZ);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_string(new_fi, value, FALSE);

	return pi;
}

proto_item *
proto_tree_add_string_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const char* value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_string(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_string_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
		gint length, const char* value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_string(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Appends string data to a FT_STRING or FT_STRINGZ, allowing progressive
 * field info update instead of only updating the representation as does
 * proto_item_append_text()
 */
void
proto_item_append_string(proto_item *pi, const char *str)
{
	field_info *fi;
	header_field_info *hfinfo;
	gchar *old_str, *new_str;

	if (!pi)
		return;
	if (!*str)
		return;

	fi = PITEM_FINFO(pi);
	hfinfo = fi->hfinfo;
	g_assert(hfinfo->type == FT_STRING || hfinfo->type == FT_STRINGZ);
	old_str = fvalue_get(fi->value);
	new_str = g_malloc(strlen(old_str) + strlen(str) + 1);
	sprintf(new_str, "%s%s", old_str, str);
	fvalue_set(fi->value, new_str, TRUE);
}

/* Set the FT_STRING value */
static void
proto_tree_set_string(field_info *fi, const char* value,
		gboolean already_allocated)
{
	fvalue_set(fi->value, (gpointer) value, already_allocated);
}

static void
proto_tree_set_string_tvb(field_info *fi, tvbuff_t *tvb, gint start, gint length)
{
	gchar	*string;

	if (length == -1) {
		length = tvb_ensure_length_remaining(tvb, start);
	}

	/* This memory is freed in proto_tree_free_node() */
	string = g_malloc(length + 1);
	tvb_memcpy(tvb, string, start, length);
	string[length] = '\0';
	proto_tree_set_string(fi, string, TRUE);
}

/* Add a FT_ETHER to a proto_tree */
proto_item *
proto_tree_add_ether(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_ETHER);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_ether(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_ether_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_ether(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_ether_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		const guint8* value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_ether(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_ETHER value */
static void
proto_tree_set_ether(field_info *fi, const guint8* value)
{
	fvalue_set(fi->value, (gpointer) value, FALSE);
}

static void
proto_tree_set_ether_tvb(field_info *fi, tvbuff_t *tvb, gint start)
{
	proto_tree_set_ether(fi, tvb_get_ptr(tvb, start, 6));
}

/* Add a FT_BOOLEAN to a proto_tree */
proto_item *
proto_tree_add_boolean(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_BOOLEAN);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_boolean(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_boolean_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_boolean(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_boolean_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_boolean(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_BOOLEAN value */
static void
proto_tree_set_boolean(field_info *fi, guint32 value)
{
	proto_tree_set_uint(fi, value);
}

/* Add a FT_FLOAT to a proto_tree */
proto_item *
proto_tree_add_float(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		float value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_FLOAT);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_float(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_float_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		float value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_float(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_float_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		float value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_float(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_FLOAT value */
static void
proto_tree_set_float(field_info *fi, float value)
{
	fvalue_set_floating(fi->value, value);
}

/* Add a FT_DOUBLE to a proto_tree */
proto_item *
proto_tree_add_double(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		double value)
{
	proto_item		*pi;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo->type == FT_DOUBLE);

	pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length, &new_fi);
	proto_tree_set_double(new_fi, value);

	return pi;
}

proto_item *
proto_tree_add_double_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		double value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_double(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_double_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		double value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_double(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_DOUBLE value */
static void
proto_tree_set_double(field_info *fi, double value)
{
	fvalue_set_floating(fi->value, value);
}

/* Add any FT_UINT* to a proto_tree */
proto_item *
proto_tree_add_uint(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi = NULL;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	switch(hfinfo->type) {
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_FRAMENUM:
			pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length,
					&new_fi);
			proto_tree_set_uint(new_fi, value);
			break;

		default:
			g_assert_not_reached();
	}

	return pi;
}

proto_item *
proto_tree_add_uint_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_uint(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_uint_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		guint32 value, const char *format, ...)
{
	proto_item		*pi;
	va_list			ap;

	pi = proto_tree_add_uint(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_UINT* value */
static void
proto_tree_set_uint(field_info *fi, guint32 value)
{
	header_field_info	*hfinfo;
	guint32			integer;

	hfinfo = fi->hfinfo;
	integer = value;

	if (hfinfo->bitmask) {
		/* Mask out irrelevant portions */
		integer &= hfinfo->bitmask;

		/* Shift bits */
		if (hfinfo->bitshift > 0) {
			integer >>= hfinfo->bitshift;
		}
	}
	fvalue_set_integer(fi->value, integer);
}

/* Add any FT_INT* to a proto_tree */
proto_item *
proto_tree_add_int(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		gint32 value)
{
	proto_item		*pi = NULL;
	field_info		*new_fi;
	header_field_info	*hfinfo;

	if (!tree)
		return (NULL);

	hfinfo = proto_registrar_get_nth(hfindex);
	switch(hfinfo->type) {
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
			pi = proto_tree_add_pi(tree, hfindex, tvb, start, &length,
					&new_fi);
			proto_tree_set_int(new_fi, value);
			break;

		default:
			g_assert_not_reached();
	}

	return pi;
}

proto_item *
proto_tree_add_int_hidden(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		gint32 value)
{
	proto_item		*pi;
	field_info 		*fi;

	pi = proto_tree_add_int(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	fi = PITEM_FINFO(pi);
	fi->visible = FALSE;

	return pi;
}

proto_item *
proto_tree_add_int_format(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start, gint length,
		gint32 value, const char *format, ...)
{
	proto_item		*pi = NULL;
	va_list			ap;

	pi = proto_tree_add_int(tree, hfindex, tvb, start, length, value);
	if (pi == NULL)
		return (NULL);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);

	return pi;
}

/* Set the FT_INT* value */
static void
proto_tree_set_int(field_info *fi, gint32 value)
{
	header_field_info	*hfinfo;
	guint32			integer;

	hfinfo = fi->hfinfo;
	integer = (guint32) value;

	if (hfinfo->bitmask) {
		/* Mask out irrelevant portions */
		integer &= hfinfo->bitmask;

		/* Shift bits */
		if (hfinfo->bitshift > 0) {
			integer >>= hfinfo->bitshift;
		}
	}
	fvalue_set_integer(fi->value, integer);
}


/* Add a field_info struct to the proto_tree, encapsulating it in a GNode (proto_item) */
static proto_item *
proto_tree_add_node(proto_tree *tree, field_info *fi)
{
	GNode *new_gnode;
	proto_node *pnode, *tnode;
	field_info *tfi;

	/*
	 * Make sure "tree" is ready to have subtrees under it, by
	 * checking whether it's been given an ett_ value.
	 *
	 * "tnode->finfo" may be null; that's the case for the root
	 * node of the protocol tree.  That node is not displayed,
	 * so it doesn't need an ett_ value to remember whether it
	 * was expanded.
	 */
	tnode = GNODE_PNODE(tree);
	tfi = tnode->finfo;
	g_assert(tfi == NULL ||
	    (tfi->tree_type >= 0 && tfi->tree_type < num_tree_types));

	pnode = g_mem_chunk_alloc(gmc_proto_node);
	pnode->finfo = fi;
	pnode->tree_data = PTREE_DATA(tree);

	new_gnode = g_node_new(pnode);
	g_node_append((GNode*)tree, new_gnode);

	return (proto_item*) new_gnode;
}


/* Generic way to allocate field_info and add to proto_tree.
 * Sets *pfi to address of newly-allocated field_info struct, if pfi is
 * non-NULL. */
static proto_item *
proto_tree_add_pi(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
    gint *length, field_info **pfi)
{
	proto_item	*pi;
	field_info	*fi;
	GHashTable	*hash;
	GPtrArray	*ptrs;

	if (!tree)
		return(NULL);

	fi = alloc_field_info(tree, hfindex, tvb, start, length);
	pi = proto_tree_add_node(tree, fi);

	/* If the proto_tree wants to keep a record of this finfo
	 * for quick lookup, then record it. */
	hash = PTREE_DATA(tree)->interesting_hfids;
	ptrs = g_hash_table_lookup(hash, GINT_TO_POINTER(hfindex));
	if (ptrs) {
		g_ptr_array_add(ptrs, fi);
	}

	/* Does the caller want to know the fi pointer? */
	if (pfi) {
		*pfi = fi;
	}

	return pi;
}

static field_info *
alloc_field_info(proto_tree *tree, int hfindex, tvbuff_t *tvb, gint start,
    gint *length)
{
	header_field_info	*hfinfo;
	field_info		*fi;

	/*
	 * We only allow a null tvbuff if the item has a zero length,
	 * i.e. if there's no data backing it.
	 */
	g_assert(tvb != NULL || *length == 0);

	g_assert(hfindex >= 0 && (guint) hfindex < gpa_hfinfo->len);
	hfinfo = proto_registrar_get_nth(hfindex);
	g_assert(hfinfo != NULL);

	if (*length == -1) {
		/*
		 * For FT_NONE, FT_PROTOCOL, FT_BYTES, and FT_STRING fields,
		 * a length of -1 means "set the length to what remains in
		 * the tvbuff".
		 *
		 * The assumption is either that
		 *
		 *	1) the length of the item can only be determined
		 *	   by dissection (typically true of items with
		 *	   subitems, which are probably FT_NONE or
		 *	   FT_PROTOCOL)
		 *
		 * or
		 *
		 *	2) if the tvbuff is "short" (either due to a short
		 *	   snapshot length or due to lack of reassembly of
		 *	   fragments/segments/whatever), we want to display
		 *	   what's available in the field (probably FT_BYTES
		 *	   or FT_STRING) and then throw an exception later
		 *
		 * or
		 *
		 *	3) the field is defined to be "what's left in the
		 *	   packet"
		 *
		 * so we set the length to what remains in the tvbuff so
		 * that, if we throw an exception while dissecting, it
		 * has what is probably the right value.
		 *
		 * For FT_STRINGZ, it means "the string is null-terminated,
		 * not null-padded; set the length to the actual length
		 * of the string", and if the tvbuff if short, we just
		 * throw an exception.
		 *
		 * It's not valid for any other type of field.
		 */
		switch (hfinfo->type) {

		case FT_PROTOCOL:
		case FT_NONE:
		case FT_BYTES:
		case FT_STRING:
			*length = tvb_ensure_length_remaining(tvb, start);
			break;

		case FT_STRINGZ:
			/*
			 * Leave the length as -1, so our caller knows
			 * it was -1.
			 */
			break;

		default:
			g_assert_not_reached();
		}
	}

	fi = g_mem_chunk_alloc(gmc_field_info);
	fi->hfinfo = hfinfo;
	fi->start = start;
	if (tvb) {
		fi->start += tvb_raw_offset(tvb);
	}
	fi->length = *length;
	fi->tree_type = -1;
	fi->visible = PTREE_DATA(tree)->visible;
	fi->representation = NULL;

	fi->value = fvalue_new(fi->hfinfo->type);

	/* add the data source tvbuff */
	if (tvb) {
		fi->ds_tvb = tvb_get_ds_tvb(tvb);
	} else {
		fi->ds_tvb = NULL;
	}

	return fi;
}

/* Set representation of a proto_tree entry, if the protocol tree is to
   be visible. */
static void
proto_tree_set_representation(proto_item *pi, const char *format, va_list ap)
{
	int					ret;	/*tmp return value */
	field_info *fi = PITEM_FINFO(pi);

	if (fi->visible) {
		fi->representation = g_mem_chunk_alloc(gmc_item_labels);
		ret = vsnprintf(fi->representation, ITEM_LABEL_LENGTH, format, ap);
		if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
			fi->representation[ITEM_LABEL_LENGTH - 1] = '\0';
	}
}

/* Set text of proto_item after having already been created. */
void
proto_item_set_text(proto_item *pi, const char *format, ...)
{
	field_info *fi = NULL;
	va_list	ap;

	if (pi==NULL) {
		return;
	}

	fi = PITEM_FINFO(pi);

	if (fi->representation)
		g_mem_chunk_free(gmc_item_labels, fi->representation);

	va_start(ap, format);
	proto_tree_set_representation(pi, format, ap);
	va_end(ap);
}

/* Append to text of proto_item after having already been created. */
void
proto_item_append_text(proto_item *pi, const char *format, ...)
{
	field_info *fi = NULL;
	size_t curlen;
	va_list	ap;
	int					ret;	/*tmp return value */

	if (pi==NULL) {
		return;
	}

	fi = PITEM_FINFO(pi);

	if (fi->visible) {
		va_start(ap, format);

		/*
		 * If we don't already have a representation,
		 * generate the default representation.
		 */
		if (fi->representation == NULL) {
			fi->representation = g_mem_chunk_alloc(gmc_item_labels);
			proto_item_fill_label(fi, fi->representation);
		}

		curlen = strlen(fi->representation);
		if (ITEM_LABEL_LENGTH > curlen) {
			ret = vsnprintf(fi->representation + curlen,
			    ITEM_LABEL_LENGTH - curlen, format, ap);
			if ((ret == -1) || (ret >= (int)(ITEM_LABEL_LENGTH - curlen)))
				fi->representation[ITEM_LABEL_LENGTH - 1] = '\0';
		}
		va_end(ap);
	}
}

void
proto_item_set_len(proto_item *pi, gint length)
{
	field_info *fi;

	if (pi == NULL)
		return;
	fi = PITEM_FINFO(pi);
	fi->length = length;
}

/*
 * Sets the length of the item based on its start and on the specified
 * offset, which is the offset past the end of the item; as the start
 * in the item is relative to the beginning of the data source tvbuff,
 * we need to pass in a tvbuff - the end offset is relative to the beginning
 * of that tvbuff.
 */
void
proto_item_set_end(proto_item *pi, tvbuff_t *tvb, gint end)
{
	field_info *fi;

	if (pi == NULL)
		return;
	fi = PITEM_FINFO(pi);
	end += tvb_raw_offset(tvb);
	fi->length = end - fi->start;
}

int
proto_item_get_len(proto_item *pi)
{
	field_info *fi = PITEM_FINFO(pi);
	return fi->length;
}

proto_tree*
proto_tree_create_root(void)
{
	proto_node  *pnode;

	/* Initialize the proto_node */
	pnode = g_mem_chunk_alloc(gmc_proto_node);
	pnode->finfo = NULL;
	pnode->tree_data = g_new(tree_data_t, 1);

	/* Initialize the tree_data_t */
	pnode->tree_data->interesting_hfids =
	    g_hash_table_new(g_direct_hash, g_direct_equal);

	/* Set the default to FALSE so it's easier to
	 * find errors; if we expect to see the protocol tree
	 * but for some reason the default 'visible' is not
	 * changed, then we'll find out very quickly. */
	pnode->tree_data->visible = FALSE;

	return (proto_tree*) g_node_new(pnode);
}


/* "prime" a proto_tree with a single hfid that a dfilter
 * is interested in. */
void
proto_tree_prime_hfid(proto_tree *tree, gint hfid)
{
	g_hash_table_insert(PTREE_DATA(tree)->interesting_hfids,
		GINT_TO_POINTER(hfid), g_ptr_array_new());
}


proto_tree*
proto_item_add_subtree(proto_item *pi,  gint idx) {
	field_info *fi;

	if (!pi)
		return(NULL);

	fi = PITEM_FINFO(pi);
	g_assert(idx >= 0 && idx < num_tree_types);
	fi->tree_type = idx;
	return (proto_tree*) pi;
}

static gint
proto_match_short_name(gconstpointer p_arg, gconstpointer name_arg)
{
	const protocol_t *p = p_arg;
	const char *name = name_arg;

	return g_strcasecmp(p->short_name, name);
}

static gint
proto_match_name(gconstpointer p_arg, gconstpointer name_arg)
{
	const protocol_t *p = p_arg;
	const char *name = name_arg;

	return g_strcasecmp(p->name, name);
}

static gint
proto_match_filter_name(gconstpointer p_arg, gconstpointer name_arg)
{
	const protocol_t *p = p_arg;
	const char *name = name_arg;

	return g_strcasecmp(p->filter_name, name);
}

static gint
proto_compare_name(gconstpointer p1_arg, gconstpointer p2_arg)
{
	const protocol_t *p1 = p1_arg;
	const protocol_t *p2 = p2_arg;

	return g_strcasecmp(p1->short_name, p2->short_name);
}

int
proto_register_protocol(char *name, char *short_name, char *filter_name)
{
	protocol_t *protocol;
	header_field_info *hfinfo;
	int proto_id;

	/*
	 * Make sure there's not already a protocol with any of those
	 * names.  Crash if there is, as that's an error in the code,
	 * and the code has to be fixed not to register more than one
	 * protocol with the same name.
	 */
	g_assert(g_list_find_custom(protocols, name, proto_match_name) == NULL);
	g_assert(g_list_find_custom(protocols, short_name, proto_match_short_name) == NULL);
	g_assert(g_list_find_custom(protocols, filter_name, proto_match_filter_name) == NULL);

	/* Add this protocol to the list of known protocols; the list
	   is sorted by protocol short name. */
	protocol = g_malloc(sizeof (protocol_t));
	protocol->name = name;
	protocol->short_name = short_name;
	protocol->filter_name = filter_name;
	protocol->fields = NULL;
	protocol->is_enabled = TRUE; /* protocol is enabled by default */
	protocol->can_disable = TRUE;
	protocols = g_list_insert_sorted(protocols, protocol,
	    proto_compare_name);

	/* Here we do allocate a new header_field_info struct */
	hfinfo = g_mem_chunk_alloc(gmc_hfinfo);
	hfinfo->name = name;
	hfinfo->abbrev = filter_name;
	hfinfo->type = FT_PROTOCOL;
	hfinfo->strings = NULL;
	hfinfo->bitmask = 0;
	hfinfo->bitshift = 0;
	hfinfo->blurb = "";
	hfinfo->parent = -1; /* this field differentiates protos and fields */

	proto_id = proto_register_field_init(hfinfo, hfinfo->parent);
	protocol->proto_id = proto_id;
	return proto_id;
}

/*
 * Routines to use to iterate over the protocols.
 * The argument passed to the iterator routines is an opaque cookie to
 * their callers; it's the GList pointer for the current element in
 * the list.
 * The ID of the protocol is returned, or -1 if there is no protocol.
 */
int
proto_get_first_protocol(void **cookie)
{
	protocol_t *protocol;

	if (protocols == NULL)
		return -1;
	*cookie = protocols;
	protocol = protocols->data;
	return protocol->proto_id;
}

int
proto_get_next_protocol(void **cookie)
{
	GList *list_item = *cookie;
	protocol_t *protocol;

	list_item = g_list_next(list_item);
	if (list_item == NULL)
		return -1;
	*cookie = list_item;
	protocol = list_item->data;
	return protocol->proto_id;
}

header_field_info *
proto_get_first_protocol_field(int proto_id, void **cookie)
{
	protocol_t *protocol = find_protocol_by_id(proto_id);
	hf_register_info *ptr;

	if ((protocol == NULL) || (protocol->fields == NULL))
		return NULL;

	*cookie = protocol->fields;
	ptr = protocol->fields->data;
	return &ptr->hfinfo;
}

header_field_info *
proto_get_next_protocol_field(void **cookie)
{
	GList *list_item = *cookie;
	hf_register_info *ptr;

	list_item = g_list_next(list_item);
	if (list_item == NULL)
		return NULL;

	*cookie = list_item;
	ptr = list_item->data;
	return &ptr->hfinfo;
}

/*
 * Find the protocol list entry for a protocol given its field ID.
 */
static gint
compare_proto_id(gconstpointer proto_arg, gconstpointer id_arg)
{
	const protocol_t *protocol = proto_arg;
	const int *id_ptr = id_arg;

	return (protocol->proto_id == *id_ptr) ? 0 : 1;
}

static protocol_t *
find_protocol_by_id(int proto_id)
{
	GList *list_entry;

	list_entry = g_list_find_custom(protocols, &proto_id, compare_proto_id);
	if (list_entry == NULL)
		return NULL;
	return list_entry->data;
}

static gint compare_filter_name(gconstpointer proto_arg,
				gconstpointer filter_name)
{
	const protocol_t *protocol = proto_arg;
	const gchar* f_name = filter_name;

	return (strcmp(protocol->filter_name, f_name));
}

int proto_get_id_by_filter_name(gchar* filter_name)
{
	GList *list_entry;
	protocol_t *protocol;

	list_entry = g_list_find_custom(protocols, filter_name,
	    compare_filter_name);
	if (list_entry == NULL)
		return -1;
	protocol = list_entry->data;
	return protocol->proto_id;
}

char *
proto_get_protocol_name(int proto_id)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	return protocol->name;
}

char *
proto_get_protocol_short_name(int proto_id)
{
	protocol_t *protocol;

	if (proto_id == -1)
		return "(none)";
	protocol = find_protocol_by_id(proto_id);
	return protocol->short_name;
}

char *
proto_get_protocol_filter_name(int proto_id)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	return protocol->filter_name;
}

gboolean
proto_is_protocol_enabled(int proto_id)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	return protocol->is_enabled;
}

gboolean
proto_can_disable_protocol(int proto_id)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	return protocol->can_disable;
}

void
proto_set_decoding(int proto_id, gboolean enabled)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	g_assert(enabled || protocol->can_disable);
	protocol->is_enabled = enabled;
}

void
proto_set_cant_disable(int proto_id)
{
	protocol_t *protocol;

	protocol = find_protocol_by_id(proto_id);
	protocol->can_disable = FALSE;
}

/* for use with static arrays only, since we don't allocate our own copies
of the header_field_info struct contained within the hf_register_info struct */
void
proto_register_field_array(int parent, hf_register_info *hf, int num_records)
{
	int			field_id, i;
	hf_register_info	*ptr = hf;
	protocol_t		*proto;

	proto = find_protocol_by_id(parent);
	for (i = 0; i < num_records; i++, ptr++) {
		/*
		 * Make sure we haven't registed this yet.
		 * Most fields have variables associated with them
		 * that are initialized to -1; some have array elements,
		 * or possibly uninitialized variables, so we also allow
		 * 0 (which is unlikely to be the field ID we get back
		 * from "proto_register_field_init()").
		 */
		g_assert(*ptr->p_id == -1 || *ptr->p_id == 0);

		if (proto != NULL) {
			if (proto->fields == NULL) {
				proto->fields = g_list_append(NULL, ptr);
				proto->last_field = proto->fields;
			} else {
				proto->last_field =
				    g_list_append(proto->last_field, ptr)->next;
			}
		}
		field_id = proto_register_field_init(&ptr->hfinfo, parent);
		*ptr->p_id = field_id;
	}
}

static int
proto_register_field_init(header_field_info *hfinfo, int parent)
{
	/* The field must have names */
	g_assert(hfinfo->name);
	g_assert(hfinfo->abbrev);

	/* These types of fields are allowed to have value_strings or true_false_strings */
	g_assert((hfinfo->strings == NULL) || (
			(hfinfo->type == FT_UINT8) ||
			(hfinfo->type == FT_UINT16) ||
			(hfinfo->type == FT_UINT24) ||
			(hfinfo->type == FT_UINT32) ||
			(hfinfo->type == FT_INT8) ||
			(hfinfo->type == FT_INT16) ||
			(hfinfo->type == FT_INT24) ||
			(hfinfo->type == FT_INT32) ||
			(hfinfo->type == FT_BOOLEAN) ||
			(hfinfo->type == FT_FRAMENUM) ));

	switch (hfinfo->type) {

	case FT_UINT8:
	case FT_UINT16:
	case FT_UINT24:
	case FT_UINT32:
	case FT_INT8:
	case FT_INT16:
	case FT_INT24:
	case FT_INT32:
		/* Require integral types (other than frame number, which is
		   always displayed in decimal) to have a number base */
		g_assert(hfinfo->display != BASE_NONE);
		break;

	case FT_FRAMENUM:
		/* Don't allow bitfields or value strings for frame numbers */
		g_assert(hfinfo->bitmask == 0);
		g_assert(hfinfo->strings == NULL);
		break;

	default:
		break;
	}
	/* if this is a bitfield, compute bitshift */
	if (hfinfo->bitmask) {
		while ((hfinfo->bitmask & (1 << hfinfo->bitshift)) == 0)
			hfinfo->bitshift++;
	}

	hfinfo->parent = parent;
	hfinfo->same_name_next = NULL;
	hfinfo->same_name_prev = NULL;

	/* if we always add and never delete, then id == len - 1 is correct */
	g_ptr_array_add(gpa_hfinfo, hfinfo);
	hfinfo->id = gpa_hfinfo->len - 1;

	/* if we have real names, enter this field in the name tree */
	if ((hfinfo->name[0] != 0) && (hfinfo->abbrev[0] != 0 )) {

		header_field_info *same_name_hfinfo, *same_name_next_hfinfo;

		/* We allow multiple hfinfo's to be registered under the same
		 * abbreviation. This was done for X.25, as, depending
		 * on whether it's modulo-8 or modulo-128 operation,
		 * some bitfield fields may be in different bits of
		 * a byte, and we want to be able to refer to that field
		 * with one name regardless of whether the packets
		 * are modulo-8 or modulo-128 packets. */
		same_name_hfinfo = g_tree_lookup(gpa_name_tree, hfinfo->abbrev);
		if (same_name_hfinfo) {
			/* There's already a field with this name.
			 * Put it after that field in the list of
			 * fields with this name, then allow the code
			 * after this if{} block to replace the old
			 * hfinfo with the new hfinfo in the GTree. Thus,
			 * we end up with a linked-list of same-named hfinfo's,
			 * with the root of the list being the hfinfo in the GTree */
			same_name_next_hfinfo =
			    same_name_hfinfo->same_name_next;

			hfinfo->same_name_next = same_name_next_hfinfo;
			if (same_name_next_hfinfo)
				same_name_next_hfinfo->same_name_prev = hfinfo;

			same_name_hfinfo->same_name_next = hfinfo;
			hfinfo->same_name_prev = same_name_hfinfo;
		}
		g_tree_insert(gpa_name_tree, hfinfo->abbrev, hfinfo);
	}

	return hfinfo->id;
}

void
proto_register_subtree_array(gint **indices, int num_indices)
{
	int	i;
	gint	**ptr = indices;

	/*
	 * Make sure we haven't already allocated the array of "tree is
	 * expanded" flags.
	 *
	 * XXX - if it's *really* important to allow more ett_ values to
	 * be given out after "proto_init()" is called, we could expand
	 * the array.
	 */
	g_assert(tree_is_expanded == NULL);

	/*
	 * Assign "num_indices" subtree numbers starting at "num_tree_types",
	 * returning the indices through the pointers in the array whose
	 * first element is pointed to by "indices", and update
	 * "num_tree_types" appropriately.
	 */
	for (i = 0; i < num_indices; i++, ptr++, num_tree_types++)
		**ptr = num_tree_types;
}

void
proto_item_fill_label(field_info *fi, gchar *label_str)
{
	header_field_info		*hfinfo = fi->hfinfo;

	guint8				*bytes;
	guint32				integer;
	ipv4_addr			*ipv4;
	guint32				n_addr; /* network-order IPv4 address */
	int					ret;	/*tmp return value */

	switch(hfinfo->type) {
		case FT_NONE:
		case FT_PROTOCOL:
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s", hfinfo->name);
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_BOOLEAN:
			fill_label_boolean(fi, label_str);
			break;

		case FT_BYTES:
		case FT_UINT_BYTES:
			bytes = fvalue_get(fi->value);
			if (bytes) {
				ret = snprintf(label_str, ITEM_LABEL_LENGTH,
					"%s: %s", hfinfo->name,
					 bytes_to_str(bytes, fvalue_length(fi->value)));
				if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
					label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			}
			else {
				ret = snprintf(label_str, ITEM_LABEL_LENGTH,
					"%s: <MISSING>", hfinfo->name);
				if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
					label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			}
			break;

		/* Four types of integers to take care of:
		 * 	Bitfield, with val_string
		 * 	Bitfield, w/o val_string
		 * 	Non-bitfield, with val_string
		 * 	Non-bitfield, w/o val_string
		 */
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_FRAMENUM:
			if (hfinfo->bitmask) {
				if (hfinfo->strings) {
					fill_label_enumerated_bitfield(fi, label_str);
				}
				else {
					fill_label_numeric_bitfield(fi, label_str);
				}
			}
			else {
				if (hfinfo->strings) {
					fill_label_enumerated_uint(fi, label_str);
				}
				else {
					fill_label_uint(fi, label_str);
				}
			}
			break;

		case FT_UINT64:
			fill_label_uint64(fi, label_str);
			break;

		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
			g_assert(!hfinfo->bitmask);
			if (hfinfo->strings) {
				fill_label_enumerated_int(fi, label_str);
			}
			else {
				fill_label_int(fi, label_str);
			}
			break;

		case FT_INT64:
			fill_label_int64(fi, label_str);
			break;

		case FT_FLOAT:
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %." STRINGIFY(FLT_DIG) "f",
				hfinfo->name, fvalue_get_floating(fi->value));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_DOUBLE:
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %." STRINGIFY(DBL_DIG) "g",
				hfinfo->name, fvalue_get_floating(fi->value));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_ABSOLUTE_TIME:
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s", hfinfo->name,
				abs_time_to_str(fvalue_get(fi->value)));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_RELATIVE_TIME:
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s seconds", hfinfo->name,
				rel_time_to_secs_str(fvalue_get(fi->value)));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_IPXNET:
			integer = fvalue_get_integer(fi->value);
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: 0x%08X (%s)", hfinfo->name,
				integer, get_ipxnet_name(integer));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_ETHER:
			bytes = fvalue_get(fi->value);
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s (%s)", hfinfo->name,
				ether_to_str(bytes),
				get_ether_name(bytes));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_IPv4:
			ipv4 = fvalue_get(fi->value);
			n_addr = ipv4_get_net_order_addr(ipv4);
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s (%s)", hfinfo->name,
				get_hostname(n_addr),
				ip_to_str((guint8*)&n_addr));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_IPv6:
			bytes = fvalue_get(fi->value);
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s (%s)", hfinfo->name,
				get_hostname6((struct e_in6_addr *)bytes),
				ip6_to_str((struct e_in6_addr*)bytes));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		case FT_STRING:
		case FT_STRINGZ:
		case FT_UINT_STRING:
			bytes = fvalue_get(fi->value);
			ret = snprintf(label_str, ITEM_LABEL_LENGTH,
				"%s: %s", hfinfo->name,
				format_text(bytes, strlen(bytes)));
			if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
				label_str[ITEM_LABEL_LENGTH - 1] = '\0';
			break;

		default:
			g_error("hfinfo->type %d (%s) not handled\n",
					hfinfo->type,
					ftype_name(hfinfo->type));
			g_assert_not_reached();
			break;
	}
}

static void
fill_label_uint64(field_info *fi, gchar *label_str)
{
	unsigned char *bytes;
	header_field_info *hfinfo = fi->hfinfo;
	int					ret;	/*tmp return value */

	bytes=fvalue_get(fi->value);
	switch(hfinfo->display){
	case BASE_DEC:
		ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			"%s: %s", hfinfo->name,
			u64toa(bytes));
		if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
			label_str[ITEM_LABEL_LENGTH - 1] = '\0';
		break;
	case BASE_HEX:
		ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			"%s: %s", hfinfo->name,
			u64toh(bytes));
		if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
			label_str[ITEM_LABEL_LENGTH - 1] = '\0';
		break;
	default:
		g_assert_not_reached();
		;
	}
}

static void
fill_label_int64(field_info *fi, gchar *label_str)
{
	unsigned char *bytes;
	header_field_info *hfinfo = fi->hfinfo;
	int					ret;	/*tmp return value */

	bytes=fvalue_get(fi->value);
	switch(hfinfo->display){
	case BASE_DEC:
		ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			"%s: %s", hfinfo->name,
			i64toa(bytes));
		if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
			label_str[ITEM_LABEL_LENGTH - 1] = '\0';
		break;
	case BASE_HEX:
		ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			"%s: %s", hfinfo->name,
			u64toh(bytes));
		if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
			label_str[ITEM_LABEL_LENGTH - 1] = '\0';
		break;
	default:
		g_assert_not_reached();
		;
	}
}

static void
fill_label_boolean(field_info *fi, gchar *label_str)
{
	char	*p = label_str;
	int	bitfield_byte_length = 0, bitwidth;
	guint32	unshifted_value;
	guint32	value;
	int					ret;	/*tmp return value */

	header_field_info		*hfinfo = fi->hfinfo;
	static const true_false_string	default_tf = { "True", "False" };
	const true_false_string		*tfstring = &default_tf;

	if (hfinfo->strings) {
		tfstring = (const struct true_false_string*) hfinfo->strings;
	}

	value = fvalue_get_integer(fi->value);
	if (hfinfo->bitmask) {
		/* Figure out the bit width */
		bitwidth = hfinfo_bitwidth(hfinfo);

		/* Un-shift bits */
		unshifted_value = value;
		if (hfinfo->bitshift > 0) {
			unshifted_value <<= hfinfo->bitshift;
		}

		/* Create the bitfield first */
		p = decode_bitfield_value(label_str, unshifted_value, hfinfo->bitmask, bitwidth);
		bitfield_byte_length = p - label_str;
	}

	/* Fill in the textual info */
	ret = snprintf(p, ITEM_LABEL_LENGTH - bitfield_byte_length,
		"%s: %s",  hfinfo->name,
		value ? tfstring->true_string : tfstring->false_string);
	if ((ret == -1) || (ret >= (ITEM_LABEL_LENGTH - bitfield_byte_length)))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}


/* Fills data for bitfield ints with val_strings */
static void
fill_label_enumerated_bitfield(field_info *fi, gchar *label_str)
{
	char *format = NULL, *p;
	int bitfield_byte_length, bitwidth;
	guint32 unshifted_value;
	guint32 value;
	int					ret;	/*tmp return value */

	header_field_info	*hfinfo = fi->hfinfo;

	/* Figure out the bit width */
	bitwidth = hfinfo_bitwidth(hfinfo);

	/* Pick the proper format string */
	format = hfinfo_uint_vals_format(hfinfo);

	/* Un-shift bits */
	unshifted_value = fvalue_get_integer(fi->value);
	value = unshifted_value;
	if (hfinfo->bitshift > 0) {
		unshifted_value <<= hfinfo->bitshift;
	}

	/* Create the bitfield first */
	p = decode_bitfield_value(label_str, unshifted_value, hfinfo->bitmask, bitwidth);
	bitfield_byte_length = p - label_str;

	/* Fill in the textual info using stored (shifted) value */
	ret = snprintf(p, ITEM_LABEL_LENGTH - bitfield_byte_length,
			format,  hfinfo->name,
			val_to_str(value, cVALS(hfinfo->strings), "Unknown"), value);
	if ((ret == -1) || (ret >= (ITEM_LABEL_LENGTH - bitfield_byte_length)))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}

static void
fill_label_numeric_bitfield(field_info *fi, gchar *label_str)
{
	char *format = NULL, *p;
	int bitfield_byte_length, bitwidth;
	guint32 unshifted_value;
	guint32 value;
	int					ret;	/*tmp return value */

	header_field_info	*hfinfo = fi->hfinfo;

	/* Figure out the bit width */
	bitwidth = hfinfo_bitwidth(hfinfo);

	/* Pick the proper format string */
	format = hfinfo_uint_format(hfinfo);

	/* Un-shift bits */
	unshifted_value = fvalue_get_integer(fi->value);
	value = unshifted_value;
	if (hfinfo->bitshift > 0) {
		unshifted_value <<= hfinfo->bitshift;
	}

	/* Create the bitfield using */
	p = decode_bitfield_value(label_str, unshifted_value, hfinfo->bitmask, bitwidth);
	bitfield_byte_length = p - label_str;

	/* Fill in the textual info using stored (shifted) value */
	ret = snprintf(p, ITEM_LABEL_LENGTH - bitfield_byte_length,
			format,  hfinfo->name, value);
	if ((ret == -1) || (ret >= (ITEM_LABEL_LENGTH - bitfield_byte_length)))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';

}

static void
fill_label_enumerated_uint(field_info *fi, gchar *label_str)
{
	char *format = NULL;
	header_field_info	*hfinfo = fi->hfinfo;
	guint32 value;
	int					ret;	/*tmp return value */

	/* Pick the proper format string */
	format = hfinfo_uint_vals_format(hfinfo);

	value = fvalue_get_integer(fi->value);

	/* Fill in the textual info */
	ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			format,  hfinfo->name,
			val_to_str(value, cVALS(hfinfo->strings), "Unknown"), value);
	if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}

static void
fill_label_uint(field_info *fi, gchar *label_str)
{
	char *format = NULL;
	header_field_info	*hfinfo = fi->hfinfo;
	guint32 value;
	int					ret;	/*tmp return value */

	/* Pick the proper format string */
	format = hfinfo_uint_format(hfinfo);
	value = fvalue_get_integer(fi->value);

	/* Fill in the textual info */
	ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			format,  hfinfo->name, value);
	if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}

static void
fill_label_enumerated_int(field_info *fi, gchar *label_str)
{
	char *format = NULL;
	header_field_info	*hfinfo = fi->hfinfo;
	guint32 value;
	int					ret;	/*tmp return value */

	/* Pick the proper format string */
	format = hfinfo_int_vals_format(hfinfo);
	value = fvalue_get_integer(fi->value);

	/* Fill in the textual info */
	ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			format,  hfinfo->name,
			val_to_str(value, cVALS(hfinfo->strings), "Unknown"), value);
	if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}

static void
fill_label_int(field_info *fi, gchar *label_str)
{
	char *format = NULL;
	header_field_info	*hfinfo = fi->hfinfo;
	guint32 value;
	int					ret;	/*tmp return value */

	/* Pick the proper format string */
	format = hfinfo_int_format(hfinfo);
	value = fvalue_get_integer(fi->value);

	/* Fill in the textual info */
	ret = snprintf(label_str, ITEM_LABEL_LENGTH,
			format,  hfinfo->name, value);
	if ((ret == -1) || (ret >= ITEM_LABEL_LENGTH))
		label_str[ITEM_LABEL_LENGTH - 1] = '\0';
}

int
hfinfo_bitwidth(header_field_info *hfinfo)
{
	int bitwidth = 0;

	if (!hfinfo->bitmask) {
		return 0;
	}

	switch(hfinfo->type) {
		case FT_UINT8:
		case FT_INT8:
			bitwidth = 8;
			break;
		case FT_UINT16:
		case FT_INT16:
			bitwidth = 16;
			break;
		case FT_UINT24:
		case FT_INT24:
			bitwidth = 24;
			break;
		case FT_UINT32:
		case FT_INT32:
			bitwidth = 32;
			break;
		case FT_BOOLEAN:
			bitwidth = hfinfo->display; /* hacky? :) */
			break;
		default:
			g_assert_not_reached();
			;
	}
	return bitwidth;
}

static char*
hfinfo_uint_vals_format(header_field_info *hfinfo)
{
	char *format = NULL;

	switch(hfinfo->display) {
		case BASE_DEC:
			format = "%s: %s (%u)";
			break;
		case BASE_OCT: /* I'm lazy */
			format = "%s: %s (%o)";
			break;
		case BASE_HEX:
			switch(hfinfo->type) {
				case FT_UINT8:
					format = "%s: %s (0x%02x)";
					break;
				case FT_UINT16:
					format = "%s: %s (0x%04x)";
					break;
				case FT_UINT24:
					format = "%s: %s (0x%06x)";
					break;
				case FT_UINT32:
					format = "%s: %s (0x%08x)";
					break;
				default:
					g_assert_not_reached();
					;
			}
			break;
		default:
			g_assert_not_reached();
			;
	}
	return format;
}

static char*
hfinfo_uint_format(header_field_info *hfinfo)
{
	char *format = NULL;

	/* Pick the proper format string */
	if (hfinfo->type == FT_FRAMENUM) {
		/*
		 * Frame numbers are always displayed in decimal.
		 */
		format = "%s: %u";
	} else {
		switch(hfinfo->display) {
			case BASE_DEC:
				format = "%s: %u";
				break;
			case BASE_OCT: /* I'm lazy */
				format = "%s: %o";
				break;
			case BASE_HEX:
				switch(hfinfo->type) {
					case FT_UINT8:
						format = "%s: 0x%02x";
						break;
					case FT_UINT16:
						format = "%s: 0x%04x";
						break;
					case FT_UINT24:
						format = "%s: 0x%06x";
						break;
					case FT_UINT32:
						format = "%s: 0x%08x";
						break;
					default:
						g_assert_not_reached();
						;
				}
				break;
			default:
				g_assert_not_reached();
				;
		}
	}
	return format;
}

static char*
hfinfo_int_vals_format(header_field_info *hfinfo)
{
	char *format = NULL;

	switch(hfinfo->display) {
		case BASE_DEC:
			format = "%s: %s (%d)";
			break;
		case BASE_OCT: /* I'm lazy */
			format = "%s: %s (%o)";
			break;
		case BASE_HEX:
			switch(hfinfo->type) {
				case FT_INT8:
					format = "%s: %s (0x%02x)";
					break;
				case FT_INT16:
					format = "%s: %s (0x%04x)";
					break;
				case FT_INT24:
					format = "%s: %s (0x%06x)";
					break;
				case FT_INT32:
					format = "%s: %s (0x%08x)";
					break;
				default:
					g_assert_not_reached();
					;
			}
			break;
		default:
			g_assert_not_reached();
			;
	}
	return format;
}

static char*
hfinfo_int_format(header_field_info *hfinfo)
{
	char *format = NULL;

	/* Pick the proper format string */
	switch(hfinfo->display) {
		case BASE_DEC:
			format = "%s: %d";
			break;
		case BASE_OCT: /* I'm lazy */
			format = "%s: %o";
			break;
		case BASE_HEX:
			switch(hfinfo->type) {
				case FT_INT8:
					format = "%s: 0x%02x";
					break;
				case FT_INT16:
					format = "%s: 0x%04x";
					break;
				case FT_INT24:
					format = "%s: 0x%06x";
					break;
				case FT_INT32:
					format = "%s: 0x%08x";
					break;
				default:
					g_assert_not_reached();
					;
			}
			break;
		default:
			g_assert_not_reached();
			;
	}
	return format;
}



int
proto_registrar_n(void)
{
	return gpa_hfinfo->len;
}

char*
proto_registrar_get_name(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (hfinfo)
		return hfinfo->name;
	else
		return NULL;
}

char*
proto_registrar_get_abbrev(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (hfinfo)
		return hfinfo->abbrev;
	else
		return NULL;
}

int
proto_registrar_get_ftype(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (hfinfo)
		return hfinfo->type;
	else
		return -1;
}

int
proto_registrar_get_parent(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (hfinfo)
		return hfinfo->parent;
	else
		return -2;
}

gboolean
proto_registrar_is_protocol(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (hfinfo)
		return (hfinfo->parent == -1 ? TRUE : FALSE);
	else
		return FALSE;
}

/* Returns length of field in packet (not necessarily the length
 * in our internal representation, as in the case of IPv4).
 * 0 means undeterminable at time of registration
 * -1 means the field is not registered. */
gint
proto_registrar_get_length(int n)
{
	header_field_info *hfinfo;

	hfinfo = proto_registrar_get_nth(n);
	if (!hfinfo)
		return -1;

	return ftype_length(hfinfo->type);
}



/* Looks for a protocol or a field in a proto_tree. Returns TRUE if
 * it exists anywhere, or FALSE if it exists nowhere. */
gboolean
proto_check_for_protocol_or_field(proto_tree* tree, int id)
{
	GPtrArray *ptrs = proto_get_finfo_ptr_array(tree, id);

	if (!ptrs) {
		return FALSE;
	}
	else if (g_ptr_array_len(ptrs) > 0) {
		return TRUE;
	}
	else {
		return FALSE;
	}
}

/* Return GPtrArray* of field_info pointers for all hfindex that appear in tree.
 * This only works if the hfindex was "primed" before the dissection
 * took place, as we just pass back the already-created GPtrArray*.
 * The caller should *not* free the GPtrArray*; proto_tree_free_node()
 * handles that. */
GPtrArray*
proto_get_finfo_ptr_array(proto_tree *tree, int id)
{
	return g_hash_table_lookup(PTREE_DATA(tree)->interesting_hfids,
	    GINT_TO_POINTER(id));
}


typedef struct {
	guint		offset;
	field_info	*finfo;
	tvbuff_t	*tvb;
} offset_search_t;

static gboolean
check_for_offset(GNode *node, gpointer data)
{
	field_info          *fi = PITEM_FINFO(node);
	offset_search_t		*offsearch = data;

	/* !fi == the top most container node which holds nothing */
	if (fi && fi->visible && fi->ds_tvb && offsearch->tvb == fi->ds_tvb) {
		if (offsearch->offset >= (guint) fi->start &&
				offsearch->offset < (guint) (fi->start + fi->length)) {

			offsearch->finfo = fi;
			return FALSE; /* keep traversing */
		}
	}
	return FALSE; /* keep traversing */
}

/* Search a proto_tree backwards (from leaves to root) looking for the field
 * whose start/length occupies 'offset' */
/* XXX - I couldn't find an easy way to search backwards, so I search
 * forwards, w/o stopping. Therefore, the last finfo I find will the be
 * the one I want to return to the user. This algorithm is inefficient
 * and could be re-done, but I'd have to handle all the children and
 * siblings of each node myself. When I have more time I'll do that.
 * (yeah right) */
field_info*
proto_find_field_from_offset(proto_tree *tree, guint offset, tvbuff_t *tvb)
{
	offset_search_t		offsearch;

	offsearch.offset = offset;
	offsearch.finfo = NULL;
	offsearch.tvb = tvb;

	g_node_traverse((GNode*)tree, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
			check_for_offset, &offsearch);

	return offsearch.finfo;
}

/* Dumps the protocols in the registration database to stdout.  An independent
 * program can take this output and format it into nice tables or HTML or
 * whatever.
 *
 * There is one record per line. The fields are tab-delimited.
 *
 * Field 1 = protocol name
 * Field 2 = protocol short name
 * Field 3 = protocol filter name
 */
void
proto_registrar_dump_protocols(void)
{
	protocol_t		*protocol;
	int			i;
	void			*cookie;

	for (i = proto_get_first_protocol(&cookie); i != -1;
	    i = proto_get_next_protocol(&cookie)) {
		protocol = find_protocol_by_id(i);
		printf("%s\t%s\t%s\n", protocol->name, protocol->short_name,
		    protocol->filter_name);
	}
}

/* Dumps the contents of the registration database to stdout. An indepedent
 * program can take this output and format it into nice tables or HTML or
 * whatever.
 *
 * There is one record per line. Each record is either a protocol or a header
 * field, differentiated by the first field. The fields are tab-delimited.
 *
 * Protocols
 * ---------
 * Field 1 = 'P'
 * Field 2 = protocol name
 * Field 3 = protocol abbreviation
 *
 * Header Fields
 * -------------
 * Field 1 = 'F'
 * Field 2 = field name
 * Field 3 = field abbreviation
 * Field 4 = type ( textual representation of the the ftenum type )
 * Field 5 = parent protocol abbreviation
 */
void
proto_registrar_dump_fields(void)
{
	header_field_info	*hfinfo, *parent_hfinfo;
	int			i, len;
	const char 		*enum_name;

	len = gpa_hfinfo->len;
	for (i = 0; i < len ; i++) {
		hfinfo = proto_registrar_get_nth(i);

		/*
		 * Skip fields with zero-length names or abbreviations;
		 * the pseudo-field for "proto_tree_add_text()" is such
		 * a field, and we don't want it in the list of filterable
		 * fields.
		 *
		 *
		 * XXX - perhaps the name and abbrev field should be null
		 * pointers rather than null strings for that pseudo-field,
		 * but we'd have to add checks for null pointers in some
		 * places if we did that.
		 *
		 * Or perhaps protocol tree items added with
		 * "proto_tree_add_text()" should have -1 as the field index,
		 * with no pseudo-field being used, but that might also
		 * require special checks for -1 to be added.
		 */
		if (hfinfo->name[0] == 0 || hfinfo->abbrev[0] == 0)
			continue;

		/* format for protocols */
		if (proto_registrar_is_protocol(i)) {
			printf("P\t%s\t%s\n", hfinfo->name, hfinfo->abbrev);
		}
		/* format for header fields */
		else {
			/*
			 * If this field isn't at the head of the list of
			 * fields with this name, skip this field - all
			 * fields with the same name are really just versions
			 * of the same field stored in different bits, and
			 * should have the same type/radix/value list, and
			 * just differ in their bit masks.  (If a field isn't
			 * a bitfield, but can be, say, 1 or 2 bytes long,
			 * it can just be made FT_UINT16, meaning the
			 * *maximum* length is 2 bytes, and be used
			 * for all lengths.)
			 */
			if (hfinfo->same_name_prev != NULL)
				continue;

			parent_hfinfo = proto_registrar_get_nth(hfinfo->parent);
			g_assert(parent_hfinfo);

			enum_name = ftype_name(hfinfo->type);
			printf("F\t%s\t%s\t%s\t%s\t%s\n", hfinfo->name, hfinfo->abbrev,
				enum_name,parent_hfinfo->abbrev, hfinfo->blurb);
		}
	}
}

static char*
hfinfo_numeric_format(header_field_info *hfinfo)
{
	char *format = NULL;

	/* Pick the proper format string */
	if (hfinfo->type == FT_FRAMENUM) {
		/*
		 * Frame numbers are always displayed in decimal.
		 */
		format = "%s == %u";
	} else {
		/* Pick the proper format string */
		switch(hfinfo->display) {
			case BASE_DEC:
			case BASE_OCT: /* I'm lazy */
				switch(hfinfo->type) {
					case FT_UINT8:
					case FT_UINT16:
					case FT_UINT24:
					case FT_UINT32:
						format = "%s == %u";
						break;
					case FT_INT8:
					case FT_INT16:
					case FT_INT24:
					case FT_INT32:
						format = "%s == %d";
						break;
					default:
						g_assert_not_reached();
						;
				}
				break;
			case BASE_HEX:
				switch(hfinfo->type) {
					case FT_UINT8:
						format = "%s == 0x%02x";
						break;
					case FT_UINT16:
						format = "%s == 0x%04x";
						break;
					case FT_UINT24:
						format = "%s == 0x%06x";
						break;
					case FT_UINT32:
						format = "%s == 0x%08x";
						break;
					default:
						g_assert_not_reached();
						;
				}
				break;
			default:
				g_assert_not_reached();
				;
		}
	}
	return format;
}

/*
 * Returns TRUE if we can do a "match selected" on the field, FALSE
 * otherwise.
 */
gboolean
proto_can_match_selected(field_info *finfo, epan_dissect_t *edt)
{
	header_field_info	*hfinfo;
	gint			length;

	hfinfo = finfo->hfinfo;
	g_assert(hfinfo);

	switch(hfinfo->type) {

		case FT_BOOLEAN:
		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
		case FT_FRAMENUM:
		case FT_UINT64:
		case FT_INT64:
		case FT_IPv4:
		case FT_IPXNET:
		case FT_IPv6:
		case FT_FLOAT:
		case FT_DOUBLE:
		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
		case FT_STRING:
		case FT_STRINGZ:
		case FT_UINT_STRING:
		case FT_ETHER:
		case FT_BYTES:
		case FT_UINT_BYTES:
		case FT_PROTOCOL:
			/*
			 * These all have values, so we can match.
			 */
			return TRUE;

		default:
			/*
			 * This doesn't have a value, so we'd match
			 * on the raw bytes at this address.
			 *
			 * Should we be allowed to access to the raw bytes?
			 * If "edt" is NULL, the answer is "no".
			 */
			if (edt == NULL)
				return FALSE;

			/*
			 * Is this field part of the raw frame tvbuff?
			 * If not, we can't use "frame[N:M]" to match
			 * it.
			 *
			 * XXX - should this be frame-relative, or
			 * protocol-relative?
			 *
			 * XXX - does this fallback for non-registered
			 * fields even make sense?
			 */
			if (finfo->ds_tvb != edt->tvb)
				return FALSE;

			/*
			 * If the length is 0, there's nothing to match, so
			 * we can't match.  (Also check for negative values,
			 * just in case, as we'll cast it to an unsigned
			 * value later.)
			 */
			length = finfo->length;
			if (length <= 0)
				return FALSE;

			/*
			 * Don't go past the end of that tvbuff.
			 */
			if ((guint)length > tvb_length(finfo->ds_tvb))
				length = tvb_length(finfo->ds_tvb);
			if (length <= 0)
				return FALSE;
			return TRUE;
	}
}

char*
proto_construct_dfilter_string(field_info *finfo, epan_dissect_t *edt)
{
	header_field_info	*hfinfo;
	int			abbrev_len;
	char			*buf, *stringified, *format, *ptr;
	int			dfilter_len, i;
	gint			start, length;
	guint8			c;

	hfinfo = finfo->hfinfo;
	g_assert(hfinfo);
	abbrev_len = strlen(hfinfo->abbrev);

	/*
	 * XXX - we should add "val_to_string_repr" and "string_repr_len"
	 * functions for more types, and use them whenever possible.
	 *
	 * The FT_UINT and FT_INT types are the only tricky ones, as
	 * we choose the base in the string expression based on the
	 * display base of the field.
	 *
	 * Note that the base does matter, as this is also used for
	 * the protocolinfo tap.
	 *
	 * It might be nice to use that in "proto_item_fill_label()"
	 * as well, although, there, you'd have to deal with the base
	 * *and* with resolved values for addresses.
	 *
	 * Perhaps we need two different val_to_string routines, one
	 * to generate items for display filters and one to generate
	 * strings for display, and pass to both of them the
	 * "display" and "strings" values in the header_field_info
	 * structure for the field, so they can get the base and,
	 * if the field is Boolean or an enumerated integer type,
	 * the tables used to generate human-readable values.
	 */
	switch(hfinfo->type) {

		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
		case FT_FRAMENUM:
			/*
			 * 4 bytes for " == ".
			 * 11 bytes for:
			 *
			 *	a sign + up to 10 digits of 32-bit integer,
			 *	in decimal;
			 *
			 *	"0x" + 8 digits of 32-bit integer, in hex;
			 *
			 *	11 digits of 32-bit integer, in octal.
			 *	(No, we don't do octal, but this way,
			 *	we know that if we do, this will still
			 *	work.)
			 *
			 * 1 byte for the trailing '\0'.
			 */
			dfilter_len = abbrev_len + 4 + 11 + 1;
			buf = g_malloc0(dfilter_len);
			format = hfinfo_numeric_format(hfinfo);
			snprintf(buf, dfilter_len, format, hfinfo->abbrev, fvalue_get_integer(finfo->value));
			break;

		case FT_UINT64:
			/*
			 * 4 bytes for " == ".
			 * N bytes for the string for the number.
			 * 1 byte for the trailing '\0'.
			 */
			stringified = u64toa(fvalue_get(finfo->value));
			dfilter_len = abbrev_len + 4 + strlen(stringified) +1;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %s", hfinfo->abbrev,
					stringified);
			break;

		case FT_INT64:
			/*
			 * 4 bytes for " == ".
			 * N bytes for the string for the number.
			 * 1 byte for the trailing '\0'.
			 */
			stringified = i64toa(fvalue_get(finfo->value));
			dfilter_len = abbrev_len + 4 + strlen(stringified) +1;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %s", hfinfo->abbrev,
					stringified);
			break;

		case FT_IPXNET:
			/*
			 * 4 bytes for " == ".
			 * 2 bytes for "0x".
			 * 8 bytes for 8 digits of 32-bit hex number.
			 * 1 byte for the trailing '\0'.
			 */
			dfilter_len = abbrev_len + 4 + 2 + 8 + 1;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == 0x%08x", hfinfo->abbrev,
					fvalue_get_integer(finfo->value));
			break;

		case FT_IPv6:
			/*
			 * 4 bytes for " == ".
			 * N bytes for the string for the address.
			 * 1 byte for the trailing '\0'.
			 */
			stringified = ip6_to_str((struct e_in6_addr*) fvalue_get(finfo->value));
			dfilter_len = abbrev_len + 4 + strlen(stringified) + 1;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %s", hfinfo->abbrev,
					stringified);
			break;

		/* These use the fvalue's "to_string_repr" method. */
		case FT_BOOLEAN:
		case FT_STRING:
		case FT_ETHER:
		case FT_BYTES:
		case FT_UINT_BYTES:
		case FT_FLOAT:
		case FT_DOUBLE:
		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
		case FT_IPv4:
			/* Figure out the string length needed.
			 * 	The ft_repr length.
			 * 	4 bytes for " == ".
			 * 	1 byte for trailing NUL.
			 */
			dfilter_len = fvalue_string_repr_len(finfo->value,
					FTREPR_DFILTER);
			dfilter_len += abbrev_len + 4 + 1;
			buf = g_malloc0(dfilter_len);

			/* Create the string */
			snprintf(buf, dfilter_len, "%s == ", hfinfo->abbrev);
			fvalue_to_string_repr(finfo->value,
					FTREPR_DFILTER,
					&buf[abbrev_len + 4]);
			break;

		case FT_PROTOCOL:
			buf = g_strdup(finfo->hfinfo->abbrev);
			break;

		default:
			/*
			 * This doesn't have a value, so we'd match
			 * on the raw bytes at this address.
			 *
			 * Should we be allowed to access to the raw bytes?
			 * If "edt" is NULL, the answer is "no".
			 */
			if (edt == NULL)
				return FALSE;

			/*
			 * Is this field part of the raw frame tvbuff?
			 * If not, we can't use "frame[N:M]" to match
			 * it.
			 *
			 * XXX - should this be frame-relative, or
			 * protocol-relative?
			 *
			 * XXX - does this fallback for non-registered
			 * fields even make sense?
			 */
			if (finfo->ds_tvb != edt->tvb)
				return NULL;	/* you lose */

			/*
			 * If the length is 0, there's nothing to match, so
			 * we can't match.  (Also check for negative values,
			 * just in case, as we'll cast it to an unsigned
			 * value later.)
			 */
			length = finfo->length;
			if (length <= 0)
				return NULL;

			/*
			 * Don't go past the end of that tvbuff.
			 */
			if ((guint)length > tvb_length(finfo->ds_tvb))
				length = tvb_length(finfo->ds_tvb);
			if (length <= 0)
				return NULL;
			
			start = finfo->start;
			buf = g_malloc0(32 + length * 3);
			ptr = buf;

			sprintf(ptr, "frame[%d:%d] == ", finfo->start, length);
			ptr = buf+strlen(buf);

			for (i=0;i<length; i++) {
				c = tvb_get_guint8(finfo->ds_tvb, start);
				start++;
				if (i == 0 ) {
					sprintf(ptr, "%02x", c);
				}
				else {
					sprintf(ptr, ":%02x", c);
				}
				ptr = buf+strlen(buf);
			}
			break;
	}

	return buf;
}
