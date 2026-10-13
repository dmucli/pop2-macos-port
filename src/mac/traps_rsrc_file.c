/* Trap glue: Resource Manager (Pascal stack) and File Manager (A0 = param block). */
#include "mac.h"
#include "../fs/vfs.h"
#include <stdio.h>
#include <stdlib.h>

static void popstr(char *out) { pstr_to_c(pop32(), out, 256); }

static void T_GetResource(uint16_t w)  { (void)w; int16_t id = pops16(); uint32_t t = pop32(); ret32(GetResource(t, id)); }
static void T_Get1Resource(uint16_t w) { (void)w; int16_t id = pops16(); uint32_t t = pop32(); ret32(Get1Resource(t, id)); }
static void T_GetNamedResource(uint16_t w)  { (void)w; char n[256]; popstr(n); uint32_t t = pop32(); ret32(GetNamedResource(t, n)); }
static void T_Get1NamedResource(uint16_t w) { (void)w; char n[256]; popstr(n); uint32_t t = pop32(); ret32(Get1NamedResource(t, n)); }
static void T_GetIndResource(uint16_t w)  { (void)w; int16_t i = pops16(); uint32_t t = pop32(); ret32(GetIndResource(t, i)); }
static void T_Get1IndResource(uint16_t w) { (void)w; int16_t i = pops16(); uint32_t t = pop32(); ret32(Get1IndResource(t, i)); }
static void T_CountResources(uint16_t w)  { (void)w; uint32_t t = pop32(); ret16((uint16_t)CountResources(t)); }
static void T_Count1Resources(uint16_t w) { (void)w; uint32_t t = pop32(); ret16((uint16_t)Count1Resources(t)); }
static void T_CountTypes(uint16_t w)  { (void)w; ret16((uint16_t)CountTypes()); }
static void T_Count1Types(uint16_t w) { (void)w; ret16((uint16_t)Count1Types()); }
static void T_GetIndType(uint16_t w)  { (void)w; int16_t i = pops16(); maddr p = pop32(); wr32(p, GetIndType(i)); }
static void T_Get1IndType(uint16_t w) { (void)w; int16_t i = pops16(); maddr p = pop32(); wr32(p, Get1IndType(i)); }
static void T_LoadResource(uint16_t w)    { (void)w; LoadResource(pop32()); }
static void T_ReleaseResource(uint16_t w) { (void)w; ReleaseResource(pop32()); }
static void T_DetachResource(uint16_t w)  { (void)w; DetachResource(pop32()); }
static void T_GetResInfo(uint16_t w) {
    (void)w; maddr name = pop32(), type = pop32(), id = pop32(), h = pop32();
    int16_t i = 0; uint32_t t = 0; char n[256] = "";
    GetResInfo(h, &i, &t, n);
    if (!ResError()) { if (id) wr16(id, (uint16_t)i); if (type) wr32(type, t); if (name) c_to_pstr(n, name); }
}
static void T_SetResInfo(uint16_t w) { (void)w; maddr name = pop32(); int16_t id = pops16(); maddr h = pop32(); char n[256]; if (name) pstr_to_c(name, n, sizeof n); SetResInfo(h, id, name ? n : NULL); }
static void T_GetResAttrs(uint16_t w) { (void)w; ret16((uint16_t)GetResAttrs(pop32())); }
static void T_SetResAttrs(uint16_t w) { (void)w; int16_t a = pops16(); SetResAttrs(pop32(), a); }
static void T_SizeRsrc(uint16_t w) { (void)w; ret32((uint32_t)GetResourceSizeOnDisk(pop32())); }
static void T_HomeResFile(uint16_t w) { (void)w; ret16((uint16_t)HomeResFile(pop32())); }
static void T_ChangedResource(uint16_t w) { (void)w; ChangedResource(pop32()); }
static void T_AddResource(uint16_t w) { (void)w; char n[256]; maddr np = pop32(); int16_t id = pops16(); uint32_t t = pop32(); maddr h = pop32(); if (np) pstr_to_c(np, n, sizeof n); else n[0] = 0; AddResource(h, t, id, n); }
static void T_RemoveResource(uint16_t w) { (void)w; RemoveResource(pop32()); }
static void T_WriteResource(uint16_t w) { (void)w; WriteResource(pop32()); }
static void T_UpdateResFile(uint16_t w) { (void)w; UpdateResFile(pops16()); }
static void T_CloseResFile(uint16_t w) { (void)w; CloseResFile(pops16()); }
static void T_CurResFile(uint16_t w) { (void)w; ret16((uint16_t)CurResFile()); }
static void T_UseResFile(uint16_t w) { (void)w; UseResFile(pops16()); }
static void T_ResError(uint16_t w) { (void)w; ret16((uint16_t)ResError()); }
static void T_SetResLoad(uint16_t w) { (void)w; SetResLoad(pop8()); }
static void T_OpenResFile(uint16_t w) { (void)w; char n[256]; popstr(n); ret16((uint16_t)OpenResFile(n)); }
static void T_OpenRFPerm(uint16_t w) { (void)w; int8_t perm = (int8_t)pop8(); int16_t vref = pops16(); char n[256]; popstr(n); ret16((uint16_t)HOpenResFile(vref, 0, n, perm)); }
static void T_CreateResFile(uint16_t w) { (void)w; char n[256]; popstr(n); CreateResFile(n); }
static void T_GetResFileAttrs(uint16_t w) { (void)w; ret16(GetResFileAttrs(pops16())); }
static void T_SetResFileAttrs(uint16_t w) { (void)w; uint16_t a = pop16(); SetResFileAttrs(pops16(), a); }
static void T_SetResourceSize(uint16_t w) { (void)w; pop32(); pop32(); }
static void T_SetResPurge(uint16_t w) { (void)w; pop16(); }
static void T_UniqueID(uint16_t w) { (void)w; uint32_t t = pop32(); int16_t id; do { id = (int16_t)(128 + (rand() & 0x3FFF)); } while (GetResource(t, id)); ret16((uint16_t)id); }

/* HOpenResFile(vRefNum, dirID, fileName, permission) via Pack? no: trap A81A */
static void T_HOpenResFile(uint16_t w) { (void)w; int8_t perm = (int8_t)pop8(); char n[256]; popstr(n); uint32_t dir = pop32(); int16_t v = pops16(); ret16((uint16_t)HOpenResFile(v, dir, n, perm)); }
static void T_HCreateResFile(uint16_t w) { (void)w; char n[256]; popstr(n); uint32_t dir = pop32(); int16_t v = pops16(); HCreateResFile(v, dir, n); }

/* ---- File Manager ---- */
#define ASYNC(w) (((w) & 0x0400) != 0)
#define HFS(w)   (((w) & 0x0200) != 0)
#define FMR(e) cpu_set(R_D0, (uint32_t)(int32_t)(e))
static void T_Open(uint16_t w)     { FMR(HFS(w) ? PBHOpen(A0, ASYNC(w)) : PBOpen(A0, ASYNC(w))); }
static void T_OpenRF(uint16_t w)   { FMR(HFS(w) ? PBHOpenRF(A0, ASYNC(w)) : PBOpenRF(A0, ASYNC(w))); }
static void T_Close(uint16_t w)    { FMR(PBClose(A0, ASYNC(w))); }
static void T_Read(uint16_t w)     { FMR(PBRead(A0, ASYNC(w))); }
static void T_Write(uint16_t w)    { FMR(PBWrite(A0, ASYNC(w))); }
static void T_GetVInfo(uint16_t w) { FMR(HFS(w) ? PBHGetVInfo(A0, ASYNC(w)) : PBGetVInfo(A0, ASYNC(w))); }
static void T_Create(uint16_t w)   { FMR(HFS(w) ? PBHCreate(A0, ASYNC(w)) : PBCreate(A0, ASYNC(w))); }
static void T_Delete(uint16_t w)   { FMR(HFS(w) ? PBHDelete(A0, ASYNC(w)) : PBDelete(A0, ASYNC(w))); }
static void T_Rename(uint16_t w)   { FMR(HFS(w) ? PBHRename(A0, ASYNC(w)) : PBRename(A0, ASYNC(w))); }
static void T_GetFInfo(uint16_t w) { FMR(HFS(w) ? PBHGetFInfo(A0, ASYNC(w)) : PBGetFInfo(A0, ASYNC(w))); }
static void T_SetFInfo(uint16_t w) { FMR(HFS(w) ? PBHSetFInfo(A0, ASYNC(w)) : PBSetFInfo(A0, ASYNC(w))); }
static void T_GetEOF(uint16_t w)   { FMR(PBGetEOF(A0, ASYNC(w))); }
static void T_SetEOF(uint16_t w)   { FMR(PBSetEOF(A0, ASYNC(w))); }
static void T_GetFPos(uint16_t w)  { FMR(PBGetFPos(A0, ASYNC(w))); }
static void T_SetFPos(uint16_t w)  { FMR(PBSetFPos(A0, ASYNC(w))); }
static void T_FlushVol(uint16_t w) { FMR(PBFlushVol(A0, ASYNC(w))); }
static void T_FlushFile(uint16_t w){ FMR(PBFlushFile(A0, ASYNC(w))); }
static void T_GetVol(uint16_t w)   { FMR(HFS(w) ? PBHGetVol(A0, ASYNC(w)) : PBGetVol(A0, ASYNC(w))); }
static void T_SetVol(uint16_t w)   { FMR(HFS(w) ? PBHSetVol(A0, ASYNC(w)) : PBSetVol(A0, ASYNC(w))); }
static void T_SetFLock(uint16_t w) { FMR(HFS(w) ? PBHSetFLock(A0, ASYNC(w)) : PBSetFLock(A0, ASYNC(w))); }
static void T_RstFLock(uint16_t w) { FMR(HFS(w) ? PBHRstFLock(A0, ASYNC(w)) : PBRstFLock(A0, ASYNC(w))); }
static void T_noerr_pb(uint16_t w) { (void)w; wr16(A0 + 16, 0); FMR(0); }
static void T_Eject(uint16_t w)    { (void)w; wr16(A0 + 16, 0); FMR(0); }
static void T_Unmount(uint16_t w)  { (void)w; wr16(A0 + 16, 0); FMR(0); }
static void T_Mount(uint16_t w)    { (void)w; wr16(A0 + 16, (uint16_t)-56); FMR(-56); }  /* nsDrvErr */
static void T_Control(uint16_t w) {
    maddr pb = A0;
    fprintf(stderr, "[file] Control ref=%d code=%d\n", (int16_t)rd16(pb + 24), (int16_t)rd16(pb + 26));
    (void)w; wr16(pb + 16, (uint16_t)-17); FMR(-17);   /* controlErr */
}
static void T_Status(uint16_t w) { (void)w; wr16(A0 + 16, (uint16_t)-18); FMR(-18); }

static void T_HFSDispatch(uint16_t w) {
    int async = ASYNC(w);
    maddr pb = A0;
    switch (D0 & 0xFF) {
        case 0x01: FMR(PBOpenWD(pb, async)); break;
        case 0x02: FMR(PBCloseWD(pb, async)); break;
        case 0x06: FMR(PBDirCreate(pb, async)); break;
        case 0x07: FMR(PBGetWDInfo(pb, async)); break;
        case 0x08: FMR(PBGetFCBInfo(pb, async)); break;
        case 0x09: FMR(PBGetCatInfo(pb, async)); break;
        case 0x0A: FMR(PBSetCatInfo(pb, async)); break;
        case 0x1A: FMR(HFS(w) ? PBHOpen(pb, async) : PBOpen(pb, async)); break;      /* [H]OpenDF */
        case 0x1B: FMR(HFS(w) ? PBHOpenRF(pb, async) : PBOpenRF(pb, async)); break;  /* [H]OpenRF */
        case 0x30: {                                     /* GetVolParms */
            maddr buf = rd32(pb + 32);
            uint32_t n = rd32(pb + 36);
            uint8_t parms[14] = {0, 1, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            if (buf) mput(buf, parms, n < 14 ? n : 14);
            wr32(pb + 40, n < 14 ? n : 14);
            wr16(pb + 16, 0); FMR(0);
            break;
        }
        default:
            fprintf(stderr, "[file] HFSDispatch selector %d unimplemented\n", (int)(D0 & 0xFF));
            wr16(pb + 16, (uint16_t)-50); FMR(-50);
    }
}

/* HighLevelFSDispatch (FSSpec calls), selector in D0, Pascal stack args */
static void T_HighLevelFSDispatch(uint16_t w) {
    (void)w;
    int16_t err = 0;
    switch (D0 & 0xFF) {
        case 0x01: { maddr spec = pop32(); char n[256]; popstr(n); uint32_t dir = pop32(); int16_t v = pops16(); err = FSMakeFSSpec(v, dir, n, spec); break; }
        case 0x02: case 0x03: { maddr ref = pop32(); int8_t perm = (int8_t)pop8(); maddr spec = pop32(); int16_t r = 0;
                                err = FSpOpenFork(spec, (D0 & 0xFF) == 3, perm, &r); wr16(ref, (uint16_t)r); break; }
        case 0x04: { int16_t sc = pops16(); uint32_t type = pop32(), cr = pop32(); maddr spec = pop32(); err = FSpCreate(spec, cr, type, sc); break; }
        case 0x05: { maddr did = pop32(); pop16(); maddr spec = pop32(); uint32_t parid; char leaf[64];
                     struct VNode *n = fsspec_node(spec, &parid, leaf);
                     if (n) err = -48; else { n = vfs_create(parid, leaf, 1); if (n) wr32(did, n->cnid); else err = -120; } break; }
        case 0x06: err = FSpDelete(pop32()); break;
        case 0x07: { maddr fi = pop32(); err = FSpGetFInfo(pop32(), fi); break; }
        case 0x08: { maddr fi = pop32(); err = FSpSetFInfo(pop32(), fi); break; }
        case 0x09: case 0x0A: { maddr spec = pop32(); struct VNode *n = fsspec_node(spec, NULL, NULL); if (n) n->locked = (D0 & 0xFF) == 9; else err = -43; break; }
        case 0x0B: { char n[256]; popstr(n); err = FSpRename(pop32(), n); break; }
        case 0x0D: { int8_t perm = (int8_t)pop8(); maddr spec = pop32(); ret16((uint16_t)FSpOpenResFile(spec, perm)); return; }
        case 0x0E: { int16_t sc = pops16(); uint32_t type = pop32(), cr = pop32(); FSpCreateResFile(pop32(), cr, type, sc); return; }
        default:
            fprintf(stderr, "[file] HighLevelFSDispatch selector %d unimplemented\n", (int)(D0 & 0xFF));
            err = -50;
            break;
    }
    ret16((uint16_t)err);
}


/* ResourceDispatch (A822): partial resource access, selector in D0. Works on the
 * resource's handle (loaded first); writes mark the resource changed. */
void ChangedResource(maddr h);
static void T_ResourceDispatch(uint16_t w) {
    (void)w;
    uint32_t sel = cpu_get(R_D0) & 0xF;
    if (sel == 1 || sel == 2) {          /* Read/WritePartialResource(h, offset, buffer, count) */
        uint32_t count = pop32(); maddr buf = pop32(); uint32_t off = pop32(); maddr h = pop32();
        if (!h || !rd32(h)) { LoadResource(h); }
        if (!h || !rd32(h)) { wr16(LM_ResErr, (uint16_t)-192); return; }
        uint32_t size = GetHandleSize(h);
        if (off + count > size) {
            if (sel == 1) { wr16(LM_ResErr, (uint16_t)-188); return; }   /* inputOutOfBounds */
            SetHandleSize(h, off + count);
        }
        if (sel == 1) mcopy(buf, rd32(h) + off, count);
        else { mcopy(rd32(h) + off, buf, count); ChangedResource(h); }
        wr16(LM_ResErr, 0);
    } else if (sel == 3) {               /* SetResourceSize(h, newSize) */
        uint32_t size = pop32(); maddr h = pop32();
        if (!h) { wr16(LM_ResErr, (uint16_t)-192); return; }
        if (!rd32(h)) LoadResource(h);
        if (rd32(h)) SetHandleSize(h, size);
        ChangedResource(h);
        wr16(LM_ResErr, 0);
    } else {
        fprintf(stderr, "[rsrc] ResourceDispatch selector %u unsupported\n", sel);
    }
}
void traps_rsrc_init(void) {
    trap_register(0xA822, T_ResourceDispatch);
    trap_register(0xA9A0, T_GetResource);
    trap_register(0xA81F, T_Get1Resource);
    trap_register(0xA9A1, T_GetNamedResource);
    trap_register(0xA820, T_Get1NamedResource);
    trap_register(0xA99D, T_GetIndResource);
    trap_register(0xA80E, T_Get1IndResource);
    trap_register(0xA99C, T_CountResources);
    trap_register(0xA80D, T_Count1Resources);
    trap_register(0xA99E, T_CountTypes);
    trap_register(0xA81C, T_Count1Types);
    trap_register(0xA99F, T_GetIndType);
    trap_register(0xA80F, T_Get1IndType);
    trap_register(0xA9A2, T_LoadResource);
    trap_register(0xA9A3, T_ReleaseResource);
    trap_register(0xA992, T_DetachResource);
    trap_register(0xA9A8, T_GetResInfo);
    trap_register(0xA9A9, T_SetResInfo);
    trap_register(0xA9A6, T_GetResAttrs);
    trap_register(0xA9A7, T_SetResAttrs);
    trap_register(0xA9A5, T_SizeRsrc);
    trap_register(0xA9A4, T_HomeResFile);
    trap_register(0xA9AA, T_ChangedResource);
    trap_register(0xA9AB, T_AddResource);
    trap_register(0xA9AD, T_RemoveResource);
    trap_register(0xA9B0, T_WriteResource);
    trap_register(0xA999, T_UpdateResFile);
    trap_register(0xA99A, T_CloseResFile);
    trap_register(0xA994, T_CurResFile);
    trap_register(0xA998, T_UseResFile);
    trap_register(0xA9AF, T_ResError);
    trap_register(0xA99B, T_SetResLoad);
    trap_register(0xA997, T_OpenResFile);
    trap_register(0xA9C4, T_OpenRFPerm);
    trap_register(0xA9B1, T_CreateResFile);
    trap_register(0xA9F6, T_GetResFileAttrs);
    trap_register(0xA9F7, T_SetResFileAttrs);
    trap_register(0xA82B, T_SetResourceSize);
    trap_register(0xA993, T_SetResPurge);
    trap_register(0xA9C1, T_UniqueID);
    trap_register(0xA81A, T_HOpenResFile);
    trap_register(0xA81B, T_HCreateResFile);
}

void traps_file_init(void) {
    trap_register(0xA000, T_Open);
    trap_register(0xA00A, T_OpenRF);
    trap_register(0xA001, T_Close);
    trap_register(0xA002, T_Read);
    trap_register(0xA003, T_Write);
    trap_register(0xA004, T_Control);
    trap_register(0xA005, T_Status);
    trap_register(0xA007, T_GetVInfo);
    trap_register(0xA008, T_Create);
    trap_register(0xA009, T_Delete);
    trap_register(0xA00B, T_Rename);
    trap_register(0xA00C, T_GetFInfo);
    trap_register(0xA00D, T_SetFInfo);
    trap_register(0xA00E, T_Unmount);
    trap_register(0xA00F, T_Mount);
    trap_register(0xA011, T_GetEOF);
    trap_register(0xA012, T_SetEOF);
    trap_register(0xA013, T_FlushVol);
    trap_register(0xA014, T_GetVol);
    trap_register(0xA015, T_SetVol);
    trap_register(0xA017, T_Eject);
    trap_register(0xA018, T_GetFPos);
    trap_register(0xA041, T_SetFLock);
    trap_register(0xA042, T_RstFLock);
    trap_register(0xA044, T_SetFPos);
    trap_register(0xA045, T_FlushFile);
    trap_register(0xA010, T_noerr_pb);    /* Allocate */
    trap_register(0xA060, T_HFSDispatch);
    trap_register(0xAA52, T_HighLevelFSDispatch);
}
