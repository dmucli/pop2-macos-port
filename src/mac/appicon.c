/* The application icon, taken from the user's copy of the game at launch (the app
 * bundle ships no publisher artwork): BNDL/FREF give the 'APPL' icon, drawn from its
 * 'icl8' (8-bit, standard Mac palette) and 'ICN#' mask. */
#include "mac.h"
#include <stdlib.h>

void macos_set_app_icon(const uint8_t *rgba, int w, int h);

static void std8(int i, uint8_t rgb[3]) {      /* the standard Mac 8-bit color table */
    static const uint8_t lv[6] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
    static const uint8_t ramp[10] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
    if (i < 215) { rgb[0] = lv[i / 36]; rgb[1] = lv[i / 6 % 6]; rgb[2] = lv[i % 6]; return; }
    if (i == 255) { rgb[0] = rgb[1] = rgb[2] = 0; return; }
    int c = (i - 215) / 10, k = (i - 215) % 10;
    uint8_t v = ramp[k];
    rgb[0] = (c == 0 || c == 3) ? v : 0; rgb[1] = (c == 1 || c == 3) ? v : 0; rgb[2] = (c == 2 || c == 3) ? v : 0;
}

/* resource id of the icon family for the application (FREF type 'APPL') */
static int16_t appl_icon_id(void) {
    maddr bndl = GetResource(FOURCC('B','N','D','L'), 128);
    if (!bndl || !rd32(bndl)) return 128;
    int16_t found = 128;
    maddr b = rd32(bndl);
    int ntypes = rd16(b + 6) + 1;
    maddr p = b + 8, icn = 0, fref = 0; int nicn = 0, nfref = 0;
    for (int t = 0; t < ntypes; t++) {
        uint32_t type = rd32(p); int n = rd16(p + 4) + 1;
        if (type == FOURCC('I','C','N','#')) { icn = p + 6; nicn = n; }
        if (type == FOURCC('F','R','E','F')) { fref = p + 6; nfref = n; }
        p += 6 + 4 * n;
    }
    for (int i = 0; i < nfref; i++) {
        maddr fh = GetResource(FOURCC('F','R','E','F'), (int16_t)rd16(fref + 4 * i + 2));
        if (!fh) continue;
        if (rd32(fh) && rd32(rd32(fh)) == FOURCC('A','P','P','L')) {
            int16_t local = (int16_t)rd16(rd32(fh) + 4);
            for (int k = 0; k < nicn; k++) if ((int16_t)rd16(icn + 4 * k) == local) found = (int16_t)rd16(icn + 4 * k + 2);
        }
        ReleaseResource(fh);
    }
    ReleaseResource(bndl);
    return found;
}

void runtime_set_app_icon(void) {
    int16_t id = appl_icon_id();
    maddr icl8 = GetResource(FOURCC('i','c','l','8'), id), icnm = GetResource(FOURCC('I','C','N','#'), id);
    if (!icl8 || !rd32(icl8) || !icnm || !rd32(icnm)) {
        if (icl8) ReleaseResource(icl8);
        if (icnm) ReleaseResource(icnm);
        return;
    }
    enum { S = 4, W = 32 * S };                  /* nearest-neighbour 4x: crisp pixels */
    uint8_t *rgba = calloc(W * W, 4);
    maddr pix = rd32(icl8), mask = rd32(icnm) + 128;
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        uint8_t c[3]; std8(rd8(pix + 32 * y + x), c);
        int on = (rd8(mask + 4 * y + x / 8) >> (7 - x % 8)) & 1;
        for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) {
            uint8_t *o = rgba + 4 * ((y * S + dy) * W + x * S + dx);
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = on ? 255 : 0;
        }
    }
    ReleaseResource(icl8); ReleaseResource(icnm);   /* leave the game's heap as it was */
    macos_set_app_icon(rgba, W, W);
    free(rgba);
}
