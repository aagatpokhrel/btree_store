#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b_tree.h"

#define JDISK_SECTOR_SIZE 1024

typedef struct tnode {
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

typedef struct {
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
} B_Tree;

static Tree_Node *allocate_tree_node(B_Tree *bt) {
  Tree_Node *tn;
  int i;

  if (bt->free_list != NULL) {
    tn = bt->free_list;
    bt->free_list = tn->ptr;
  } else {
    tn = (Tree_Node *) malloc(sizeof(Tree_Node));
    tn->keys = (unsigned char **) malloc(sizeof(unsigned char *) * (bt->keys_per_block + 1));
    tn->lbas = (unsigned int *) malloc(sizeof(unsigned int) * (bt->lbas_per_block + 1));
  }

  for (i = 0; i <= bt->keys_per_block; i++) {
    tn->keys[i] = tn->bytes + 2 + i * bt->key_size;
  }

  tn->nkeys = 0;
  tn->flush = 0;
  tn->internal = 0;
  tn->lba = 0;
  tn->parent = NULL;
  tn->parent_index = -1;
  tn->ptr = NULL;

  return tn;
}

static void free_tree_node(B_Tree *bt, Tree_Node *tn) {
  tn->ptr = bt->free_list;
  bt->free_list = tn;
}

static void read_tree_node(B_Tree *bt, Tree_Node *tn, unsigned int lba) {
  jdisk_read(bt->disk, lba, tn->bytes);
  tn->nkeys = tn->bytes[1];
  tn->internal = tn->bytes[0];
  tn->lba = lba;
  tn->flush = 0;
  memcpy(tn->lbas, tn->bytes + JDISK_SECTOR_SIZE - (bt->lbas_per_block) * 4, (bt->lbas_per_block) * 4);
}

static void write_tree_node(B_Tree *bt, Tree_Node *tn) {
  tn->bytes[0] = tn->internal;
  tn->bytes[1] = tn->nkeys;
  memcpy(tn->bytes + JDISK_SECTOR_SIZE - (bt->lbas_per_block) * 4, tn->lbas, (bt->lbas_per_block) * 4);
  jdisk_write(bt->disk, tn->lba, tn->bytes);
  tn->flush = 0;
}

static void free_and_flush(B_Tree *bt, Tree_Node *tn) {
  if (tn->flush) {
    write_tree_node(bt, tn);
  }
  free_tree_node(bt, tn);
}

void *b_tree_create(char *filename, long size, int key_size) {
  B_Tree *bt;
  Tree_Node *root;
  unsigned int root_lba;
  unsigned char sector0[1024];

  if (key_size < 4 || key_size > 254) return NULL;

  bt = (B_Tree *) malloc(sizeof(B_Tree));
  bt->key_size = key_size;
  bt->disk = jdisk_create(filename, size);
  if (bt->disk == NULL) {
    free(bt);
    return NULL;
  }
  bt->size = size;
  bt->num_lbas = size / JDISK_SECTOR_SIZE;
  bt->keys_per_block = (JDISK_SECTOR_SIZE - 6) / (key_size + 4);
  bt->lbas_per_block = bt->keys_per_block + 1;
  bt->free_list = NULL;
  bt->tmp_e = NULL;
  bt->tmp_e_index = -1;
  bt->flush = 1;

  // Allocate root
  bt->first_free_block = 1;
  root_lba = 1;
  bt->root_lba = root_lba;
  bt->first_free_block = 2;

  // Write sector 0
  memset(sector0, 0, 1024);
  memcpy(sector0, &bt->key_size, 4);
  memcpy(sector0 + 4, &bt->root_lba, 4);
  memcpy(sector0 + 8, &bt->first_free_block, 8);
  jdisk_write(bt->disk, 0, sector0);

  // Create root node
  root = allocate_tree_node(bt);
  root->internal = 0;
  root->nkeys = 0;
  root->lba = root_lba;
  root->flush = 1;
  write_tree_node(bt, root);
  free_tree_node(bt, root);

  return bt;
}

void *b_tree_attach(char *filename) {
  B_Tree *bt;
  unsigned char sector0[1024];

  bt = (B_Tree *) malloc(sizeof(B_Tree));
  bt->disk = jdisk_attach(filename);
  if (bt->disk == NULL) {
    free(bt);
    return NULL;
  }
  bt->size = jdisk_size(bt->disk);
  bt->num_lbas = bt->size / JDISK_SECTOR_SIZE;

  jdisk_read(bt->disk, 0, sector0);
  memcpy(&bt->key_size, sector0, 4);
  memcpy(&bt->root_lba, sector0 + 4, 4);
  memcpy(&bt->first_free_block, sector0 + 8, 8);

  bt->keys_per_block = (JDISK_SECTOR_SIZE - 6) / (bt->key_size + 4);
  bt->lbas_per_block = bt->keys_per_block + 1;
  bt->free_list = NULL;
  bt->tmp_e = NULL;
  bt->tmp_e_index = -1;
  bt->flush = 0;

  return bt;
}

unsigned int b_tree_insert(void *b_tree, void *key, void *record) {
  B_Tree *bt = (B_Tree *) b_tree;
  Tree_Node *tn, *child;
  unsigned int lba, val_lba;
  int i, j, cmp;

  // Allocate val sector
  if (bt->first_free_block >= bt->num_lbas) return 0;
  val_lba = bt->first_free_block++;
  jdisk_write(bt->disk, val_lba, record);

  // Find the leaf
  tn = allocate_tree_node(bt);
  read_tree_node(bt, tn, bt->root_lba);
  while (tn->internal) {
    for (i = 0; i < tn->nkeys; i++) {
      cmp = memcmp(key, tn->keys[i], bt->key_size);
      if (cmp <= 0) break;
    }
    child = allocate_tree_node(bt);
    read_tree_node(bt, child, tn->lbas[i]);
    child->parent = tn;
    child->parent_index = i;
    free_and_flush(bt, tn);
    tn = child;
  }

  // Insert into leaf
  for (i = 0; i < tn->nkeys; i++) {
    cmp = memcmp(key, tn->keys[i], bt->key_size);
    if (cmp == 0) {
      // Replace
      tn->lbas[i] = val_lba;
      tn->flush = 1;
      free_and_flush(bt, tn);
      bt->flush = 1;
      return val_lba;
    }
    if (cmp < 0) break;
  }

  // Shift keys and lbas
  for (j = tn->nkeys; j > i; j--) {
    memcpy(tn->keys[j], tn->keys[j-1], bt->key_size);
    tn->lbas[j] = tn->lbas[j-1];
  }
  memcpy(tn->keys[i], key, bt->key_size);
  tn->lbas[i] = val_lba;
  tn->nkeys++;
  tn->flush = 1;

  // Split if necessary
  while (tn->nkeys > bt->keys_per_block) {
    // Split
    Tree_Node *new_tn = allocate_tree_node(bt);
    new_tn->internal = tn->internal;
    new_tn->lba = bt->first_free_block++;
    if (new_tn->lba >= bt->num_lbas) {
      free_tree_node(bt, new_tn);
      free_and_flush(bt, tn);
      return 0;
    }

    int mid = tn->nkeys / 2;
    new_tn->nkeys = tn->nkeys - mid - 1;
    tn->nkeys = mid;

    for (j = 0; j < new_tn->nkeys; j++) {
      memcpy(new_tn->keys[j], tn->keys[mid + 1 + j], bt->key_size);
      new_tn->lbas[j] = tn->lbas[mid + 1 + j];
    }
    new_tn->lbas[new_tn->nkeys] = tn->lbas[tn->nkeys + 1];
    new_tn->flush = 1;

    if (tn->parent == NULL) {
      // New root
      Tree_Node *new_root = allocate_tree_node(bt);
      new_root->internal = 1;
      new_root->nkeys = 1;
      memcpy(new_root->keys[0], tn->keys[mid], bt->key_size);
      new_root->lbas[0] = tn->lba;
      new_root->lbas[1] = new_tn->lba;
      new_root->lba = bt->first_free_block++;
      if (new_root->lba >= bt->num_lbas) {
        free_tree_node(bt, new_root);
        free_tree_node(bt, new_tn);
        free_and_flush(bt, tn);
        return 0;
      }
      bt->root_lba = new_root->lba;
      new_root->flush = 1;
      write_tree_node(bt, new_root);
      free_tree_node(bt, new_root);
    } else {
      // Insert into parent
      Tree_Node *parent = tn->parent;
      int pidx = tn->parent_index;
      for (j = parent->nkeys; j > pidx; j--) {
        memcpy(parent->keys[j], parent->keys[j-1], bt->key_size);
        parent->lbas[j+1] = parent->lbas[j];
      }
      memcpy(parent->keys[pidx], tn->keys[mid], bt->key_size);
      parent->lbas[pidx+1] = new_tn->lba;
      parent->nkeys++;
      parent->flush = 1;
      free_and_flush(bt, parent);
    }

    write_tree_node(bt, new_tn);
    free_tree_node(bt, new_tn);
    tn->flush = 1;
    write_tree_node(bt, tn);

    if (tn->parent == NULL) break;
    tn = tn->parent;
  }

  free_and_flush(bt, tn);

  // Update sector 0
  if (bt->flush) {
    unsigned char sector0[1024];
    memcpy(sector0, &bt->key_size, 4);
    memcpy(sector0 + 4, &bt->root_lba, 4);
    memcpy(sector0 + 8, &bt->first_free_block, 8);
    jdisk_write(bt->disk, 0, sector0);
    bt->flush = 0;
  }

  return val_lba;
}

unsigned int b_tree_find(void *b_tree, void *key) {
  B_Tree *bt = (B_Tree *) b_tree;
  Tree_Node *tn;
  int i, cmp;

  tn = allocate_tree_node(bt);
  read_tree_node(bt, tn, bt->root_lba);
  while (tn->internal) {
    for (i = 0; i < tn->nkeys; i++) {
      cmp = memcmp(key, tn->keys[i], bt->key_size);
      if (cmp <= 0) break;
    }
    unsigned int next_lba = tn->lbas[i];
    free_and_flush(bt, tn);
    tn = allocate_tree_node(bt);
    read_tree_node(bt, tn, next_lba);
  }

  for (i = 0; i < tn->nkeys; i++) {
    cmp = memcmp(key, tn->keys[i], bt->key_size);
    if (cmp == 0) {
      unsigned int lba = tn->lbas[i];
      free_and_flush(bt, tn);
      return lba;
    }
  }

  free_and_flush(bt, tn);
  return 0;
}

void *b_tree_disk(void *b_tree) {
  return ((B_Tree *) b_tree)->disk;
}

int b_tree_key_size(void *b_tree) {
  return ((B_Tree *) b_tree)->key_size;
}

void b_tree_print_tree(void *b_tree) {
  // Implement print tree
  printf("Print tree not implemented\n");
}
