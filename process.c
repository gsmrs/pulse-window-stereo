#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <dirent.h>

#include "arena.h"

typedef struct ProcessNode {
    pid_t pid;
    struct ProcessNode *next;
    struct ProcessNode *children;
} ProcessNode;

#define EXP_MAX_PROCESSES 12
#define NUM_MAX_PROCESSES (1 << EXP_MAX_PROCESSES)

typedef struct {
    ProcessNode *root;
    int32_t count;

    Arena *arena;

    // MSI hashtable mapping PID -> ProcessNode*
    ProcessNode *ht[NUM_MAX_PROCESSES];
} ProcessTree;

bool
is_numeric(const char *s) {
    char ch;
    while ((ch = *s++)) {
        if (!isdigit(ch)) {
            return false;
        }
    }
    return true;
}

static int32_t
ht_lookup(uint64_t hash, int exp, int32_t index) {
    uint32_t mask = ((uint32_t)1 << exp) - 1;
    uint32_t step = (hash >> (64 - exp)) | 1;
    return (index + step) & mask;
}

static ProcessNode *
process_tree_get(ProcessTree *tree, pid_t pid) {
    // hash(pid) = pid
    for (int32_t index = pid, count = 0; count < NUM_MAX_PROCESSES; count++) {
        index = ht_lookup((uint64_t) pid, EXP_MAX_PROCESSES, index);
        if (!tree->ht[index]) {
            // empty slot found, create new node and return
            ProcessNode *node = ARENA_ALLOC1(tree->arena, ProcessNode);
            assert(node);
            node->pid = pid;
            tree->ht[index] = node;
            return node;
        }
        else if (tree->ht[index]->pid == pid) {
            // found
            return tree->ht[index];
        }
    }
    assert(0 && "map full");
}

static void
process_tree_insert(ProcessTree *tree, pid_t pid, pid_t parent_pid) {
    tree->count++;
    ProcessNode *node = process_tree_get(tree, pid);
    ProcessNode *parent = process_tree_get(tree, parent_pid);

    node->next = parent->children;
    parent->children = node;
}

static void
load_process_tree(ProcessTree *tree, Arena *arena) {
    DIR *dir = opendir("/proc");
    assert(dir);

    tree->arena = arena;
    tree->count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if ((entry->d_type == DT_DIR)
                && (strcmp(entry->d_name, ".") != 0)
                && (strcmp(entry->d_name, "..") != 0)
                && is_numeric(entry->d_name))
        {
            pid_t pid = atoi(entry->d_name);
            char stat_file_path[PATH_MAX];
            snprintf(stat_file_path, sizeof(stat_file_path),
                    "/proc/%d/stat", pid);
            FILE *f = fopen(stat_file_path, "rb");
            if (f) {
                pid_t parent_pid;
                int matched = fscanf(f, "%*s %*s %*s %d", &parent_pid);
                fclose(f);
                if (matched) {
                    process_tree_insert(tree, pid, parent_pid);
                }
            } else {
                // couldn't open process file, probably short-lived process
                // which was still alive during readdir() but isn't anymore
            }
        }
    }

    closedir(dir);
}

void
debug_dump_process_node(ProcessNode *node, int depth) {
    if (!node) {
        return;
    }

    for (int i = 0; i < depth; i++) printf("  ");

    printf("Process(%d)\n", node->pid);
    for (ProcessNode *child = node->children; child; child = child->next) {
        debug_dump_process_node(child, depth + 1);
    }
}

static void
collect_children(ProcessNode *node, pid_t *children, int32_t capacity, int32_t *count) {
    assert((*count) < capacity);
    children[(*count)++] = node->pid;

    for (ProcessNode *child = node->children; child; child = child->next) {
        collect_children(child, children, capacity, count);
    }
}

int32_t
get_children_recursive(Arena *arena, pid_t parent, pid_t **children) {
    ProcessTree tree = {0};
    load_process_tree(&tree, arena);
    ProcessNode *node = process_tree_get(&tree, parent);
    *children = ARENA_ALLOC_ARRAY(arena, pid_t, tree.count);
    int32_t child_count = 0;
    collect_children(node, *children, tree.count, &child_count);
    return child_count;
}

