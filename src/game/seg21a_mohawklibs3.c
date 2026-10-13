/* CODE 21 "Mohawk Libs 3", first part (0x5fb64-0x63134): the Mohawk file layer.
 *
 * A thin portable file API over the Mac File Manager: files are named by a "fileSpec"
 * (0x106 bytes: +0 vRefNum, +2 dirID, +6 name, a C string or, once qualified, a Pascal
 * string), errors are Mohawk codes (0x283c...) mapped from OSErrs by fileMapOSError and
 * kept in gFileError, and every failing call can be retried through an optional
 * exception handler (fileCheckException). Most calls go through one global parameter
 * block (gFilePB), whose ioNamePtr often points into the caller's stack frame: the
 * locals of these functions therefore live at their original A6 offsets.
 * The end of the range (getMaster/findEmptySlot) belongs to the memory layer.
 *
 * Ported from the Ghidra disassembly; addresses in comments are Ghidra addresses. */
#include "port.h"
#include "gen/proto.h"
#include "gen/traps_c.h"

/* ---- globals (Ghidra addresses) ---- */
#define gFileDriveNum   GH(0x107240)   /* word: drive searched by driveVolumeEnumProc */
#define gFileInitCount  GH(0x107242)   /* word: fileInit nesting count (0x392 bytes of state from here) */
#define gFileFlags      GH(0x107244)   /* word: fileInit argument (0 -> 0xffff) */
#define gFileOSError    GH(0x107246)   /* word: last OSErr seen by fileMapOSError */
#define gFileError      GH(0x107248)   /* word: last Mohawk file error (fileGetError) */
#define gFileLaunchDir  GH(0x10724a)   /* fileSpec (0x106): the directory at fileInit time */
#define gFileCurVRefNum GH(0x107350)   /* word: current directory's volume (fileSetDir) */
#define gFileCurDirID   GH(0x107352)   /* long: current directory's ID */
#define gFileXceptClass GH(0x107356)   /* byte: error class passed to the exception proc */
#define gFileXceptOff   GH(0x107358)   /* word: nonzero = no exception proc calls (fileSetXceptFail) */
#define gFileXceptVol   GH(0x10735c)   /* word: volume of the current operation */
#define gFileXceptMsg   GH(0x10735e)   /* long: message buffer for the exception proc (-> gFileMsgBuf) */
#define gFileXceptProc  GH(0x107362)   /* long: Pascal exception proc(VAR class): Boolean retry */
#define gFileMsgBuf     GH(0x107366)   /* 256 bytes */
#define gFilePB         GH(0x107466)   /* 0x6c bytes: the shared File Manager parameter block */
#define gFileCurPath    GH(0x1074d2)   /* Str255: full path of the current directory */
#define gFileAtRoot     GH(0x1075d2)   /* word: 1 when the current directory is a volume root */
#define gFileStr5d6     GH(0x1075d6)   /* ".Sony" (floppy driver name) */
#define gFileStr5dc     GH(0x1075dc)   /* Pascal ".Sony", opened for its drive-info control call */
#define gFileStr5e4     GH(0x1075e4)   /* ".AppleCD" */
#define gFileStr5ee     GH(0x1075ee)   /* ".AFPTranslator" */
#define gFileStr5fe     GH(0x1075fe)   /* Pascal ":a" (dummy file name: FSMakeFSSpec gives the dir ID) */
#define gFileStr602     GH(0x107602)   /* Pascal ":a" */
#define gFileStr606     GH(0x107606)   /* ".." */
#define gFileStr60a     GH(0x10760a)   /* ":" */
#define gFileStr60c     GH(0x10760c)   /* "../" */
#define gFileStr610     GH(0x107610)   /* "./" */
#define gFileStr614     GH(0x107614)   /* "::" */
#define gFileStr618     GH(0x107618)   /* Pascal "::" */
#define gMemError       GH(0x10761e)   /* word: memory layer error */
#define gMemPrevError   GH(0x107620)   /* word: previous memory layer error */
#define gMemPageCount   GH(0x107624)   /* word: number of master-pointer pages */
#define gMemPages       GH(0x107626)   /* long[]: pages of 0x200 master records of 14 bytes */

/* Low memory */
#define UTableBase      0x11c
#define DrvQHdr         0x308
#define VCBQHdr         0x356

#define FERR_BADSPEC    0x2843          /* invalid / unqualified fileSpec */
#define FERR_NODRIVE    0x286f

/* ---- stack fidelity ----
 * qualify and fileGetDir copy a whole FSSpec (0x46 bytes) from their frame into a
 * fileSpec, including the name bytes after the name, which are stale stack contents left
 * by earlier calls (saved A6 and registers, return addresses, arguments). For the copy to
 * match the original, the port reproduces those stack writes too:
 *  - ENTER = ENTRY_LINK plus the stack image of "link A6; movem.l <mask>,-(SP)": the
 *    caller's A6 at (A6) and the register images below the locals (mask bit n = Dn,
 *    bit 8+n = An, as in the movem). CPU A6 points to the frame while the function runs
 *    and the saved registers are restored on return (callee-saved, like the movem).
 *  - RL/RW/RB mirror the original's loads of D3-D7/A2-A4, so the callees save the same
 *    register values.
 *  - JSR(ret, FN_x, call) writes the original return address (Ghidra address `ret`, the
 *    instruction after the jsr) into the slot below the arguments after the call.
 *  - ASYNC: the "clr.b -(SP)" async flag of the PB glue writes only the high byte of its
 *    word; the low byte keeps what was in memory. */
typedef struct { uint32_t r[16]; unsigned mask; } SavedRegs;
static void frame_leave(SavedRegs *s) {
    for (int r = 0; r < 16; r++)
        if (s->mask & (1u << r)) cpu_set(R_D0 + r, s->r[r]);
}
static SavedRegs frame_enter(maddr regs_at, maddr a6, unsigned mask) {
    SavedRegs s;
    s.mask = mask | (a6 ? 1u << 14 : 0);
    for (int r = 0; r < 16; r++) s.r[r] = cpu_get(R_D0 + r);
    if (a6) { W32(a6, s.r[14]); cpu_set(R_A6, a6); }
    for (int r = 0; r < 16; r++)
        if (mask & (1u << r)) { W32(regs_at, s.r[r]); regs_at += 4; }
    return s;
}
#define ENTER(fn, link, mask) \
    ENTRY_LINK(fn, link, 4 * __builtin_popcount(mask)); \
    SavedRegs saved_ __attribute__((cleanup(frame_leave))) = \
        frame_enter(a6 - (link) - 4 * (uint32_t)__builtin_popcount(mask), a6, (mask))
#define ENTER_NOLINK(fn, mask) \
    ENTRY_NOLINK(fn, 4 * __builtin_popcount(mask)); \
    SavedRegs saved_ __attribute__((cleanup(frame_leave))) = \
        frame_enter(args_ - 4 - 4 * (uint32_t)__builtin_popcount(mask), 0, (mask))

static inline void setw(int r, uint32_t v) { cpu_set(r, (cpu_get(r) & 0xffff0000u) | (v & 0xffffu)); }
static inline void setb(int r, uint32_t v) { cpu_set(r, (cpu_get(r) & 0xffffff00u) | (v & 0xffu)); }
#define RL(r, v)  cpu_set(R_##r, (uint32_t)(v))
#define RW(r, v)  setw(R_##r, (uint32_t)(v))
#define RB(r, v)  setb(R_##r, (uint32_t)(v))

maddr loader_segment_handle(int16_t seg);
/* runtime address of a CODE 21 location given by its Ghidra address */
static void set_ret(maddr slot, uint32_t ghidra) {
    maddr h = loader_segment_handle(21);
    if (h && rd32(h)) W32(slot, rd32(h) + (ghidra - 0x5fb60));
}
#define JSR(ret, fn, call) __extension__ ({ maddr js_ = emu_sp(); uint32_t jr_ = (call); \
        set_ret(js_ - (uint32_t)fn_info[fn].arg_bytes - 4, (ret)); jr_; })
#define ASYNC ((int16_t)R8(emu_sp() - 7))

/* fileCheckException returns its Boolean like the Pascal slot: in the high byte of the
 * word (the original writes only that byte). Callers test this byte. */
#define XCEPT(ret, err) ((JSR(ret, FN_fileCheckException, fileCheckException(err)) >> 8) & 0xff)
#define MAPERR(ret, err) JSR(ret, FN_fileMapOSError, fileMapOSError(err))

/* A call made with SP n bytes lower than the callee's own arguments (the original has
 * already pushed part of an outer call's arguments). */
#define DEEPER(n, call) __extension__ ({ maddr dp_ = emu_sp(); emu_set_sp(dp_ - (n)); \
        uint32_t dr_ = (call); emu_set_sp(dp_); dr_; })

/* Pascal call through a pointer with a word result: "subq.w #2,SP; <push a>; <push b>;
 * jsr (A0); move.w (SP)+,D0". The callee pops its arguments. */
static uint16_t pcall_ll(maddr fn, uint32_t a, uint32_t b, uint32_t ret) {
    maddr s = emu_sp();
    emu_set_sp(s - 2);
    push32(a);
    push32(b);
    call68_addr(fn);
    set_ret(s - 14, ret);
    uint16_t r = RU16(s - 2);
    emu_set_sp(s);
    return r;
}
static uint16_t pcall_l(maddr fn, uint32_t a, uint32_t ret) {
    maddr s = emu_sp();
    emu_set_sp(s - 2);
    push32(a);
    call68_addr(fn);
    set_ret(s - 10, ret);
    uint16_t r = RU16(s - 2);
    emu_set_sp(s);
    return r;
}

/* ======================================================================== */

/* 0005fb64: is the file layer initialised? (Boolean in the high byte) */
uint32_t fileQueryInit(void) {
    return R16(gFileInitCount) != 0 ? 0x100 : 0;
}

/* 0005fb92: undo one fileInit; the last one restores the launch directory and clears the state */
uint32_t fileQuit(void) {
    ENTER(FN_fileQuit, 0x4, 0x0018);            /* link -4; movem D3/D4 */
    uint16_t n = RU16(gFileInitCount);
    RW(D4, n);
    uint32_t d3 = n;
    RL(D3, d3);
    W32(a6 - 4, d3);
    d3 -= 1;
    RL(D3, d3);
    uint16_t v = (int32_t)d3 < 0 ? 0 : (uint16_t)(R32(a6 - 4) - 1);
    W16(gFileInitCount, v);
    if (v == 0) {
        JSR(0x5fbc8, FN_fileSetDir, fileSetDir(gFileLaunchDir));
        JSR(0x5fbda, FN_tc_memset, tc_memset(gFileInitCount, 0, 0x392));
    }
    return 0;
}

/* 0005fbf2: check that the volume of a fileSpec is present and writable */
uint32_t fileCheckVol(uint32_t spec) {
    ENTER(FN_fileCheckVol, 0x180, 0x0478);      /* HParamBlock at -0x180 */
    RL(A2, spec);
    W16(a6 - 0x164, 0);                         /* ioVolIndex */
    W16(a6 - 0x16a, RU16(spec));                /* ioVRefNum */
    W32(a6 - 0x174, 0);                         /* ioCompletion */
    W32(a6 - 0x16e, a6 - 0x104);                /* ioNamePtr */
    if (S16(JSR(0x5fc26, FN_tc_PBHGetVInfo, tc_PBHGetVInfo(ASYNC, a6 - 0x180))) != 0) {
        W16(gFileError, 0x284c);
        return 0x284c;
    }
    int32_t attr = R16(a6 - 0x15a);             /* ioVAtrb */
    W32(a6 - 4, attr);
    if ((attr & 0xffff8000) != 0 || (R32(a6 - 4) & 0x80) != 0) {
        W16(gFileError, 0x284d);
        return 0x284d;
    }
    W16(gFileError, 0);
    return 0;
}

/* 0005fc90 */
uint32_t fileGetOSError(void) {
    return RU16(gFileOSError);
}

/* 0005fcb2: add the data+resource sizes of every file under a directory (recursive) */
uint32_t getDirectorySize(uint32_t total, uint32_t dirID, int16_t vRefNum) {
    ENTER(FN_getDirectorySize, 0x8c, 0x0478);   /* CInfoPB at -0x8c */
    RL(A2, total);
    RL(D3, dirID);
    RW(D4, vRefNum);
    uint32_t index = 1;
    RL(D5, index);
    int16_t err = 0;
    RW(D6, err);
    W32(a6 - 0x80, 0);                          /* ioCompletion */
    W16(a6 - 0x76, vRefNum);                    /* ioVRefNum */
    do {
        W32(a6 - 0x5c, dirID);                  /* ioDirID */
        W16(a6 - 0x70, index);                  /* ioFDirIndex */
        index++;
        RL(D5, index);
        W32(a6 - 0x7a, a6 - 0x20);              /* ioNamePtr */
        err = S16(JSR(0x5fcf8, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, a6 - 0x8c)));
        RW(D6, err);
        if (err == 0) {
            if (_BitTst(a6 - 0x6e, 3)) {        /* ioFlAttrib: directory */
                err = S16(JSR(0x5fd1e, FN_getDirectorySize, getDirectorySize(total, R32(a6 - 0x5c), vRefNum)));
                RW(D6, err);
            } else {
                uint32_t n = R32(a6 - 0x56) + R32(a6 - 0x4c);   /* ioFlLgLen + ioFlRLgLen */
                W32(total, R32(total) + n);
            }
        }
    } while (err == 0);
    if (err == -0x2b) return 0;                 /* fnfErr: end of the directory */
    return (uint16_t)err;
}

/* 0005fd6c: OSErr -> Mohawk file error; sets the exception class and gFileOSError */
uint32_t fileMapOSError(int16_t err) {
    uint16_t r;
    int cls;                                    /* -1: class byte untouched */
    switch (err) {
    case 0:                                         r = 0;      cls = -1; break;
    case -0x25:                                     r = 0x2846; cls = 1; break;
    case -0x1399: case -0x30:                       r = 0x283e; cls = 1; break;
    case -0x3a:                                     r = 0x2841; cls = 1; break;
    case -0x139f: case -0x1388: case -0x2e:         r = 0x283c; cls = 2; break;
    case -0x13a3: case -0x1398: case -0x23:         r = 0x284c; cls = 3; break;
    case -0x13a7: case -0x2c:                       r = 0x284d; cls = 4; break;
    case -0x78:                                     r = 0x2846; cls = 1; break;
    case -0x13a5: case -0x139a: case -0x1394: case -0x2b: r = 0x2845; cls = 1; break;
    case -0x32:                                     r = 0x2843; cls = 1; break;
    case -0x31:                                     r = 0x2847; cls = 1; break;
    case -0x36:                                     r = 0x283c; cls = 1; break;
    case -0x13a2: case -0x2a:                       r = 0x283c; cls = 1; break;
    case -0x33:                                     r = 0x286f; cls = 1; break;
    case -0x1392: case -0x138f: case -0x2f:         r = 0x283d; cls = 1; break;
    case -0x13a4: case -0x13a8: case -0x2d:         r = 0x283c; cls = 1; break;
    case -0x1390: case -0x22:                       r = 0x2849; cls = 1; break;
    case -0x37:                                     r = 0x284b; cls = 1; break;
    case -0x3d:                                     r = 0x283c; cls = 1; break;
    case -0x1391: case -0x27:                       r = 0x283f; cls = 1; break;
    default:                                        r = 0x2848; cls = 0; break;
    }
    if (cls >= 0) W8(gFileXceptClass, cls);
    W16(gFileOSError, err);
    return r;
}

/* 0005ffca: volume info for a drive/volume: *drvInfo, name (C string at out+2), locked flag */
uint32_t _fileGetVolInfo(uint32_t lockedOut, uint32_t out, int16_t vol) {
    ENTER(FN__fileGetVolInfo, 0x28, 0x1cf8);
    RL(A4, lockedOut);
    RW(D5, vol);
    W32(a6 - 0x28, gFilePB);
    W32(gFilePB + 0xc, 0);
    for (;;) {
        maddr pb = R32(a6 - 0x28);
        RL(A2, pb);
        W16(pb + 0x1c, 0);                      /* ioVolIndex */
        W16(pb + 0x16, vol);                    /* ioVRefNum */
        W32(pb + 0x12, a6 - 0x24);              /* ioNamePtr */
        int16_t err = S16(JSR(0x60008, FN_tc_PBHGetVInfo, tc_PBHGetVInfo(ASYNC, pb)));
        RW(D6, err);
        if (err == 0) break;
        if (!XCEPT(0x6001a, err)) return (uint16_t)err;
    }
    maddr pb = R32(a6 - 0x28);
    RL(A2, pb);
    uint16_t drv = RU16(pb + 0x42);             /* ioVDrvInfo */
    RL(A3, out);
    W16(out, drv);
    JSR(0x6003e, FN_tc_PtoCstr, tc_PtoCstr(a6 - 0x24));
    RL(A3, out + 2);
    JSR(0x6004c, FN_tc_strcpy, tc_strcpy(out + 2, a6 - 0x24));
    uint16_t at = RU16(pb + 0x26);              /* ioVAtrb */
    RW(D7, at);
    RW(D4, at);
    uint32_t d3 = at;
    W32(a6 - 4, d3);
    d3 &= 0xffff8000;
    RL(D3, d3);
    uint16_t locked = (d3 == 0xffff8000 || (R32(a6 - 4) & 0x80) == 0x80) ? 1 : 0;
    W16(lockedOut, locked);
    return 0;
}

/* 000600b4: fileEnumVols callback: fill the drive record if this volume is on gFileDriveNum */
uint32_t driveVolumeEnumProc(uint32_t rec, uint32_t vrefp) {
    ENTER(FN_driveVolumeEnumProc, 0x24, 0x1c08);
    RL(D3, rec);
    RL(A3, vrefp);
    RL(A4, rec);
    if (S16(JSR(0x600d8, FN__fileGetVolInfo, _fileGetVolInfo(a6 - 2, a6 - 0x24, R16(vrefp))))) return 0;
    if (R16(a6 - 0x24) != R16(gFileDriveNum)) return 0;
    RL(A2, rec);
    W16(rec + 0x22, RU16(vrefp));
    uint16_t f = (uint16_t)(((RU16(a6 - 2) & 1) << 6) | (RU16(rec) & 0xffbf));
    W16(rec, f);
    RL(A2, rec + 2);
    JSR(0x60110, FN_tc_strcpy, tc_strcpy(rec + 2, a6 - 0x22));
    return 1;
}

/* 00060144: find the volume mounted on a drive (via fileEnumVols) */
uint32_t fileGetDriveVolume(int16_t drive, uint32_t rec) {
    ENTER(FN_fileGetDriveVolume, 0, 0x0408);
    RW(D3, drive);
    RL(A2, rec);
    W16(gFileDriveNum, drive);
    JSR(0x60162, FN_fileEnumVols, fileEnumVols(rec, JT_ADDR(FN_driveVolumeEnumProc)));
    return 0;
}

/* 00060184: ask the exception proc whether to retry after an OSErr. Returns the Boolean
 * in the high byte (see XCEPT); "conv": "manual", glue below. */
uint32_t fileCheckException(int16_t err) {
    ENTER(FN_fileCheckException, 0, 0x0018);
    RW(D3, err);
    int16_t msg = 0;
    RW(D4, msg);
    if (err == 0) return 0;
    if (R16(gFileXceptOff) != 0) return 0;
    if (R32(gFileXceptProc) == 0) return 0;
    switch (S16(JSR(0x601be, FN_fileGetOSError, fileGetOSError()))) {
    case -0x13a7: case -0x2e:               msg = 1; break;
    case -0x13a3: case -0x1398: case -0x23: msg = 2; break;
    case -0x2c:                             msg = 3; break;
    case -0x13a4: case -0x13a8: case -0x2d: msg = 4; break;
    case -0x1390: case -0x22:               msg = 5; break;
    case -0x37:                             msg = 6; break;
    }
    RW(D4, msg);
    if (msg == 0) return 0;
    JSR(0x60240, FN_tc_GetIndString, tc_GetIndString(msg, 0x7d00, R32(gFileXceptMsg)));
    JSR(0x60248, FN_tc_PtoCstr, tc_PtoCstr(R32(gFileXceptMsg)));
    uint16_t r = pcall_l(R32(gFileXceptProc), gFileXceptClass, 0x60256);
    return (uint32_t)(r & 0xff) << 8;          /* move.b D0,(0xa,A6): the low byte of the result */
}
void glue_fileCheckException(void) {
    maddr sp = emu_sp();
    uint32_t r = fileCheckException((int16_t)RU16(sp));
    W8(sp + 2, r >> 8);                         /* only the high byte of the slot */
    emu_set_sp(sp + 2);
    emu_set_d0(r >> 8);
}

/* 00060286: translate a Unix-style path ("/vol/dir/x", "../x", "./x") into a Mac path */
uint32_t fileXlatSpecToMac(uint32_t src, uint32_t dst) {
    ENTER(FN_fileXlatSpecToMac, 0xc, 0x1c08);
    maddr s = src, d = dst;
    RL(A3, s);
    RL(A4, d);
    if (R8(s) == 0) {
        RL(A2, d);
        W8(d, 0);
        return d;
    }
    W32(a6 - 0xc, d);
    W8(a6 - 8, ':');
    if (R8(s) == '/') {
        uint32_t vol = d;
        RL(D3, vol);
        W32(a6 - 4, vol);
        JSR(0x602d0, FN_tc_GetVol, tc_GetVol(gFilePB + 0x16, vol));
        JSR(0x602d8, FN_tc_PtoCstr, tc_PtoCstr(vol));
        d = JSR(0x602e0, FN_tc_strlen, tc_strlen(d)) + d;
        W8(d, ':'); d++;
        do s++; while (R8(s) == '/');
    } else if (R8(s) == '.' && R8(s + 1) == '.' && R8(s + 2) == '/') {
        W8(d, ':'); d++;
        W8(d, ':'); d++;
        s += 3;
    } else if (R8(s) == '.' && R8(s + 1) == '.') {
        W8(d, ':'); d++;
        W8(d, ':'); d++;
        s += 2;
    } else {
        W8(d, ':'); d++;
        if (R8(s) == '.' && R8(s + 1) == '/') s += 2;
    }
    W8(d, 0);
    while (R8(s) != 0) {
        int8_t c = RS8(s);
        if (c == '/') {
            W8(d, ':'); d++;
            s++;
        } else if (c == '.') {
            if (R8(s + 1) == '.' && R8(s + 2) == '/') {
                W8(d, ':'); d++;
                s += 3;
            } else if (R8(s + 1) == '/') {
                s++;
            } else {
                W8(d, R8(s)); d++; s++;
            }
        } else {
            W8(d, R8(s)); d++; s++;
        }
    }
    W8(d, 0);
    return R32(a6 - 0xc);
}

/* 0006041c: build "vol:dir:...:" (C string) for a directory by walking up its parents;
 * withVol=0 leaves out the volume name. D0 = error ("ret": 2). */
uint32_t fileDerivePathFromIDs__FPcslUs(int16_t withVol, uint32_t dirID, int16_t vRefNum, uint32_t buf) {
    ENTER(FN_fileDerivePathFromIDs__FPcslUs, 0x10c, 0x1cf8);
    RW(D3, withVol);
    RL(D4, dirID);
    RW(D5, vRefNum);
    RL(A3, buf);
    maddr pb = gFilePB;
    RL(A4, pb);
    W32(a6 - 8, a6 - 0xc);
    W16(a6 - 0xc, 0x013a);                      /* Pascal ":" */
    W8(buf, 0);
    W32(pb + 0x64, dirID);                      /* ioDrParID */
    W32(pb + 0x12, a6 - 0x10c);                 /* ioNamePtr */
    do {
        RL(A2, pb);
        W16(pb + 0x16, vRefNum);
        W16(pb + 0x1c, 0xffff);                 /* ioFDirIndex -1: info about ioDirID */
        W32(pb + 0x30, R32(pb + 0x64));
        int16_t err = S16(JSR(0x60476, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, gFilePB)));
        RW(D6, err);
        if (err != 0) {
            W8(buf, 0);
            uint16_t e = (uint16_t)MAPERR(0x6048a, err);
            RW(D7, e);
            W16(gFileError, e);
            return e;
        }
        JSR(0x604a0, FN_pstrcat, pstrcat(a6 - 0x10c, a6 - 0xc));
        if (R32(pb + 0x30) != 2 || withVol != 0)
            JSR(0x604bc, FN_pstrinsert, pstrinsert(buf, a6 - 0x10c));
    } while (R32(pb + 0x30) != 2);
    JSR(0x604d2, FN_tc_PtoCstr, tc_PtoCstr(buf));
    W16(a6 - 4, 0);
    W16(gFileError, 0);
    return 0;
}

/* 0006050a: path of the default (working) directory, "vol:dir:...:" */
uint32_t fileGetWDPath__FPc(uint32_t buf) {
    ENTER(FN_fileGetWDPath__FPc, 0x4, 0x1c38);
    RL(A3, buf);
    W32(a6 - 4, gFilePB);
    int16_t err = S16(JSR(0x60536, FN_HGETVOL, HGETVOL(gFilePB + 0x30, gFilePB + 0x20, buf)));
    RW(D3, err);
    if (err != 0) {
        uint16_t e = (uint16_t)MAPERR(0x60546, err);
        RW(D4, e);
        W16(gFileError, e);
        return e;
    }
    JSR(0x60556, FN_tc_PtoCstr, tc_PtoCstr(buf));
    maddr p = JSR(0x6055e, FN_tc_strlen, tc_strlen(buf)) + buf;
    W8(p, ':'); p++;
    RL(A4, p);
    W8(p, 0);
    maddr pb = R32(a6 - 4);
    RL(A2, pb);
    JSR(0x60584, FN_fileDerivePathFromIDs__FPcslUs, fileDerivePathFromIDs__FPcslUs(0, R32(pb + 0x30), R16(pb + 0x20), p));
    RW(D5, 0);
    W16(gFileError, 0);
    return 0;
}

/* 000605ac: name of a volume (C string). D0 = error ("ret": 2). */
uint32_t fileGetVolName__FsPc(uint32_t buf, int16_t vRefNum) {
    ENTER(FN_fileGetVolName__FsPc, 0, 0x0c78);
    RL(A2, buf);
    RW(D3, vRefNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    W32(pb + 0x12, buf);
    W32(pb + 0xc, 0);
    W16(pb + 0x1c, 0);
    W16(pb + 0x16, vRefNum);
    int16_t err = S16(JSR(0x605e0, FN_tc_PBGetVInfo, tc_PBGetVInfo(ASYNC, pb)));
    RW(D4, err);
    if (err != 0) {
        W8(buf, 0);
        uint16_t e = (uint16_t)MAPERR(0x605f4, err);
        RW(D5, e);
        W16(gFileError, e);
        return e;
    }
    JSR(0x60604, FN_tc_PtoCstr, tc_PtoCstr(buf));
    RW(D6, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00060630: static constructor of gFileLaunchDir */
uint32_t FUN_00060630(void) {
    ENTER_NOLINK(FN_FUN_00060630, 0x0400);      /* move.l A2,-(SP) */
    RL(A2, gFileInitCount);
    return JSR(0x60640, FN___ct__8fileSpecDv, __ct__8fileSpecDv(gFileLaunchDir));
}

/* 00060646: static destructor of gFileLaunchDir */
uint32_t seg21_060646(void) {
    ENTER_NOLINK(FN_seg21_060646, 0x0400);
    RL(A2, gFileInitCount);
    return JSR(0x60656, FN___dt__8fileSpecDv, __dt__8fileSpecDv(gFileLaunchDir));
}

/* 0006065c */
uint32_t fileClose(int16_t refNum) {
    ENTER(FN_fileClose, 0, 0x0cf8);
    RW(D3, refNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    W32(pb + 0xc, 0);
    for (;;) {
        RL(A2, pb);
        W16(pb + 0x18, refNum);                 /* ioRefNum */
        int16_t err = S16(JSR(0x60684, FN_tc_PBClose, tc_PBClose(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x606a2, err);
        RW(D4, e);
        if (XCEPT(0x606ae, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 000606da: create a file (flags: 2 directory, 8 locked, 4 invisible), type 'TEXT' creator 'MHWK' */
uint32_t fileCreate(int16_t flags, uint32_t spec) {
    ENTER(FN_fileCreate, 0x14, 0x0cf8);
    RW(D3, flags);
    RL(A2, spec);
    if (R16(spec) == 0) {
        RL(A3, spec);
        int16_t q = S16(JSR(0x606fc, FN_qualify__8fileSpec, qualify__8fileSpec(spec)));
        RW(D4, q);
        if (q != 0) return (uint16_t)q;
    }
    W16(gFileXceptVol, RU16(spec));
    for (;;) {
        int16_t err;
        if ((uint16_t)flags & 2) {
            err = S16(_FSpDirCreate(spec, 0, a6 - 4));
            RW(D5, err);
            if (err != 0) goto fail;
        } else {
            err = S16(_FSpCreate(spec, 0x4d48574b, 0x54455854, 0));    /* 'MHWK' 'TEXT' */
            RW(D5, err);
            if (err != 0) goto fail;
            if ((uint16_t)flags & 8) {
                err = S16(_FSpSetFLock(spec));
                RW(D5, err);
                if (err != 0) goto fail;
            }
        }
        if ((uint16_t)flags & 4) {
            err = S16(_FSpGetFInfo(spec, a6 - 0x14));
            RW(D5, err);
            if (err != 0) goto fail;
            _BitSet(a6 - 0xc, 1);               /* fdFlags: invisible */
            err = S16(_FSpSetFInfo(spec, a6 - 0x14));
            RW(D5, err);
            if (err != 0) goto fail;
        }
        RW(D6, 0);
        W16(gFileError, 0);
        return 0;
    fail:;
        uint16_t e = (uint16_t)MAPERR(0x607da, err);
        RW(D4, e);
        if (XCEPT(0x607e6, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00060814 */
uint32_t fileDelete(uint32_t spec) {
    ENTER(FN_fileDelete, 0x104, 0x1cf8);
    RL(A3, spec);
    maddr pb = gFilePB;
    RL(A4, pb);
    RL(D3, spec);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x6083c, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    W16(gFileXceptVol, RU16(spec));
    for (;;) {
        RL(A2, spec + 6);
        JSR(0x60862, FN_tc_strcpy, tc_strcpy(a6 - 0x104, spec + 6));
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W32(pb + 0x12, a6 - 0x104);
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x30, R32(spec + 2));
        int16_t err = S16(JSR(0x6088e, FN_tc_PBHDelete, tc_PBHDelete(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x608ac, err);
        RW(D4, e);
        if (XCEPT(0x608b8, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 000608e4 */
uint32_t fileEject(int16_t drive) {
    ENTER(FN_fileEject, 0, 0x0cf8);
    RW(D3, drive);
    maddr pb = gFilePB;
    RL(A3, pb);
    W32(pb + 0xc, 0);
    W16(pb + 0x16, drive);
    RW(D5, _PBMountVol(pb));
    W16(gFileXceptVol, RU16(pb + 0x16));
    for (;;) {
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x16, drive);
        int16_t err = S16(_PBEject(pb));
        RW(D5, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x6093a, err);
        RW(D4, e);
        if (XCEPT(0x60946, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00060972: call proc(name, data) (Pascal, Boolean stop) for each subdirectory of the
 * current directory. The loop test reads D5 before setting it: the caller's D5. */
uint32_t fileEnumDirs(uint32_t data, uint32_t proc) {
    ENTER(FN_fileEnumDirs, 0x194, 0x04f8);
    RL(D3, data);
    RL(A2, proc);
    int16_t err = (int16_t)cpu_get(R_D5);       /* uninitialised in the original */
    W32(a6 - 0x188, 0);                         /* ioCompletion */
    for (;;) {
        uint32_t index = 1;
        RL(D4, index);
        while (err == 0) {
            W16(a6 - 0x178, index);             /* ioFDirIndex */
            W32(a6 - 0x182, a6 - 0x128);        /* ioNamePtr */
            W16(a6 - 0x17e, RU16(gFileCurVRefNum));
            W32(a6 - 0x164, R32(gFileCurDirID));
            err = S16(JSR(0x609b4, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, a6 - 0x194)));
            RW(D5, err);
            if (err != 0) break;
            if (_BitTst(a6 - 0x176, 3)) {
                JSR(0x609d6, FN_tc_PtoCstr, tc_PtoCstr(a6 - 0x128));
                JSR(0x609e4, FN_tc_strcpy, tc_strcpy(a6 - 0x28, a6 - 0x128));
                if (pcall_ll(proc, a6 - 0x28, data, 0x609f2) != 0) break;
            }
            index++;
            RL(D4, index);
        }
        if (err == -0x2b) {
            W16(a6 - 6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x60a1a, err);
        RW(D6, e);
        if (XCEPT(0x60a26, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00060a58: call proc(&drive, data) for each drive in the drive queue */
uint32_t fileEnumDrives(uint32_t data, uint32_t proc) {
    ENTER(FN_fileEnumDrives, 0x20, 0x1c18);
    RL(D3, data);
    RL(A2, proc);
    RL(A3, DrvQHdr);
    for (maddr q = R32(DrvQHdr + 2); ; q = R32(q)) {
        RL(A4, q);
        if (q == 0) break;
        W16(a6 - 0x20, RU16(q + 6));            /* dQDrive */
        if (pcall_ll(proc, a6 - 0x20, data, 0x60a92) != 0) break;
    }
    RW(D4, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00060ac6: call proc(name, data) for each file of the current directory (same quirk as
 * fileEnumDirs) */
uint32_t fileEnumFiles(uint32_t data, uint32_t proc) {
    ENTER(FN_fileEnumFiles, 0x190, 0x04f8);
    RL(D3, data);
    RL(A2, proc);
    int16_t err = (int16_t)cpu_get(R_D5);       /* uninitialised in the original */
    W32(a6 - 0x184, 0);                         /* ioCompletion */
    for (;;) {
        uint32_t index = 1;
        RL(D4, index);
        while (err == 0) {
            W16(a6 - 0x174, index);
            W32(a6 - 0x160, R32(gFileCurDirID));
            W16(a6 - 0x17a, RU16(gFileCurVRefNum));
            W32(a6 - 0x17e, a6 - 0x124);
            err = S16(JSR(0x60b08, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, a6 - 0x190)));
            RW(D5, err);
            if (err != 0) break;
            if (!_BitTst(a6 - 0x172, 3)) {
                JSR(0x60b2a, FN_tc_PtoCstr, tc_PtoCstr(a6 - 0x124));
                JSR(0x60b38, FN_tc_strcpy, tc_strcpy(a6 - 0x24, a6 - 0x124));
                if (pcall_ll(proc, a6 - 0x24, data, 0x60b46) != 0) break;
            }
            index++;
            RL(D4, index);
        }
        if (err == -0x2b) {
            RW(D7, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x60b6c, err);
        RW(D6, e);
        if (XCEPT(0x60b78, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00060bac: call proc(&vRefNum, data) for each mounted volume */
uint32_t fileEnumVols(uint32_t data, uint32_t proc) {
    ENTER(FN_fileEnumVols, 0x20, 0x1c08);
    RL(D3, data);
    RL(A2, proc);
    RL(A3, VCBQHdr);
    for (maddr v = R32(VCBQHdr + 2); ; v = R32(v)) {
        RL(A4, v);
        if (v == 0) break;
        W16(a6 - 0x20, RU16(v + 0x4e));         /* vcbVRefNum */
        if (pcall_ll(proc, a6 - 0x20, data, 0x60be6) != 0) break;
    }
    return 0;
}

/* 00060c10 */
uint32_t fileExists(uint32_t spec) {
    ENTER(FN_fileExists, 0x108, 0x1cf8);
    RL(A4, spec);
    W32(a6 - 0x108, gFilePB);
    RL(D3, spec);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x60c3a, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    W32(R32(a6 - 0x108) + 0xc, 0);
    W8(a6 - 0x104, 0);
    JSR(0x60c66, FN_pstrinsert, pstrinsert(a6 - 0x104, spec + 6));
    for (;;) {
        maddr pb = R32(a6 - 0x108);
        RL(A2, pb);
        W16(pb + 0x1c, 0);
        RL(A3, spec);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x104);
        int16_t err = S16(JSR(0x60c94, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x60cb2, err);
        RW(D5, e);
        if (XCEPT(0x60cbe, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00060cea: attributes: 1 file, 2 directory, 8 locked, 4 invisible */
uint32_t fileGetAttr(uint32_t out, uint32_t spec) {
    ENTER(FN_fileGetAttr, 0x108, 0x1cf8);
    RL(A4, out);
    W32(a6 - 0x108, gFilePB);
    RL(D3, spec);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x60d16, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    W8(a6 - 0x104, 0);
    JSR(0x60d3a, FN_pstrinsert, pstrinsert(a6 - 0x104, spec + 6));
    for (;;) {
        RL(A2, R32(a6 - 0x108));
        W32(R32(a6 - 0x108) + 0xc, 0);
        if (R8(spec + 6) == 0) W16(R32(a6 - 0x108) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x108) + 0x1c, 0);
        RL(A2, spec);
        maddr pb = R32(a6 - 0x108);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x104);
        int16_t err = S16(JSR(0x60d8a, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) break;
        uint16_t e = (uint16_t)MAPERR(0x60d9c, err);
        RW(D5, e);
        if (XCEPT(0x60da8, err)) continue;
        RW(D6, e);
        W16(gFileError, e);
        return e;
    }
    RL(A2, out);
    W16(out, 0);
    if (_BitTst(R32(a6 - 0x108) + 0x1e, 3)) W16(out, RU16(out) | 2);
    else W16(out, RU16(out) | 1);
    if (_BitTst(R32(a6 - 0x108) + 0x1e, 7)) W16(out, RU16(out) | 8);
    if (_BitTst(R32(a6 - 0x108) + 0x28, 1)) W16(out, RU16(out) | 4);
    RW(D7, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00060e44: fileSpec of the current directory (an FSSpec: Pascal name) */
uint32_t fileGetDir(uint32_t spec) {
    ENTER(FN_fileGetDir, 0x48, 0x0c78);
    RL(A3, spec);
    W16(gFileXceptVol, RU16(spec));
    int16_t err = S16(_FSMakeFSSpec(0, 0, gFileCurPath, a6 - 0x48));
    RW(D3, err);
    if (err == 0) {
        mcopy(spec, a6 - 0x48, 0x46);
        RW(D4, 0);
        W16(gFileError, 0);
        return 0;
    }
    if (R16(gFileAtRoot) == 1) {
        RL(A2, spec);
        W16(spec, RU16(a6 - 0x48));
        W32(spec + 2, 1);
        W8(spec + 6, 0);
        RL(A2, spec + 6);
        JSR(0x60eb2, FN_pstrcat, pstrcat(spec + 6, gFileCurPath));
        RW(D5, 0);
        W16(gFileError, 0);
        return 0;
    }
    uint16_t e = (uint16_t)MAPERR(0x60ec6, err);
    RW(D6, e);
    W16(gFileError, e);
    return 0;
}

/* 00060ee8: describe a drive: flags word (0x100 floppy/CD, 0x200 hard disk, 0x300/0x400
 * other kinds, 0x10/0x20/0x80 bits), then the volume on it (fileGetDriveVolume) */
uint32_t fileGetDriveInfo(uint32_t rec, int16_t drive) {
    ENTER(FN_fileGetDriveInfo, 0x28, 0x1cf8);
    RL(A3, rec);
    RW(D3, drive);
    W32(a6 - 0x14, gFilePB);
    RL(A4, DrvQHdr);
    W32(a6 - 0x24, R32(DrvQHdr + 2));
    for (;;) {
        if (R32(a6 - 0x24) == 0) {
            W16(a6 - 0x10, FERR_NODRIVE);
            W16(gFileError, RU16(a6 - 0x10));
            return FERR_NODRIVE;
        }
        if (R16(R32(a6 - 0x24) + 6) == drive) break;
        W32(a6 - 0x24, R32(R32(a6 - 0x24)));
    }
    RL(A2, rec);
    W16(rec + 0x22, 0);
    W16(rec, RU16(rec) & 0xff7f);
    for (;;) {
        uint16_t f;
        int16_t err;
        if (R16(R32(a6 - 0x24) + 0xa) == 1) {   /* dQFSID */
            RL(A2, rec);
            f = (RU16(rec) & 0xff) | 0x400; W16(rec, f);
            f &= 0xffef; W16(rec, f);
            f = (f & 0xffdf) | 0x20; W16(rec, f);
            JSR(0x60f8c, FN_fileGetDriveVolume, fileGetDriveVolume(drive, rec));
            W16(a6 - 0xe, 0);
            W16(gFileError, 0);
            return 0;
        }
        W32(a6 - 0x28, R32(R32(a6 - 0x24) - 4));        /* the drive's flag bytes */
        RL(A2, rec);
        f = RU16(rec) & 0xffdf; W16(rec, f);
        if (R8(a6 - 0x27) == 8) {
            f &= 0xffef; W16(rec, f);
        } else {
            RL(A2, rec);
            W16(rec, (RU16(rec) & 0xffef) | 0x10);
        }
        int16_t unit = (int16_t)(-R16(R32(a6 - 0x24) + 8) - 1);    /* -dQRefNum - 1 */
        RW(D4, unit);
        W32(a6 - 0x20, R32((int32_t)unit * 4 + R32(UTableBase)));   /* DCE handle */
        maddr dce = R32(R32(a6 - 0x20));
        RL(A2, dce);
        int16_t dflags = R16(dce + 4);
        RW(D5, dflags);
        if ((int32_t)dflags & 0x40) W32(a6 - 0x1c, R32(R32(dce)));
        else W32(a6 - 0x1c, R32(R32(R32(a6 - 0x20))));
        W32(a6 - 0x1c, R32(a6 - 0x1c) + 0x12);   /* drvrName */
        maddr np = R32(a6 - 0x1c);
        W32(a6 - 0x1c, np + 1);
        W8(a6 - 0x18, R8(np));
        if (JSR(0x61048, FN_tc_memcmp, tc_memcmp(R32(a6 - 0x1c), gFileStr5d6, R8(a6 - 0x18))) == 0) {
            maddr pb = R32(a6 - 0x14);
            RL(A2, pb);
            W32(pb + 0xc, 0);
            W32(pb + 0x12, gFileStr5dc);
            W8(pb + 0x1b, 0);
            err = S16(JSR(0x61072, FN_tc_PBOpen, tc_PBOpen(ASYNC, pb)));
            RW(D6, err);
            if (err != 0) goto fail;
            W16(pb + 0x16, RU16(R32(a6 - 0x24) + 6));
            RL(A2, R32(a6 - 0x14));
            W16(pb + 0x18, RU16(pb + 0x18));
            W16(pb + 0x1a, 0x17);               /* csCode 23: drive info */
            err = S16(JSR(0x610a6, FN_tc_PBControl, tc_PBControl(ASYNC, pb)));
            RW(D6, err);
            if (err != 0) goto fail;
            if (R16(R32(a6 - 0x14) + 0x1e) == 7) { RL(A2, rec); W16(rec, (RU16(rec) & 0xff) | 0x200); }
            RL(A2, rec);
            if (R16(R32(a6 - 0x14) + 0x1e) == 1) W16(rec, RU16(rec) & 0xff);
            else W16(rec, (RU16(rec) & 0xff) | 0x300);
            JSR(0x610fe, FN_fileGetDriveVolume, fileGetDriveVolume(drive, rec));
            W16(a6 - 0xc, 0);
            W16(gFileError, 0);
            return 0;
        }
        if (JSR(0x61126, FN_tc_memcmp, tc_memcmp(R32(a6 - 0x1c), gFileStr5e4, R8(a6 - 0x18))) == 0) {
            RL(A2, rec);
            W16(rec, (RU16(rec) & 0xff) | 0x100);
            JSR(0x61144, FN_fileGetDriveVolume, fileGetDriveVolume(drive, rec));
            W16(a6 - 0xa, 0);
            W16(gFileError, 0);
            return 0;
        }
        if (JSR(0x6116c, FN_tc_memcmp, tc_memcmp(R32(a6 - 0x1c), gFileStr5ee, R8(a6 - 0x18))) == 0) {
            RL(A2, rec);
            f = (RU16(rec) & 0xff) | 0x400; W16(rec, f);
            f &= 0xffef; W16(rec, f);
            f = (f & 0xffdf) | 0x20; W16(rec, f);
            JSR(0x6119a, FN_fileGetDriveVolume, fileGetDriveVolume(drive, rec));
            W16(a6 - 8, 0);
            W16(gFileError, 0);
            return 0;
        }
        RL(A2, rec);
        if (((int32_t)dflags & 0x200) == 0 && ((int32_t)RS8(a6 - 0x28) & 0x80) != 0)
            W16(rec, (RU16(rec) & 0xff) | 0x100);
        else
            W16(rec, (RU16(rec) & 0xff) | 0x200);
        JSR(0x611ee, FN_fileGetDriveVolume, fileGetDriveVolume(drive, rec));
        W16(a6 - 6, 0);
        W16(gFileError, 0);
        return 0;
    fail:
        RW(D7, MAPERR(0x61206, err));
        if (XCEPT(0x61212, err)) continue;
        W16(a6 - 4, FERR_NODRIVE);
        W16(gFileError, RU16(a6 - 4));
        return FERR_NODRIVE;
    }
}

/* 0006124c: logical end of file, -1 on error */
uint32_t fileGetEOF(int16_t refNum) {
    ENTER(FN_fileGetEOF, 0, 0x04f8);
    RW(D3, refNum);
    maddr pb = gFilePB;
    RL(A2, pb);
    W32(pb + 0xc, 0);
    W16(pb + 0x18, refNum);
    for (;;) {
        int16_t err = S16(JSR(0x61272, FN_tc_PBGetEOF, tc_PBGetEOF(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return R32(pb + 0x1c);
        }
        uint16_t e = (uint16_t)MAPERR(0x61296, err);
        RW(D5, e);
        if (XCEPT(0x612a2, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return 0xffffffff;
    }
}

/* 000612d0 */
uint32_t fileGetError(void) {
    return RU16(gFileError);
}

/* 000612f0: volume of an open file */
uint32_t fileGetInfo(uint32_t out, int16_t refNum) {
    ENTER(FN_fileGetInfo, 0x100, 0x1cf8);
    RL(A3, out);
    RW(D3, refNum);
    maddr pb = gFilePB;
    RL(A4, pb);
    for (;;) {
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x1c, 0);                      /* ioFCBIndx */
        W16(pb + 0x16, 0);
        W16(pb + 0x18, refNum);
        W32(pb + 0x12, a6 - 0x100);
        int16_t err = S16(JSR(0x61330, FN_tc_PBGetFCBInfo, tc_PBGetFCBInfo(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            W16(out, RU16(pb + 0x34));          /* ioFCBVRefNum */
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        RW(D4, FERR_NODRIVE);
        MAPERR(0x6135c, err);
        if (XCEPT(0x61366, err)) continue;
        RW(D7, FERR_NODRIVE);
        W16(gFileError, FERR_NODRIVE);
        return FERR_NODRIVE;
    }
}

/* 00061394 */
uint32_t fileGetLaunchDir(uint32_t spec) {
    mcopy(spec, gFileLaunchDir, 0x106);
    W16(gFileError, 0);
    return 0;
}

/* 000613d8: PBGetCatInfo into the caller's parameter block */
uint32_t fileGetOSAttr(uint32_t pbOut, uint32_t spec) {
    ENTER(FN_fileGetOSAttr, 0x108, 0x1cf8);
    RL(A4, pbOut);
    W32(a6 - 0x108, pbOut);
    RL(D3, spec);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x613fc, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    for (;;) {
        W8(a6 - 0x104, 0);
        JSR(0x61420, FN_pstrinsert, pstrinsert(a6 - 0x104, spec + 6));
        RL(A2, R32(a6 - 0x108));
        W32(R32(a6 - 0x108) + 0xc, 0);
        if (R8(a6 - 0x104) == 0) W16(R32(a6 - 0x108) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x108) + 0x1c, 0);
        RL(A2, spec);
        maddr pb = R32(a6 - 0x108);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x104);
        int16_t err = S16(JSR(0x6146c, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x6148a, err);
        RW(D5, e);
        if (XCEPT(0x61496, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 000614cc: size of a file (data fork), or of everything in a directory; -1 on error */
uint32_t fileGetSize(uint32_t spec) {
    ENTER(FN_fileGetSize, 0x10c, 0x1cf8);
    RL(A3, spec);
    RL(A2, spec);
    maddr pb = gFilePB;
    W16(gFileXceptVol, RU16(spec));
    RW(D3, 0);
    RL(A4, pb);
    W32(a6 - 0xc, 0);
    W32(a6 - 8, spec);
    if (R16(spec) == 0 && S16(JSR(0x614fe, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 8)))) != 0)
        return FERR_BADSPEC;
    for (;;) {
        W8(a6 - 0x10c, 0);
        RL(A2, spec + 6);
        JSR(0x61522, FN_pstrinsert, pstrinsert(a6 - 0x10c, spec + 6));
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, spec + 6);
        if (R8(spec + 6) == 0) W16(pb + 0x1c, 0xffff);
        else W16(pb + 0x1c, 0);
        int16_t err = S16(JSR(0x61564, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) break;
        uint16_t e = (uint16_t)MAPERR(0x61576, err);
        RW(D5, e);
        if (XCEPT(0x61582, err)) continue;
        RW(D6, e);
        W16(gFileError, e);
        return 0xffffffff;
    }
    if (_BitTst(pb + 0x1e, 3)) {
        int16_t err = S16(JSR(0x615c0, FN_getDirectorySize, getDirectorySize(a6 - 0xc, R32(pb + 0x30), R16(spec))));
        RW(D4, err);
        if (err != 0) {
            uint16_t e = (uint16_t)MAPERR(0x615d0, err);
            RW(D7, e);
            W16(gFileError, e);
            return 0xffffffff;
        }
    } else {
        W32(a6 - 0xc, R32(pb + 0x36));          /* ioFlLgLen */
    }
    W16(a6 - 4, 0);
    W16(gFileError, RU16(a6 - 4));
    return R32(a6 - 0xc);
}

/* 0006161e: fileSpec of an open file (the name is left as a Pascal string) */
uint32_t fileGetSpec(uint32_t spec, int16_t refNum) {
    ENTER(FN_fileGetSpec, 0x4, 0x1c38);
    RL(A4, spec);
    RW(D3, refNum);
    W32(a6 - 4, gFilePB);
    maddr pb = gFilePB;
    W32(pb + 0xc, 0);
    W16(pb + 0x1c, 0);
    W16(pb + 0x16, 0);
    W16(pb + 0x18, refNum);
    W32(pb + 0x12, spec + 6);
    if (S16(JSR(0x6165e, FN_tc_PBGetFCBInfo, tc_PBGetFCBInfo(ASYNC, pb))) != 0) {
        W16(gFileError, FERR_NODRIVE);
        return FERR_NODRIVE;
    }
    pb = R32(a6 - 4);
    W16(spec, RU16(pb + 0x34));                 /* ioFCBVRefNum */
    W32(spec + 2, R32(pb + 0x3a));              /* ioFCBParID */
    W16(gFileError, 0);
    return 0;
}

/* 000616ae: modification time as 6 bytes: second, minute, hour, day, month, year-1980 */
uint32_t fileGetTime(uint32_t out, uint32_t spec) {
    ENTER(FN_fileGetTime, 0x11c, 0x1cf8);
    RL(A4, out);
    W32(a6 - 0x11c, gFilePB);
    RL(D3, spec);
    W32(a6 - 8, spec);
    if (R16(spec) == 0 && S16(JSR(0x616d6, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 8)))) != 0)
        return FERR_BADSPEC;
    int16_t err = S16(JSR(0x616f0, FN_fileCheckVol, fileCheckVol(spec)));
    RW(D5, err);
    if (err != 0) {
        W16(gFileError, err);
        return (uint16_t)err;
    }
    for (;;) {
        W8(a6 - 0x118, 0);
        JSR(0x6171a, FN_pstrinsert, pstrinsert(a6 - 0x118, spec + 6));
        RL(A2, R32(a6 - 0x11c));
        W32(R32(a6 - 0x11c) + 0xc, 0);
        if (R8(a6 - 0x118) == 0) W16(R32(a6 - 0x11c) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x11c) + 0x1c, 0);
        RL(A2, spec);
        maddr pb = R32(a6 - 0x11c);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x118);
        err = S16(JSR(0x61766, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) break;
        uint16_t e = (uint16_t)MAPERR(0x61778, err);
        RW(D5, e);
        if (XCEPT(0x61784, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
    _SecondsToDate(R32(R32(a6 - 0x11c) + 0x4c), a6 - 0x18);   /* ioFlMdDat */
    W8(out, R8(a6 - 0xd));                      /* second */
    W8(out + 1, R8(a6 - 0xf));                  /* minute */
    W8(out + 2, R8(a6 - 0x11));                 /* hour */
    W8(out + 3, R8(a6 - 0x13));                 /* day */
    W8(out + 4, R8(a6 - 0x15));                 /* month */
    uint8_t y = (uint8_t)(R8(a6 - 0x17) + 0x44);
    W8(out + 5, y);
    if ((int8_t)y < 0) W8(out + 5, 0);
    W16(a6 - 4, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00061808: free bytes on a volume, -1 on error */
uint32_t fileGetVolFree(int16_t vRefNum) {
    ENTER(FN_fileGetVolFree, 0, 0x0cf8);
    RW(D3, vRefNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    W32(pb + 0xc, 0);
    for (;;) {
        RL(A2, pb);
        W16(pb + 0x1c, 0);
        W16(pb + 0x16, vRefNum);
        W32(pb + 0x12, 0);
        int16_t err = S16(JSR(0x6183c, FN_tc_PBHGetVInfo, tc_PBHGetVInfo(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return R32(pb + 0x30) * (uint32_t)RU16(pb + 0x3e);   /* ioVAlBlkSiz * ioVFrBlk */
        }
        uint16_t e = (uint16_t)MAPERR(0x61868, err);
        RW(D4, e);
        if (XCEPT(0x61874, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return 0xffffffff;
    }
}

/* 000618aa: drive number and name (C string at out+2) of a volume */
uint32_t fileGetVolInfo(uint32_t out, int16_t vRefNum) {
    ENTER(FN_fileGetVolInfo, 0x100, 0x1cf8);
    RL(A3, out);
    RW(D3, vRefNum);
    maddr pb = gFilePB;
    RL(A4, pb);
    W32(pb + 0xc, 0);
    for (;;) {
        RL(A2, pb);
        W16(pb + 0x1c, 0);
        W16(pb + 0x16, vRefNum);
        W32(pb + 0x12, a6 - 0x100);
        int16_t err = S16(JSR(0x618e4, FN_tc_PBHGetVInfo, tc_PBHGetVInfo(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) break;
        uint16_t e = (uint16_t)MAPERR(0x618f6, err);
        RW(D5, e);
        if (XCEPT(0x61902, err)) continue;
        RW(D6, e);
        W16(gFileError, e);
        return e;
    }
    RL(A2, out);
    W16(out, RU16(pb + 0x42));                  /* ioVDrvInfo */
    JSR(0x61928, FN_tc_PtoCstr, tc_PtoCstr(a6 - 0x100));
    RL(A2, out + 2);
    JSR(0x61936, FN_tc_strcpy, tc_strcpy(out + 2, a6 - 0x100));
    RW(D7, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00061962: initialise the file layer (needs the os and memory layers) */
uint32_t fileInit(int16_t flags) {
    ENTER(FN_fileInit, 0x4c, 0x0078);
    RW(D4, flags);
    uint16_t os = (uint16_t)JSR(0x61974, FN_osQueryInit, osQueryInit());
    RW(D3, 0x100);
    if (os < 0x100 || (uint16_t)JSR(0x61984, FN_memQueryInit, memQueryInit()) < 0x100) {
        RW(D6, 0x2870);
        W16(gFileError, 0x2870);
        return 0x2870;
    }
    if (R16(gFileInitCount) == 0) {
        JSR(0x619b4, FN_tc_memset, tc_memset(gFileInitCount, 0, 0x392));
        JSR(0x619c0, FN_fileGetWDPath__FPc, fileGetWDPath__FPc(gFileCurPath));
        uint32_t n = JSR(0x619c8, FN_tc_strlen, tc_strlen(gFileCurPath));
        W8(gFileCurPath + (int16_t)(n - 1), 0);         /* drop the trailing ':' */
        W16(gFileAtRoot, JSR(0x619e0, FN_tc_strchr, tc_strchr(gFileCurPath, ':')) != 0 ? 0 : 1);
        JSR(0x619f8, FN_tc_CtoPstr, tc_CtoPstr(gFileCurPath));
        JSR(0x61a02, FN_fileGetDir, fileGetDir(gFileLaunchDir));
        int16_t err = S16(JSR(0x61a08, FN_fileGetError, fileGetError()));
        RW(D5, err);
        if (err != 0) return (uint16_t)err;
        W8(a6 - 0x4c, R8(gFileCurPath));
        JSR(0x61a28, FN_pstrcat, pstrcat(gFileCurPath, gFileStr5fe));
        _FSMakeFSSpec(0, 0, gFileCurPath, a6 - 0x48);
        W32(gFileCurDirID, R32(a6 - 0x46));     /* the FSSpec's parID */
        W8(gFileCurPath, R8(a6 - 0x4c));
        W16(gFileCurVRefNum, RU16(gFileLaunchDir));
        W16(gFileXceptOff, 1);
        W32(gFileXceptMsg, gFileMsgBuf);
        W16(gFileFlags, flags != 0 ? (uint16_t)flags : 0xffff);
    }
    W16(gFileInitCount, RU16(gFileInitCount) + 1);
    return 0;
}

/* 00061a98: mount the volume in a drive; returns its vRefNum, -1 on error */
uint32_t fileMountVol(int16_t drive) {
    ENTER(FN_fileMountVol, 0, 0x0cf8);
    RW(D3, drive);
    maddr pb = gFilePB;
    RL(A3, pb);
    for (;;) {
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x16, drive);
        int16_t err = S16(_PBMountVol(pb));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return RU16(pb + 0x16);
        }
        uint16_t e = (uint16_t)MAPERR(0x61adc, err);
        RW(D5, e);
        if (XCEPT(0x61ae8, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return 0xffffffff;
    }
}

/* 00061b18: open a file; mode bits: 1 read, 2 write, 0x10/0x20 shared, 0x100 create,
 * 0x200 create new only, 0x4000 truncate, 0x1000 append. Returns the refNum, 0 on error. */
uint32_t fileOpen(int16_t mode, uint32_t spec) {
    ENTER(FN_fileOpen, 0x88, 0x1cf8);
    uint16_t m = (uint16_t)mode;
    RW(D5, m);
    RL(A3, spec);
    RL(D3, spec);
    RL(A4, spec);
    if (R16(spec) == 0 && S16(JSR(0x61b3a, FN_qualify__8fileSpec, qualify__8fileSpec(spec))) != 0)
        return FERR_BADSPEC;
    W16(gFileXceptVol, RU16(spec));
    for (;;) {
        RW(D4, m);
        RL(D3, m);
        W32(a6 - 8, m);
        RL(D3, m & 0x100);
        if ((m & 0x100) || (R32(a6 - 8) & 0x200)) {
            int16_t r = S16(JSR(0x61b7c, FN_fileCreate, fileCreate(1, spec)));
            RW(D7, r);
            if (r != 0) {
                if (r != 0x283e) return 0;      /* not "already exists" */
                if (m & 0x200) return 0;
                r = S16(JSR(0x61baa, FN_fileDelete, fileDelete(spec)));
                RW(D7, r);
                if (r != 0) return 0;
                r = S16(JSR(0x61bc8, FN_fileCreate, fileCreate(1, spec)));
                RW(D7, r);
                if (r != 0) return 0;
            }
        }
        if (m & 1) W8(a6 - 0xe, 1);             /* fsRdPerm */
        if (m & 2) W8(a6 - 0xe, 2);             /* fsWrPerm */
        RW(D4, m);
        RL(D3, m);
        W32(a6 - 4, m);
        RL(D3, m & 1);
        if ((m & 1) && (R32(a6 - 4) & 2)) W8(a6 - 0xe, 3);
        if (m & 0x10) W8(a6 - 0xe, 4);
        if (m & 0x20) W8(a6 - 0xe, 4);
        /* HParamBlock at -0x88 */
        W32(a6 - 0x7c, 0);                      /* ioCompletion */
        RL(A2, spec + 6);
        W32(a6 - 0x76, spec + 6);               /* ioNamePtr */
        W16(a6 - 0x72, RU16(spec));             /* ioVRefNum */
        W32(a6 - 0x58, R32(spec + 2));          /* ioDirID */
        W8(a6 - 0x6d, R8(a6 - 0xe));            /* ioPermssn */
        W32(a6 - 0x6c, 0);                      /* ioMisc */
        int16_t err = S16(JSR(0x61c82, FN_tc_PBHOpen, tc_PBHOpen(ASYNC, a6 - 0x88)));
        RW(D6, err);
        if (err == 0) {
            W16(a6 - 0xc, RU16(a6 - 0x70));     /* ioRefNum */
            if (m & 0x4000) JSR(0x61cac, FN_fileSetEOF, fileSetEOF(0, R16(a6 - 0xc)));
            if (m & 0x1000) {
                /* fileSeek(ref, fileGetEOF(ref), 0): the slot and refNum of fileSeek are
                 * already pushed when fileGetEOF is called */
                maddr s = emu_sp();
                W16(s - 6, RU16(a6 - 0xc));
                uint32_t eof = DEEPER(6, JSR(0x61ccc, FN_fileGetEOF, fileGetEOF(R16(a6 - 0xc))));
                JSR(0x61cd6, FN_fileSeek, fileSeek((int16_t)R8(s - 11), eof, R16(a6 - 0xc)));
            }
            return RU16(a6 - 0xc);
        }
        uint16_t e = (uint16_t)MAPERR(0x61cea, err);
        RW(D7, e);
        if (XCEPT(0x61cf6, err)) continue;
        W16(a6 - 0xa, e);
        W16(gFileError, e);
        return 0;
    }
}

/* 00061d26: no-op */
uint32_t filePrefetch(int16_t p4, int16_t p6, int16_t p8, int16_t p10, int16_t p12) {
    return 0;
}

/* 00061d5e: FSRead(refNum, count, buf) */
uint32_t fileRead(uint32_t count, uint32_t buf, int16_t refNum) {
    ENTER(FN_fileRead, 0, 0x0cf8);
    RL(A2, count);
    RL(A3, buf);
    RW(D3, refNum);
    for (;;) {
        int16_t err = S16(JSR(0x61d7e, FN_tc_FSRead, tc_FSRead(buf, count, refNum)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x61d9c, err);
        RW(D5, e);
        if (XCEPT(0x61da8, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00061dd4: rename a file to a C-string name */
uint32_t fileRename(uint32_t newName, uint32_t spec) {
    ENTER(FN_fileRename, 0x208, 0x1cf8);
    RL(A3, newName);
    RL(A4, spec);
    W32(a6 - 0x208, gFilePB);
    RL(D3, spec);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x61e02, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    for (;;) {
        W8(a6 - 0x204, 0);
        RL(A2, spec + 6);
        JSR(0x61e24, FN_pstrcat, pstrcat(a6 - 0x204, spec + 6));
        JSR(0x61e30, FN_tc_strcpy, tc_strcpy(a6 - 0x104, newName));
        JSR(0x61e3a, FN_tc_CtoPstr, tc_CtoPstr(a6 - 0x104));
        maddr pb = R32(a6 - 0x208);
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W32(pb + 0x12, a6 - 0x204);
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x1c, a6 - 0x104);             /* ioMisc: new name */
        W32(pb + 0x30, R32(spec + 2));
        int16_t err = S16(JSR(0x61e70, FN_tc_PBHRename, tc_PBHRename(ASYNC, pb)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x61e8e, err);
        RW(D5, e);
        if (XCEPT(0x61e9a, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00061ec8: seek; whence (a char: high byte of its word) 0 start, 1 current, 2 end.
 * Returns the new position, -1 on error. */
uint32_t fileSeek(int16_t whence, uint32_t offset, int16_t refNum) {
    ENTER(FN_fileSeek, 0x4, 0x0cf8);
    RL(D3, offset);
    RW(D4, refNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    for (;;) {
        switch ((uint8_t)((uint16_t)whence >> 8)) {
        case 0: W16(pb + 0x2c, 1); break;       /* fsFromStart */
        case 1: W16(pb + 0x2c, 3); break;       /* fsFromMark */
        case 2: W16(pb + 0x2c, 2); break;       /* fsFromLEOF */
        default: W16(pb + 0x2c, 3); break;
        }
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x18, refNum);
        W32(pb + 0x2e, offset);
        int16_t err = S16(JSR(0x61f38, FN_tc_PBSetFPos, tc_PBSetFPos(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            RW(D7, 0);
            W16(gFileError, 0);
            return R32(pb + 0x2e);
        }
        uint16_t e = (uint16_t)MAPERR(0x61f5c, err);
        RW(D6, e);
        if (XCEPT(0x61f68, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        return 0xffffffff;
    }
}

/* 00061f98: set attributes (8: lock+unlock, 4: invisible) */
uint32_t fileSetAttr(int16_t attr, uint32_t spec) {
    ENTER(FN_fileSetAttr, 0x110, 0x1cf8);
    RW(D4, attr);
    RL(A4, spec);
    RL(A2, gFilePB);
    W32(a6 - 0x110, gFilePB);
    W32(a6 - 0x10c, gFilePB);
    RL(D3, spec);
    W32(a6 - 8, spec);
    if (R16(spec) == 0 && S16(JSR(0x61fca, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 8)))) != 0)
        return FERR_BADSPEC;
    for (;;) {
        int16_t err;
        maddr pb;
        W8(a6 - 0x108, 0);
        JSR(0x61fe8, FN_pstrinsert, pstrinsert(a6 - 0x108, spec + 6));
        RL(A2, R32(a6 - 0x10c));
        W32(R32(a6 - 0x10c) + 0xc, 0);
        if (R8(a6 - 0x108) == 0) W16(R32(a6 - 0x10c) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x10c) + 0x1c, 0);
        RL(A2, spec);
        pb = R32(a6 - 0x10c);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x108);
        err = S16(JSR(0x62036, FN_tc_PBGetCatInfo, tc_PBGetCatInfo(ASYNC, pb)));
        RW(D5, err);
        if (err != 0) goto fail;
        pb = R32(a6 - 0x110);
        RL(A2, pb);
        W32(pb + 0xc, 0);
        RL(A3, spec);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x108);
        if ((uint16_t)attr & 8) {
            err = S16(JSR(0x6207a, FN_tc_PBHSetFLock, tc_PBHSetFLock(ASYNC, pb)));
            RW(D5, err);
            if (err != 0) goto fail;
            err = S16(JSR(0x62092, FN_tc_PBHRstFLock, tc_PBHRstFLock(ASYNC, R32(a6 - 0x110))));
            RW(D5, err);
            if (err != 0) goto fail;
        }
        RL(A2, R32(a6 - 0x10c));
        W32(R32(a6 - 0x10c) + 0xc, 0);
        if (R8(a6 - 0x108) == 0) W16(R32(a6 - 0x10c) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x10c) + 0x1c, 0);
        RL(A2, spec);
        pb = R32(a6 - 0x10c);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x108);
        if ((uint16_t)attr & 4) {
            RL(A3, pb + 0x28);
            _BitSet(pb + 0x28, 1);
        } else {
            _BitClr(R32(a6 - 0x10c) + 0x28, 1);
        }
        err = S16(JSR(0x62112, FN_tc_PBSetCatInfo, tc_PBSetCatInfo(ASYNC, R32(a6 - 0x10c))));
        RW(D5, err);
        if (err == 0) {
            RW(D7, 0);
            W16(gFileError, 0);
            return 0;
        }
    fail:;
        uint16_t e = (uint16_t)MAPERR(0x6212c, err);
        RW(D6, e);
        if (XCEPT(0x62138, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        return e;
    }
}

/* 0006216e: make a directory current: gFileCurVRefNum/gFileCurDirID/gFileCurPath */
uint32_t fileSetDir(uint32_t spec) {
    ENTER(FN_fileSetDir, 0x14c, 0x1cf8);
    RL(A3, spec);
    RL(D3, spec);
    RL(A4, spec);
    if (R16(spec) == 0 && S16(JSR(0x6218c, FN_qualify__8fileSpec, qualify__8fileSpec(spec))) != 0) {
        RW(D5, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, spec);
    W16(gFileCurVRefNum, RU16(spec));
    int16_t err = S16(JSR(0x621bc, FN_fileDerivePathFromIDs__FPcslUs,
                          fileDerivePathFromIDs__FPcslUs(1, R32(spec + 2), R16(spec), a6 - 0x14c)));
    RW(D4, err);
    if (err != 0) {
        if (R32(spec + 2) == 1) {               /* the parent of a root: the volume itself */
            W16(gFileAtRoot, 1);
            W32(gFileCurDirID, 2);
            W8(a6 - 0x4c, 1);
            for (;;) {
                uint8_t i = R8(a6 - 0x4c);
                RB(D3, i);
                if (i > R8(gFileCurPath)) break;
                if (R8(gFileCurPath + i) == ':') break;
                W8(a6 - 0x4c, R8(a6 - 0x4c) + 1);
            }
            if (R8(gFileCurPath + R8(a6 - 0x4c)) == ':')
                W8(gFileCurPath, R8(a6 - 0x4c) - 1);
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        RW(D7, err);
        W16(gFileError, err);
        return (uint16_t)err;
    }
    W16(gFileAtRoot, 0);
    JSR(0x6224c, FN_tc_CtoPstr, tc_CtoPstr(a6 - 0x14c));
    W8(gFileCurPath, 0);
    JSR(0x62262, FN_pstrcat, pstrcat(gFileCurPath, a6 - 0x14c));
    JSR(0x62272, FN_pstrcat, pstrcat(gFileCurPath, spec + 6));
    W8(a6 - 0x4b, R8(gFileCurPath));
    JSR(0x62286, FN_pstrcat, pstrcat(gFileCurPath, gFileStr602));
    _FSMakeFSSpec(0, 0, gFileCurPath, a6 - 0x48);
    W32(gFileCurDirID, R32(a6 - 0x46));
    W8(gFileCurPath, R8(a6 - 0x4b));
    W16(a6 - 2, 0);
    W16(gFileError, 0);
    return 0;
}

/* 000622d2 */
uint32_t fileSetEOF(uint32_t eof, int16_t refNum) {
    ENTER(FN_fileSetEOF, 0x4, 0x0cf8);
    RL(D3, eof);
    RW(D4, refNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    for (;;) {
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x18, refNum);
        W32(pb + 0x1c, eof);
        int16_t err = S16(JSR(0x62302, FN_tc_PBSetEOF, tc_PBSetEOF(ASYNC, pb)));
        RW(D5, err);
        if (err == 0) {
            RW(D7, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x62320, err);
        RW(D6, e);
        if (XCEPT(0x6232c, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        return e;
    }
}

/* 0006235a: PBSetCatInfo from the caller's parameter block */
uint32_t fileSetOSAttr(uint32_t pbIn, uint32_t spec) {
    ENTER(FN_fileSetOSAttr, 0x108, 0x1c78);
    RL(A4, pbIn);
    RL(A2, spec);
    W16(gFileXceptVol, RU16(spec));
    W32(a6 - 0x108, pbIn);
    W32(pbIn + 0xc, 0);
    W32(a6 - 4, spec);
    if (R16(spec) == 0 && S16(JSR(0x6238c, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 - 4)))) != 0)
        return FERR_BADSPEC;
    for (;;) {
        W8(a6 - 0x104, 0);
        JSR(0x623b0, FN_pstrinsert, pstrinsert(a6 - 0x104, spec + 6));
        if (R8(a6 - 0x104) == 0) W16(R32(a6 - 0x108) + 0x1c, 0xffff);
        else W16(R32(a6 - 0x108) + 0x1c, 0);
        RL(A2, spec);
        maddr pb = R32(a6 - 0x108);
        RL(A3, pb);
        W32(pb + 0x30, R32(spec + 2));
        W16(pb + 0x16, RU16(spec));
        W32(pb + 0x12, a6 - 0x104);
        int16_t err = S16(JSR(0x623f6, FN_tc_PBSetCatInfo, tc_PBSetCatInfo(ASYNC, pb)));
        RW(D3, err);
        if (err == 0) {
            RW(D5, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x62414, err);
        RW(D4, e);
        if (XCEPT(0x62420, err)) continue;
        RW(D6, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00062456: enable/disable the exception proc; returns the previous setting */
uint32_t fileSetXceptFail(int16_t off) {
    uint16_t old = RU16(gFileXceptOff);
    W16(gFileXceptOff, off);
    return old;
}

/* 00062490: install the exception proc; returns the previous one */
uint32_t fileSetXceptProc(uint32_t proc) {
    uint32_t old = R32(gFileXceptProc);
    W32(gFileXceptProc, proc);
    return old;
}

/* 000624ca: fileSpec::fileSpec() */
uint32_t __ct__8fileSpecDv(uint32_t self) {
    W16(self, 0);
    W32(self + 2, 0);
    W8(self + 6, 0);
    return self;
}

/* 00062506: fileSpec::~fileSpec() */
uint32_t __dt__8fileSpecDv(uint32_t self) {
    return 0;
}

/* 0006252c: fileSpec::fileSpec(const char *name) */
uint32_t __ct__8fileSpecDPCc(uint32_t self, uint32_t name) {
    ENTER(FN___ct__8fileSpecDPCc, 0, 0x1c00);
    RL(A4, self);
    RL(A2, self);
    W16(self, 0);
    W32(self + 2, 0);
    RL(A3, name);
    if (R8(name) == 0 || JSR(0x62552, FN_tc_strlen, tc_strlen(name)) >= 0x100) W8(self + 6, 0);
    else JSR(0x62570, FN_tc_strcpy, tc_strcpy(self + 6, name));
    JSR(0x6257a, FN_qualify__8fileSpec, qualify__8fileSpec(self));
    return self;
}

/* 0006259e: fileSpec::fileSpec(fileSpec dir, const char *name): the directory is passed
 * by value (0x106 bytes at 8(SP)) and modified in place; params [this, vRefNum, dirID,
 * name] (sig override), the rest of the copy is read from the argument block. */
uint32_t __ct__8fileSpecDC8fileSpecPCc(uint32_t self, int16_t dirVol, uint32_t dirID, uint32_t name) {
    ENTER(FN___ct__8fileSpecDC8fileSpecPCc, 0, 0x0c00);
    /* the argument block: from 68k these are the caller's slots already */
    W32(a6 + 8, self);
    W16(a6 + 0xc, dirVol);
    W32(a6 + 0xe, dirID);
    W32(a6 + 0x112, name);
    maddr dir = a6 + 0xc;                       /* the by-value fileSpec */
    W16(R32(a6 + 8), 0);
    W32(R32(a6 + 8) + 2, 0);
    if (R8(R32(a6 + 0x112)) == 0) {
        W8(R32(a6 + 8) + 6, 0);
        JSR(0x625d2, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 + 8)));
        JSR(0x625dc, FN___dt__8fileSpecDv, __dt__8fileSpecDv(dir));
        return R32(a6 + 8);
    }
    maddr rest = R32(a6 + 0x112);
    RL(A3, rest);
    if (R16(a6 + 0xc) == 0) {
        JSR(0x62600, FN_tc_strcpy, tc_strcpy(R32(a6 + 8) + 6, dir + 6));
    } else {
        JSR(0x6261c, FN_fileDerivePathFromIDs__FPcslUs,
            fileDerivePathFromIDs__FPcslUs(1, R32(a6 + 0xe), R16(a6 + 0xc), R32(a6 + 8) + 6));
        if (JSR(0x6262c, FN_tc_memcmp, tc_memcmp(R32(a6 + 0x112), gFileStr606, 2)) != 0) {
            JSR(0x6263c, FN_tc_PtoCstr, tc_PtoCstr(dir + 6));
            JSR(0x6264e, FN_tc_strcat, tc_strcat(R32(a6 + 8) + 6, dir + 6));
            JSR(0x62658, FN_tc_CtoPstr, tc_CtoPstr(dir + 6));
        } else {
            rest = JSR(0x62664, FN_tc_strlen, tc_strlen(R32(a6 + 0x112))) + R32(a6 + 0x112);
            RL(A3, rest);
        }
    }
    uint32_t n1 = JSR(0x62678, FN_tc_strlen, tc_strlen(R32(a6 + 8) + 6));
    RL(A2, n1);
    uint32_t n2 = JSR(0x62684, FN_tc_strlen, tc_strlen(R32(a6 + 0x112)));
    if (n1 + n2 >= 0x100) {
        W8(R32(a6 + 8) + 6, 0);
    } else {
        uint32_t n3 = DEEPER(4, JSR(0x626ac, FN_tc_strlen, tc_strlen(R32(a6 + 8) + 6)));
        JSR(0x626be, FN_fileXlatSpecToMac, fileXlatSpecToMac(rest, n3 + R32(a6 + 8) + 6));
    }
    JSR(0x626ca, FN_qualify__8fileSpec, qualify__8fileSpec(R32(a6 + 8)));
    JSR(0x626d4, FN___dt__8fileSpecDv, __dt__8fileSpecDv(dir));
    return R32(a6 + 8);
}

/* 00062704: fileSpec::fileSpec(short vRefNum, const char *path): "vol:" + path */
uint32_t __ct__8fileSpecDsPCc(uint32_t self, int16_t vRefNum, uint32_t path) {
    ENTER(FN___ct__8fileSpecDsPCc, 0, 0x1c08);
    RL(A4, self);
    RW(D3, vRefNum);
    RL(A2, self);
    W16(self, 0);
    W32(self + 2, 0);
    RL(A2, self + 6);
    if (S16(JSR(0x6272a, FN_fileGetVolName__FsPc, fileGetVolName__FsPc(self + 6, vRefNum))) != 0) {
        W8(self + 6, 0);
    } else if (R8(path) == 0) {
        JSR(0x6274e, FN_tc_strcat, tc_strcat(self + 6, gFileStr60a));
    } else {
        RL(A2, self + 6);
        uint32_t n1 = JSR(0x6275c, FN_tc_strlen, tc_strlen(self + 6));
        RL(A2, n1);
        uint32_t n2 = JSR(0x62768, FN_tc_strlen, tc_strlen(path));
        if (n1 + n2 >= 0x100) {
            W8(self + 6, 0);
        } else {
            RL(A2, path);
            if (R8(path) == '/') {
                RL(A3, self + 6);
                uint32_t n3 = DEEPER(4, JSR(0x62798, FN_tc_strlen, tc_strlen(self + 6)));
                RL(A2, path + 1);
                JSR(0x627aa, FN_fileXlatSpecToMac, fileXlatSpecToMac(path + 1, n3 + self + 6));
            } else {
                RL(A2, self + 6);
                uint32_t n3 = DEEPER(4, JSR(0x627ba, FN_tc_strlen, tc_strlen(self + 6)));
                JSR(0x627cc, FN_fileXlatSpecToMac, fileXlatSpecToMac(path, n3 + self + 6));
            }
        }
    }
    JSR(0x627d6, FN_qualify__8fileSpec, qualify__8fileSpec(self));
    return self;
}

/* 000627fa: fileSpec::fileSpec(FSSpec &) */
uint32_t __ct__8fileSpecDR6FSSpec(uint32_t self, uint32_t fss) {
    mcopy(self, fss, 0x46);
    return self;
}

/* 0006283e: 0 when both specs name the same file, 0x2844 otherwise */
uint32_t equal__8fileSpec(uint32_t a, uint32_t b) {
    ENTER(FN_equal__8fileSpec, 0, 0x1c08);
    RL(A4, a);
    if (R16(b) == 0 || R16(a) == 0) {
        RW(D3, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, b);
    RL(A3, a);
    if (R16(b) == R16(a) && R32(b + 2) == R32(a + 2)) {
        RL(A3, a + 6);
        RL(A2, b + 6);
        if (S16(JSR(0x6288e, FN_pstricmp, pstricmp(b + 6, a + 6))) == 0) return 0;
    }
    return 0x2844;
}

/* 000628c0: file name of a qualified spec (C string) */
uint32_t getName__8fileSpec(uint32_t spec, uint32_t out) {
    ENTER(FN_getName__8fileSpec, 0, 0x1c18);
    RL(A3, spec);
    RL(A4, out);
    if (R16(spec) == 0) {
        RW(D3, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, out);
    W8(out, 0);
    JSR(0x628f6, FN_pstrinsert, pstrinsert(out, spec + 6));
    JSR(0x628fe, FN_tc_PtoCstr, tc_PtoCstr(out));
    RW(D4, 0);
    W16(gFileError, 0);
    return 0;
}

/* 0006292e: full Mac path "vol:dir:name" of a spec */
uint32_t getOSSpec__8fileSpec(uint32_t spec, uint32_t out) {
    ENTER(FN_getOSSpec__8fileSpec, 0, 0x1c38);
    RL(A3, spec);
    RL(A4, out);
    if (R16(spec) == 0) {
        JSR(0x62950, FN_tc_strcpy, tc_strcpy(out, spec + 6));
        RW(D3, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, spec);
    if (S16(JSR(0x62970, FN_fileDerivePathFromIDs__FPcslUs,
                fileDerivePathFromIDs__FPcslUs(1, R32(spec + 2), R16(spec), out))) != 0) {
        RW(D4, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, spec + 6);
    JSR(0x6298e, FN_tc_PtoCstr, tc_PtoCstr(spec + 6));
    JSR(0x6299c, FN_tc_strcat, tc_strcat(out, spec + 6));
    JSR(0x629a8, FN_tc_CtoPstr, tc_CtoPstr(spec + 6));
    RW(D5, 0);
    W16(gFileError, 0);
    return 0;
}

/* 000629de: Unix-style path "/dir/..." of a spec's directory */
uint32_t getPath__8fileSpec(uint32_t spec, uint32_t out) {
    ENTER(FN_getPath__8fileSpec, 0xc, 0x1c78);
    RL(A3, spec);
    RL(A4, out);
    RL(D3, out);
    W32(a6 - 0xc, out);
    W8(out, 0);
    if (R16(spec) == 0) {
        W32(a6 - 8, JSR(0x62a08, FN_tc_strchr, tc_strchr(spec + 6, ':')));
        while (R8(R32(a6 - 8)) != 0) {
            if (R8(R32(a6 - 8)) == ':') {
                maddr d = R32(a6 - 0xc);
                W32(a6 - 0xc, d + 1);
                W8(d, '/');
                maddr s = R32(a6 - 8);
                W32(a6 - 8, s + 1);
                W32(a6 - 4, s);
            } else {
                maddr s = R32(a6 - 8);
                W32(a6 - 8, s + 1);
                uint8_t c = R8(s);
                maddr d = R32(a6 - 0xc);
                W32(a6 - 0xc, d + 1);
                W8(d, c);
            }
        }
        W8(R32(a6 - 4), 0);                     /* uninitialised when no ':' was seen */
        RW(D4, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    RL(A2, spec);
    if (S16(JSR(0x62a7c, FN_fileDerivePathFromIDs__FPcslUs,
                fileDerivePathFromIDs__FPcslUs(0, R32(spec + 2), R16(spec), out + 1))) != 0) {
        W8(out, 0);
        RW(D5, FERR_BADSPEC);
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    W8(out, '/');
    for (;;) {
        maddr p = JSR(0x62aa4, FN_tc_strchr, tc_strchr(out, ':'));
        W32(a6 - 0xc, p);
        if (p == 0) break;
        W8(p, '/');
    }
    maddr last = JSR(0x62abc, FN_tc_strlen, tc_strlen(out)) + out - 1;
    W32(a6 - 0xc, last);
    if (R8(last) == '/') W8(last, 0);
    RW(D6, 0);
    W16(gFileError, 0);
    return 0;
}

/* 00062b04 */
uint32_t getVol__8fileSpec(uint32_t spec, uint32_t out) {
    if (R16(spec) == 0) {
        W16(gFileError, FERR_BADSPEC);
        return FERR_BADSPEC;
    }
    W16(out, RU16(spec));
    W16(gFileError, 0);
    return 0;
}

/* 00062b60: resolve a spec (relative or Unix-style name) into an FSSpec-like spec */
uint32_t qualify__8fileSpec(uint32_t spec) {
    ENTER(FN_qualify__8fileSpec, 0x158, 0x1cf8);
    int16_t err;
    RL(A3, spec);
    W32(a6 - 0x110, gFileStr60c);
    W32(a6 - 0x10c, gFileStr610);
    if (R8(spec + 6) == 0) goto invalid;
    if (R16(spec) != 0) {
        RL(A2, spec + 6);
        err = S16(_FSMakeFSSpec(R16(spec), R32(spec + 2), spec + 6, a6 - 0x158));
        RW(D3, err);
        if (err != 0) {
            if (err == -0x2b) {
                RW(D6, 0);
                W16(gFileError, 0);
                return 0;
            }
            RW(D7, FERR_BADSPEC);
            W16(gFileError, FERR_BADSPEC);
            return FERR_BADSPEC;
        }
        RL(A2, spec);
        if (R32(a6 - 0x156) != R32(spec + 2) || R16(a6 - 0x158) != R16(spec)) {
            mcopy(spec, a6 - 0x158, 0x46);
            W16(a6 - 8, 0);
            W16(gFileError, 0);
            return 0;
        }
    }
    if (JSR(0x62c3e, FN_tc_strchr, tc_strchr(spec + 6, ':')) == 0) {
        RL(A2, spec + 6);
        JSR(0x62c52, FN_tc_strcpy, tc_strcpy(a6 - 0x108, spec + 6));
        JSR(0x62c64, FN_fileXlatSpecToMac, fileXlatSpecToMac(a6 - 0x108, spec + 6));
    }
    if (S16(JSR(0x62c74, FN_validName__8fileSpec, validName__8fileSpec(0, spec + 6))) == 0) goto invalid;
    if (R8(spec + 6) != ':') {
        RL(A2, spec + 6);
        JSR(0x62c92, FN_tc_CtoPstr, tc_CtoPstr(spec + 6));
        err = S16(_FSMakeFSSpec(0, 0, spec + 6, a6 - 0x158));
        RW(D3, err);
        if (err == 0 || err == -0x2b) {
            mcopy(spec, a6 - 0x158, 0x46);
            W16(a6 - 6, 0);
            W16(gFileError, 0);
            return 0;
        }
    }
    {
        RL(A2, spec + 6);
        int16_t dot = JSR(0x62ce8, FN_tc_strcmp, tc_strcmp(spec + 6, gFileStr614)) == 0 ? 1 : 0;
        RW(D4, dot);
        W8(a6 - 0x108, 0);
        JSR(0x62d06, FN_pstrcat, pstrcat(a6 - 0x108, gFileCurPath));
        JSR(0x62d12, FN_tc_CtoPstr, tc_CtoPstr(spec + 6));
        if (dot) JSR(0x62d24, FN_pstrcat, pstrcat(a6 - 0x108, gFileStr618));
        else JSR(0x62d36, FN_pstrcat, pstrcat(a6 - 0x108, spec + 6));
        err = S16(_FSMakeFSSpec(0, 0, a6 - 0x108, a6 - 0x158));
        RW(D3, err);
        if (err == 0 || err == -0x2b) {
            mcopy(spec, a6 - 0x158, 0x46);
            W16(a6 - 4, 0);
            W16(gFileError, 0);
            return 0;
        }
        RL(A2, spec + 6);
        JSR(0x62d84, FN_tc_PtoCstr, tc_PtoCstr(spec + 6));
        W16(spec, 0);
        W32(spec + 2, 0);
        W16(a6 - 2, FERR_BADSPEC);
        W16(gFileError, RU16(a6 - 2));
        return FERR_BADSPEC;
    }
invalid:
    RL(A2, spec);
    W8(spec + 6, 0);
    W16(spec, 0);
    W32(spec + 2, 0);
    RW(D5, FERR_BADSPEC);
    W16(gFileError, FERR_BADSPEC);
    return FERR_BADSPEC;
}

/* 00062dc6: name length <= 0x100 and no path component longer than 31 characters */
uint32_t validName__8fileSpec(int16_t maxLen, uint32_t name) {
    ENTER(FN_validName__8fileSpec, 0xc, 0x1c38);
    uint16_t len = (uint16_t)maxLen;
    RW(D5, len);
    RL(A3, name);
    if (name == 0 || R8(name) == 0) return 0;
    if (len != 0) {
        uint32_t n = JSR(0x62df4, FN_tc_strlen, tc_strlen(name));
        RW(D4, len);
        uint32_t lim = len;
        RL(D3, lim);
        if (n <= lim) {
            lim = JSR(0x62e08, FN_tc_strlen, tc_strlen(name));
            RL(D3, lim);
        }
        len = (uint16_t)lim;
    } else {
        len = (uint16_t)JSR(0x62e16, FN_tc_strlen, tc_strlen(name));
    }
    RW(D5, len);
    if (len > 0x100) return 0;
    W8(a6 - 8, JSR(0x62e32, FN_tc_strchr, tc_strchr(name, ':')) != 0 ? ':' : '/');
    W32(a6 - 0xc, name);
    W32(a6 - 4, name + len);
    for (maddr p = name; R8(p) != 0 && p < R32(a6 - 4); p++) {
        uint8_t c = R8(p);
        if (c == R8(a6 - 8) || c == 0) {
            if ((int32_t)(p - R32(a6 - 0xc)) > 0x1f) return 0;
            W32(a6 - 0xc, p + 1);
        }
    }
    return 1;
}

/* 00062ebe: unmount a volume (not the current directory's) */
uint32_t fileUnmountVol(int16_t vRefNum) {
    ENTER(FN_fileUnmountVol, 0x110, 0x0cf8);
    RW(D3, vRefNum);
    maddr pb = gFilePB;
    RL(A3, pb);
    JSR(0x62ed8, FN___ct__8fileSpecDv, __ct__8fileSpecDv(a6 - 0x110));
    JSR(0x62ee2, FN_fileGetDir, fileGetDir(a6 - 0x110));
    if (R16(a6 - 0x110) == vRefNum) {
        W16(a6 - 8, 0x283d);
        W16(gFileError, RU16(a6 - 8));
        W16(a6 - 0xa, 0x283d);
        JSR(0x62f04, FN___dt__8fileSpecDv, __dt__8fileSpecDv(a6 - 0x110));
        return RU16(a6 - 0xa);
    }
    for (;;) {
        RL(A2, pb);
        W32(pb + 0xc, 0);
        W16(pb + 0x16, vRefNum);
        int16_t err = S16(_PBUnmountVol(pb));
        RW(D4, err);
        if (err == 0) {
            W16(a6 - 6, 0);
            W16(gFileError, 0);
            RW(D7, 0);
            JSR(0x62f3a, FN___dt__8fileSpecDv, __dt__8fileSpecDv(a6 - 0x110));
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x62f4c, err);
        RW(D5, e);
        if (XCEPT(0x62f58, err)) continue;
        W16(a6 - 4, e);
        W16(gFileError, e);
        RW(D6, e);
        JSR(0x62f72, FN___dt__8fileSpecDv, __dt__8fileSpecDv(a6 - 0x110));
        return e;
    }
}

/* 00062f98: FSWrite(refNum, count, buf) */
uint32_t fileWrite(uint32_t count, uint32_t buf, int16_t refNum) {
    ENTER(FN_fileWrite, 0, 0x0cf8);
    RL(A2, count);
    RL(A3, buf);
    RW(D3, refNum);
    for (;;) {
        int16_t err = S16(JSR(0x62fb8, FN_tc_FSWrite, tc_FSWrite(buf, count, refNum)));
        RW(D4, err);
        if (err == 0) {
            RW(D6, 0);
            W16(gFileError, 0);
            return 0;
        }
        uint16_t e = (uint16_t)MAPERR(0x62fd6, err);
        RW(D5, e);
        if (XCEPT(0x62fe2, err)) continue;
        RW(D7, e);
        W16(gFileError, e);
        return e;
    }
}

/* 00063010: master record (14 bytes) of a memory-layer handle number (1-based: page
 * (n-1)>>9, slot (n-1)&0x1ff), or 0 when it is out of range or free (bit 2 of +4).
 * Pascal, result in D0 ("ret": 4). */
uint32_t getMaster__FUs(int16_t handle) {
    uint16_t n = (uint16_t)(handle - 1);
    uint16_t page = n >> 9;
    if ((int16_t)page > R16(gMemPageCount)) {
        W16(gMemPrevError, RU16(gMemError));
        W16(gMemError, 0x27a7);
        return 0;
    }
    uint32_t rec = (uint32_t)(n % 0x200) * 14 + R32(gMemPages + (int16_t)(page * 4));
    if (rec == 0 || ((RU16(rec + 4) >> 2) & 1) == 0) {
        W16(gMemPrevError, RU16(gMemError));
        W16(gMemError, 0x27a7);
        return 0;
    }
    W16(gMemPrevError, RU16(gMemError));
    W16(gMemError, 0);
    return rec;
}

/* 000630c6: first free master record; *num = its handle number. Result in D0. */
uint32_t findEmptySlot__FRs(uint32_t num) {
    for (int16_t page = 0; page < R16(gMemPageCount); page++) {
        maddr rec = R32(gMemPages + (int16_t)(page * 4));
        for (int16_t i = 0; i < 0x200; i++, rec += 14) {
            if (((RU16(rec + 4) >> 2) & 1) == 0) {
                W16(num, (page << 9) + i + 1);
                return rec;
            }
        }
    }
    return 0;
}
