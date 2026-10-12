/* Virtual file system: an HFS disk image read into memory, plus a writable
 * overlay directory on the host for files the game creates or changes. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct VNode {
    uint32_t cnid, parid;
    char name[32];               /* Mac Roman, no length byte */
    int isdir;
    uint8_t finfo[16];           /* FInfo / DInfo */
    uint8_t fxinfo[16];
    uint32_t crdate, mddate;
    uint8_t *fork[2];            /* 0 = data, 1 = resource */
    uint32_t forklen[2], forkcap[2];
    int dirty, deleted, locked;
    struct VNode *next;          /* all nodes, linked */
} VNode;

typedef struct {
    char volname[32];
    uint32_t crdate;
    VNode *nodes;
    uint32_t next_cnid;
    char overlay_dir[1024];
} Volume;

extern Volume g_vol;

int    vfs_mount_hfs(const char *image_path, const char *overlay_dir);
VNode *vfs_by_id(uint32_t cnid);
VNode *vfs_lookup(uint32_t parid, const char *name);     /* name: Mac Roman, case-insensitive */
VNode *vfs_resolve_path(uint32_t dirid, const char *path, uint32_t *out_parid, char *out_leaf);
VNode *vfs_child_index(uint32_t parid, int index);         /* 1-based */
int    vfs_child_count(uint32_t parid);
VNode *vfs_create(uint32_t parid, const char *name, int isdir);
int    vfs_delete(VNode *n);
int    vfs_rename(VNode *n, const char *newname);
void   vfs_set_fork_len(VNode *n, int fork, uint32_t len);
void   vfs_flush(VNode *n);       /* write a dirty node to the overlay */
void   vfs_flush_all(void);
int    mac_strcasecmp(const char *a, const char *b);
uint32_t vfs_mac_time_now(void);
