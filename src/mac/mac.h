/* Mac Toolbox reimplementation: C API (addresses are emulated-memory addresses). */
#pragma once
#include <stdint.h>
#include "mem.h"
#include "lowmem.h"
#include "../cpu/cpu.h"

struct VNode;

/* ---- Memory Manager ---- */
void     mm_init(void);
maddr    mm_zone_for(int sys);
int      mm_heap_index(maddr zone);
maddr    mm_new_ptr_in(int heap, uint32_t size, int clear);
maddr    mm_new_handle_in(int heap, uint32_t size, int clear);
void     mm_set_memerr(int16_t e);
uint32_t mm_free_bytes(int heap, uint32_t *largest);
int      mm_is_block(maddr p);
int      mm_is_handle(maddr h);
extern void (*mm_purge_notify)(maddr hnd);
maddr NewPtr(uint32_t size);
maddr NewPtrClear(uint32_t size);
maddr NewPtrSys(uint32_t size);
void  DisposePtr(maddr p);
uint32_t GetPtrSize(maddr p);
void  SetPtrSize(maddr p, uint32_t size);
maddr NewHandle(uint32_t size);
maddr NewHandleClear(uint32_t size);
maddr NewHandleSys(uint32_t size);
maddr NewEmptyHandle(void);
void  DisposeHandle(maddr h);
uint32_t GetHandleSize(maddr h);
void  SetHandleSize(maddr h, uint32_t size);
uint8_t HGetState(maddr h);
void  HSetState(maddr h, uint8_t st);
void  HLock(maddr h);
void  HUnlock(maddr h);
void  HPurge(maddr h);
void  HNoPurge(maddr h);
void  HSetRBit(maddr h);
void  HClrRBit(maddr h);
maddr RecoverHandle(maddr p);
void  EmptyHandle(maddr h);
uint32_t FreeMem(void);
uint32_t MaxMem(uint32_t *grow);
uint32_t CompactMem(uint32_t need);
void  PurgeMem(uint32_t need);

/* ---- Resource Manager ---- */
int16_t rsrc_open_node(struct VNode *n, int8_t perm);
int16_t rsrc_open_system(const uint8_t *data, uint32_t len);
void    rsrc_add_system(uint32_t type, int16_t id, const char *name, const void *data, uint32_t len);
extern void (*rsrc_missing_hook)(uint32_t type, int16_t id);
int32_t rsrc_size(maddr h);
int16_t ResError(void);
int16_t CurResFile(void);
void    UseResFile(int16_t r);
maddr   GetResource(uint32_t type, int16_t id);
maddr   Get1Resource(uint32_t type, int16_t id);
maddr   GetNamedResource(uint32_t type, const char *name);
maddr   Get1NamedResource(uint32_t type, const char *name);
int16_t CountResources(uint32_t type);
int16_t Count1Resources(uint32_t type);
maddr   GetIndResource(uint32_t type, int16_t idx);
maddr   Get1IndResource(uint32_t type, int16_t idx);
int16_t CountTypes(void);
int16_t Count1Types(void);
uint32_t GetIndType(int16_t idx);
uint32_t Get1IndType(int16_t idx);
void    LoadResource(maddr h);
void    ReleaseResource(maddr h);
void    DetachResource(maddr h);
void    GetResInfo(maddr h, int16_t *id, uint32_t *type, char *name);
void    SetResInfo(maddr h, int16_t id, const char *name);
int16_t GetResAttrs(maddr h);
void    SetResAttrs(maddr h, int16_t a);
int32_t GetResourceSizeOnDisk(maddr h);
int16_t HomeResFile(maddr h);
void    ChangedResource(maddr h);
void    AddResource(maddr h, uint32_t type, int16_t id, const char *name);
void    RemoveResource(maddr h);
void    WriteResource(maddr h);
void    UpdateResFile(int16_t r);
void    CloseResFile(int16_t r);
void    CreateResFile_node(struct VNode *n);
void    SetResLoad(int load);
uint16_t GetResFileAttrs(int16_t r);
void    SetResFileAttrs(int16_t r, uint16_t a);

/* ---- File Manager ---- */
uint32_t fm_default_dir(void);
void     fm_set_default_dir(uint32_t d);
int16_t  fm_open_wd(uint32_t dirid, int32_t procid);
int      fm_close_all(void);
int16_t  fm_alloc_fcb(struct VNode *n, int fork);
void     fm_free_fcb(int16_t ref);
#define PB(n) int16_t n(maddr pb, int async)
PB(PBOpen); PB(PBHOpen); PB(PBOpenRF); PB(PBHOpenRF); PB(PBClose); PB(PBRead); PB(PBWrite);
PB(PBGetEOF); PB(PBSetEOF); PB(PBGetFPos); PB(PBSetFPos); PB(PBCreate); PB(PBHCreate); PB(PBDirCreate);
PB(PBDelete); PB(PBHDelete); PB(PBRename); PB(PBHRename); PB(PBGetFInfo); PB(PBHGetFInfo); PB(PBGetCatInfo);
PB(PBSetFInfo); PB(PBHSetFInfo); PB(PBSetCatInfo); PB(PBSetFLock); PB(PBRstFLock); PB(PBHSetFLock); PB(PBHRstFLock);
PB(PBGetVInfo); PB(PBHGetVInfo); PB(PBGetVol); PB(PBHGetVol); PB(PBSetVol); PB(PBHSetVol); PB(PBOpenWD);
PB(PBCloseWD); PB(PBGetFCBInfo); PB(PBGetWDInfo); PB(PBFlushVol); PB(PBFlushFile);
#undef PB
int16_t FSMakeFSSpec(int16_t vref, uint32_t dirid, const char *name, maddr spec);
struct VNode *fsspec_node(maddr spec, uint32_t *parid, char *leaf);
int16_t FSpOpenFork(maddr spec, int fork, int8_t perm, int16_t *ref);
int16_t FSpCreate(maddr spec, uint32_t creator, uint32_t type, int16_t script);
int16_t FSpDelete(maddr spec);
int16_t FSpGetFInfo(maddr spec, maddr finfo);
int16_t FSpSetFInfo(maddr spec, maddr finfo);
int16_t FSpRename(maddr spec, const char *nn);
int16_t FSpOpenResFile(maddr spec, int8_t perm);
void    FSpCreateResFile(maddr spec, uint32_t creator, uint32_t type, int16_t script);
int16_t HOpenResFile(int16_t vref, uint32_t dirid, const char *name, int8_t perm);
int16_t OpenResFile(const char *name);
void    HCreateResFile(int16_t vref, uint32_t dirid, const char *name);
void    CreateResFile(const char *name);

/* ---- Segment Loader / process ---- */
int  loader_launch(const char *app_path_in_volume);
void LoadSeg(int16_t seg);
void ExitToShell(void);

/* ---- trap glue registration (one per manager) ---- */
void traps_mem_init(void);
void traps_rsrc_init(void);
void traps_file_init(void);
void traps_misc_init(void);
void traps_qd_init(void);
void traps_wm_init(void);
void traps_event_init(void);
void traps_sound_init(void);
void traps_time_init(void);

/* four-char codes */
#define FOURCC(a,b,c,d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
