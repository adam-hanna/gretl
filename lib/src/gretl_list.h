/*
 *  Copyright (c) Allin Cottrell
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef GRETL_LIST_H
#define GRETL_LIST_H

int *gretl_list_new (int nterms);

int *gretl_null_list (void);

int *gretl_list_copy (const int *src);

int *gretl_list_from_string (const char *liststr);

char *gretl_list_to_string (const int *list);

int in_gretl_list (const int *list, int k);

int gretl_list_delete_at_pos (int *list, int pos);

int *gretl_list_add (const int *orig, const int *add, int *err);

int *gretl_list_omit (const int *orig, const int *omit, int *err);

int *gretl_list_omit_last (const int *orig, int *err);

int gretl_list_diff (int *targ, const int *biglist, const int *sublist);

int *gretl_list_diff_new (const int *biglist, const int *sublist);

int gretl_list_add_list (int **targ, const int *src);

int gretl_list_insert_list (int **targ, const int *src, int pos);

void rearrange_list (int *list);

int list_members_replaced (const int *list, const DATAINFO *pdinfo,
			   int ref_id);

int gretl_list_has_const (const int *list);

int gretl_list_has_separator (const int *list);

int gretl_list_duplicates (const int *list, GretlCmdIndex ci);

int *full_var_list (const DATAINFO *pdinfo, int *nvars);

int *get_list_by_name (const char *name);

int remember_list (const int *list, const char *name, PRN *prn);

int copy_named_list_as (const char *orig, const char *new);

int destroy_saved_lists_at_level (int level);

void gretl_lists_cleanup (void);

#endif /* GRETL_LIST_H */
