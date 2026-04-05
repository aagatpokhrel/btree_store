#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_tree.h"
#include "jdisk.h"

#define JDISK_SECTOR_SIZE 1024

typedef struct tnode
{
  unsigned char bytes[JDISK_SECTOR_SIZE + 256];
  unsigned char nkeys;
  unsigned char flush;
  unsigned char internal;
  unsigned int lba;
  unsigned char **keys;
  unsigned int *lbas;
  struct tnode *parent;
  int parent_index;
  struct tnode *ptr;
} Tree_Node;

typedef struct
{
  int key_size;
  unsigned int root_lba;
  unsigned long first_free_block;
  void *disk;
  unsigned long size;
  unsigned long num_lbas;
  int keys_per_block;
  int lbas_per_block;
  Tree_Node *free_list;
  Tree_Node *tmp_e;
  int tmp_e_index;
  int flush;
} B_Tree_Struct;

/* --- Utility Functions --- */

void *b_tree_disk(void *b_tree) { return ((B_Tree_Struct *)b_tree)->disk; }
int b_tree_key_size(void *b_tree) { return ((B_Tree_Struct *)b_tree)->key_size; }

/* --- Node Management (Plank's 3-malloc + free list style) --- */

static Tree_Node *all_node(B_Tree_Struct *bt)
{
  Tree_Node *tn;
  if (bt->free_list)
  {
    tn = bt->free_list;
    bt->free_list = tn->ptr;
  }
  else
  {
    tn = malloc(sizeof(Tree_Node));
    tn->keys = malloc(sizeof(unsigned char *) * (bt->keys_per_block + 1));
    tn->lbas = malloc(sizeof(unsigned int) * (bt->keys_per_block + 2));
  }
  for (int i = 0; i <= bt->keys_per_block; i++)
  {
    tn->keys[i] = tn->bytes + 2 + (i * bt->key_size);
  }
  tn->parent = NULL;
  tn->nkeys = 0;
  tn->internal = 0;
  tn->flush = 0;
  tn->lba = 0;
  return tn;
}

static void flush_node(B_Tree_Struct *bt, Tree_Node *tn)
{
  if (!tn->flush)
    return;
  tn->bytes[0] = tn->internal;
  tn->bytes[1] = tn->nkeys;
  int lba_offset = JDISK_SECTOR_SIZE - (bt->lbas_per_block * 4);
  for (int i = 0; i < bt->lbas_per_block; i++)
  {
    unsigned int v = tn->lbas[i];
    unsigned char *p = tn->bytes + lba_offset + (i * 4);
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
  }
  jdisk_write(bt->disk, tn->lba, tn->bytes);
  tn->flush = 0;
}

static Tree_Node *read_node(B_Tree_Struct *bt, unsigned int lba)
{
  Tree_Node *tn = all_node(bt);
  jdisk_read(bt->disk, lba, tn->bytes);
  tn->internal = tn->bytes[0];
  tn->nkeys = tn->bytes[1];
  tn->lba = lba;
  int lba_offset = JDISK_SECTOR_SIZE - (bt->lbas_per_block * 4);
  for (int i = 0; i < bt->lbas_per_block; i++)
  {
    unsigned char *p = tn->bytes + lba_offset + (i * 4);
    tn->lbas[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  }
  return tn;
}

static void free_and_flush(B_Tree_Struct *bt, Tree_Node *tn)
{
  if (!tn)
    return;
  Tree_Node *p = tn->parent;
  flush_node(bt, tn);
  tn->ptr = bt->free_list;
  bt->free_list = tn;
  free_and_flush(bt, p);
}

/* --- Recursive Split --- */

static void split_node(B_Tree_Struct *bt, Tree_Node *tn)
{
  if (tn->nkeys <= bt->keys_per_block)
    return;

  Tree_Node *sib = all_node(bt);
  sib->lba = bt->first_free_block++;
  sib->internal = tn->internal;
  sib->flush = 1;
  bt->flush = 1; // Mark sector 0 for flush

  unsigned char mid_key[1024];
  int mid = tn->nkeys / 2;

  if (tn->internal)
  {
    // Internal split: move key[mid] up, remove from children
    memcpy(mid_key, tn->keys[mid], bt->key_size);
    sib->nkeys = tn->nkeys - mid - 1;
    for (int i = 0; i < sib->nkeys; i++)
    {
      memcpy(sib->keys[i], tn->keys[mid + 1 + i], bt->key_size);
      sib->lbas[i] = tn->lbas[mid + 1 + i];
    }
    sib->lbas[sib->nkeys] = tn->lbas[tn->nkeys];
    tn->nkeys = mid;
  }
  else
  {
    // Leaf split: copy key[mid] up, KEEP in sibling
    sib->nkeys = tn->nkeys - mid;
    for (int i = 0; i < sib->nkeys; i++)
    {
      memcpy(sib->keys[i], tn->keys[mid + i], bt->key_size);
      sib->lbas[i] = tn->lbas[mid + i];
    }
    // FIX: Promote the maximum key of the left node for left-biased behavior
    memcpy(mid_key, tn->keys[mid - 1], bt->key_size);
    tn->nkeys = mid;
  }

  if (tn->parent == NULL)
  {
    Tree_Node *nr = all_node(bt);
    nr->lba = bt->first_free_block++;
    nr->internal = 1;
    nr->nkeys = 1;
    nr->flush = 1;
    bt->root_lba = nr->lba;
    memcpy(nr->keys[0], mid_key, bt->key_size);
    nr->lbas[0] = tn->lba;
    nr->lbas[1] = sib->lba;
    tn->parent = nr;
    tn->parent_index = 0;
    sib->parent = nr;
    sib->parent_index = 1;
  }
  else
  {
    Tree_Node *p = tn->parent;
    int idx = 0;
    while (idx < p->nkeys && memcmp(mid_key, p->keys[idx], bt->key_size) > 0)
      idx++;
    for (int j = p->nkeys; j > idx; j--)
      memcpy(p->keys[j], p->keys[j - 1], bt->key_size);
    for (int j = p->nkeys + 1; j > idx + 1; j--)
      p->lbas[j] = p->lbas[j - 1];
    memcpy(p->keys[idx], mid_key, bt->key_size);
    p->lbas[idx + 1] = sib->lba;
    p->nkeys++;
    p->flush = 1;
    sib->parent = p;
    split_node(bt, p);
  }
  flush_node(bt, sib);
  sib->ptr = bt->free_list;
  bt->free_list = sib;
}

/* --- API --- */

void *b_tree_create(char *fn, long sz, int ks)
{
  B_Tree_Struct *bt = calloc(1, sizeof(B_Tree_Struct));
  bt->disk = jdisk_create(fn, sz);
  if (!bt->disk)
    return NULL;
  bt->key_size = ks;
  bt->size = sz;
  bt->keys_per_block = (JDISK_SECTOR_SIZE - 6) / (ks + 4);
  bt->lbas_per_block = bt->keys_per_block + 1;
  bt->root_lba = 1;
  bt->first_free_block = 2;

  Tree_Node *root = all_node(bt);
  root->lba = 1;
  root->flush = 1;
  flush_node(bt, root);
  root->ptr = bt->free_list;
  bt->free_list = root;

  unsigned char s0[1024] = {0};
  memcpy(s0, &bt->key_size, 4);
  memcpy(s0 + 4, &bt->root_lba, 4);
  memcpy(s0 + 8, &bt->first_free_block, 8);
  jdisk_write(bt->disk, 0, s0);
  return bt;
}

void *b_tree_attach(char *fn)
{
  B_Tree_Struct *bt = calloc(1, sizeof(B_Tree_Struct));
  bt->disk = jdisk_attach(fn);
  unsigned char s0[1024];
  jdisk_read(bt->disk, 0, s0);
  memcpy(&bt->key_size, s0, 4);
  memcpy(&bt->root_lba, s0 + 4, 4);
  memcpy(&bt->first_free_block, s0 + 8, 8);
  bt->keys_per_block = (JDISK_SECTOR_SIZE - 6) / (bt->key_size + 4);
  bt->lbas_per_block = bt->keys_per_block + 1;
  return bt;
}

unsigned int b_tree_find(void *b_tree, void *key)
{
  B_Tree_Struct *bt = (B_Tree_Struct *)b_tree;
  Tree_Node *curr = read_node(bt, bt->root_lba);
  while (curr->internal)
  {
    int i = 0;
    // FIX: Change to > 0 so equality follows the left child (left-biased)
    while (i < curr->nkeys && memcmp(key, curr->keys[i], bt->key_size) > 0)
      i++;
    unsigned int next_lba = curr->lbas[i];
    Tree_Node *next = read_node(bt, next_lba);
    next->parent = curr;
    next->parent_index = i;
    curr = next;
  }
  bt->tmp_e = curr;
  for (int i = 0; i < curr->nkeys; i++)
  {
    if (memcmp(key, curr->keys[i], bt->key_size) == 0)
      return curr->lbas[i];
  }
  return 0;
}

unsigned int b_tree_insert(void *b_tree, void *key, void *record)
{
  B_Tree_Struct *bt = (B_Tree_Struct *)b_tree;
  unsigned int res = b_tree_find(bt, key);
  if (res)
  {
    jdisk_write(bt->disk, res, record);
    free_and_flush(bt, bt->tmp_e);
    return res;
  }
  unsigned int val_lba = bt->first_free_block++;
  jdisk_write(bt->disk, val_lba, record);
  bt->flush = 1;

  Tree_Node *tn = bt->tmp_e;
  int i = 0;
  while (i < tn->nkeys && memcmp(key, tn->keys[i], bt->key_size) > 0)
    i++;
  for (int j = tn->nkeys; j > i; j--)
    memcpy(tn->keys[j], tn->keys[j - 1], bt->key_size);
  for (int j = tn->nkeys; j > i; j--)
    tn->lbas[j] = tn->lbas[j - 1];
  memcpy(tn->keys[i], key, bt->key_size);
  tn->lbas[i] = val_lba;
  tn->nkeys++;
  tn->flush = 1;

  if (tn->nkeys > bt->keys_per_block)
    split_node(bt, tn);

  if (bt->flush)
  {
    unsigned char s0[1024] = {0};
    memcpy(s0, &bt->key_size, 4);
    memcpy(s0 + 4, &bt->root_lba, 4);
    memcpy(s0 + 8, &bt->first_free_block, 8);
    jdisk_write(bt->disk, 0, s0);
    bt->flush = 0;
  }
  free_and_flush(bt, tn);
  return val_lba;
}

/* --- Printer --- */

static void recursive_print_nodes(B_Tree_Struct *bt, unsigned int lba, int level)
{
  Tree_Node *tn = read_node(bt, lba);
  for (int i = 0; i < level; i++)
    printf("  ");
  printf("LBA %u (%s, %d keys): ", lba, tn->internal ? "Int" : "Leaf", tn->nkeys);
  for (int i = 0; i < tn->nkeys; i++)
    printf("[%02x%02x] ", tn->keys[i][0], tn->keys[i][1]);
  printf("\n");
  if (tn->internal)
    for (int i = 0; i <= tn->nkeys; i++)
      recursive_print_nodes(bt, tn->lbas[i], level + 1);
  tn->parent = NULL;
  tn->flush = 0;
  tn->ptr = bt->free_list;
  bt->free_list = tn;
}

void b_tree_print_tree(void *b_tree)
{
  B_Tree_Struct *bt = (B_Tree_Struct *)b_tree;
  printf("\n--- B-Tree Root: %u ---\n", bt->root_lba);
  if (bt->root_lba > 0)
    recursive_print_nodes(bt, bt->root_lba, 0);
}