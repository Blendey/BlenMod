/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tree hash for the outliner space.
 */

#include <stdlib.h>
#include <string.h>

#include "BLI_ghash.h"
#include "BLI_mempool.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_outliner_types.h"

#include "BKE_outliner_treehash.hh"

#include "MEM_guardedalloc.h"

struct TseGroup {
  blender::Vector<TreeStoreElem *> elems;
  /* Index of last used #TreeStoreElem item, to speed up search for another one. */
  int lastused;
  /* Counter used to reduce the amount of 'rests' of `lastused` index, otherwise search for unused
   * item is exponential and becomes critically slow when there are a lot of items in the group. */
  int lastused_reset_count;
};

/* Only allow reset of #TseGroup.lastused counter to 0 once every 1k search. */
#define TSEGROUP_LASTUSED_RESET_VALUE 10000

/* Allocate structure for TreeStoreElements;
 * Most of elements in treestore have no duplicates,
 * so there is no need to preallocate memory for more than one pointer */
static TseGroup *tse_group_create(void)
{
  TseGroup *tse_group = MEM_new<TseGroup>("TseGroup");
  tse_group->lastused = 0;
  return tse_group;
}

static void tse_group_add_element(TseGroup *tse_group, TreeStoreElem *elem)
{
  const int64_t idx = tse_group->elems.append_and_get_index(elem);
  tse_group->lastused = idx;
}

static void tse_group_remove_element(TseGroup &group, TreeStoreElem &elem)
{
  const int64_t idx = group.elems.first_index_of(&elem);
  group.elems.remove(idx);
}

static void tse_group_free(TseGroup *tse_group)
{
  MEM_delete(tse_group);
}

static unsigned int tse_hash(const void *ptr)
{
  const TreeStoreElem *tse = static_cast<const TreeStoreElem *>(ptr);
  union {
    short h_pair[2];
    unsigned int u_int;
  } hash;

  BLI_assert((tse->type != TSE_SOME_ID) || !tse->nr);

  hash.h_pair[0] = tse->type;
  hash.h_pair[1] = tse->nr;

  hash.u_int ^= BLI_ghashutil_ptrhash(tse->id);

  return hash.u_int;
}

static bool tse_cmp(const void *a, const void *b)
{
  const TreeStoreElem *tse_a = static_cast<const TreeStoreElem *>(a);
  const TreeStoreElem *tse_b = static_cast<const TreeStoreElem *>(b);
  return tse_a->type != tse_b->type || tse_a->nr != tse_b->nr || tse_a->id != tse_b->id;
}

static void fill_treehash(GHash *treehash, BLI_mempool *treestore)
{
  TreeStoreElem *tselem;
  BLI_mempool_iter iter;
  BLI_mempool_iternew(treestore, &iter);

  BLI_assert(treehash);

  while ((tselem = static_cast<TreeStoreElem *>(BLI_mempool_iterstep(&iter)))) {
    BKE_outliner_treehash_add_element(treehash, tselem);
  }
}

GHash *BKE_outliner_treehash_create_from_treestore(BLI_mempool *treestore)
{
  GHash *treehash = BLI_ghash_new_ex(tse_hash, tse_cmp, "treehash", BLI_mempool_len(treestore));
  fill_treehash(treehash, treestore);
  return treehash;
}

static void free_treehash_group(void *key)
{
  tse_group_free(static_cast<TseGroup *>(key));
}

void BKE_outliner_treehash_clear_used(GHash *treehash)
{
  GHashIterator gh_iter;

  GHASH_ITER (gh_iter, treehash) {
    TseGroup *group = static_cast<TseGroup *>(BLI_ghashIterator_getValue(&gh_iter));
    group->lastused = 0;
    group->lastused_reset_count = 0;
  }
}

GHash *BKE_outliner_treehash_rebuild_from_treestore(GHash *treehash, BLI_mempool *treestore)
{
  BLI_assert(treehash);

  BLI_ghash_clear_ex(treehash, nullptr, free_treehash_group, BLI_mempool_len(treestore));
  fill_treehash(treehash, treestore);
  return treehash;
}

void BKE_outliner_treehash_add_element(GHash *treehash, TreeStoreElem *elem)
{
  TseGroup *group;
  void **val_p;

  if (!BLI_ghash_ensure_p(treehash, elem, &val_p)) {
    *val_p = tse_group_create();
  }
  group = static_cast<TseGroup *>(*val_p);
  tse_group_add_element(group, elem);
}

void BKE_outliner_treehash_remove_element(GHash *treehash, TreeStoreElem *elem)
{
  TseGroup *group = static_cast<TseGroup *>(BLI_ghash_lookup(treehash, elem));

  BLI_assert(group != nullptr);
  if (group->elems.size() <= 1) {
    /* one element -> remove group completely */
    BLI_ghash_remove(treehash, elem, nullptr, free_treehash_group);
  }
  else {
    tse_group_remove_element(*group, *elem);
  }
}

static TseGroup *BKE_outliner_treehash_lookup_group(GHash *th, short type, short nr, struct ID *id)
{
  TreeStoreElem tse_template;
  tse_template.type = type;
  tse_template.nr = (type == TSE_SOME_ID) ? 0 : nr; /* we're picky! :) */
  tse_template.id = id;

  BLI_assert(th);

  return static_cast<TseGroup *>(BLI_ghash_lookup(th, &tse_template));
}

TreeStoreElem *BKE_outliner_treehash_lookup_unused(GHash *treehash,
                                                   short type,
                                                   short nr,
                                                   struct ID *id)
{
  TseGroup *group;

  BLI_assert(treehash);

  group = BKE_outliner_treehash_lookup_group(treehash, type, nr, id);
  if (!group) {
    return nullptr;
  }
  /* Find unused element, with optimization to start from previously
   * found element assuming we do repeated lookups. */
  const int size = group->elems.size();
  int offset = group->lastused;

  for (int i = 0; i < size; i++, offset++) {
    /* Once at the end of the array of items, in most cases it just means that all items are
     * used, so only check the whole array once every TSEGROUP_LASTUSED_RESET_VALUE times. */
    if (offset >= size) {
      if (LIKELY(group->lastused_reset_count <= TSEGROUP_LASTUSED_RESET_VALUE)) {
        group->lastused_reset_count++;
        group->lastused = group->elems.size() - 1;
        break;
      }
      group->lastused_reset_count = 0;
      offset = 0;
    }

    if (!group->elems[offset]->used) {
      group->lastused = offset;
      return group->elems[offset];
    }
  }
  return nullptr;
}

TreeStoreElem *BKE_outliner_treehash_lookup_any(GHash *treehash, short type, short nr, ID *id)
{
  TseGroup *group;

  BLI_assert(treehash);

  group = BKE_outliner_treehash_lookup_group(treehash, type, nr, id);
  return group ? group->elems[0] : nullptr;
}

void BKE_outliner_treehash_free(GHash *treehash)
{
  BLI_assert(treehash);

  BLI_ghash_free(treehash, nullptr, free_treehash_group);
}
