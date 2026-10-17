/* Native macOS menu bar mirroring the game's Menu Manager menus.
 *
 * The emulated Menu Manager (src/mac/wm.c) calls platform_menu_sync() whenever menus
 * change; the menu bar is rebuilt on the next platform_poll(). Choosing an item:
 *  - with a Command-key equivalent: the game receives that Command-key press, exactly as
 *    if the user had typed it (the game handles those during play, menu bar hidden);
 *  - otherwise: the game receives a click in its menu bar and MenuSelect returns the
 *    chosen item (platform_menu_pending). */
#import <Cocoa/Cocoa.h>
#include "../mac/mem.h"
#include "../mac/mods.h"
#include "../mac/states.h"
#include "platform.h"

int   wm_menu_count(void);
maddr wm_menu_handle(int i);
void  event_menu_command_key(uint8_t ch);
void  event_menu_click(void);
void  platform_toggle_fullscreen(void);
void  runtime_quit(void);
void  event_release_all(void);
static void controls_window(void);
static void states_window(void);

static int dirty = 1;
static uint32_t pending;

void platform_menu_sync(void) { dirty = 1; }
int  platform_menu_has_pending(void) { return pending != 0; }
int  platform_menu_pending(uint32_t *r) { *r = pending; pending = 0; return *r != 0; }

static NSString *mac_str(maddr p) {
    NSString *s = [[NSString alloc] initWithBytes:mptr(p + 1) length:rd8(p) encoding:NSMacOSRomanStringEncoding];
    return s ? s : @"";
}

/* One game menu item, decoded from the MenuInfo record. */
typedef struct { NSString *text; uint8_t cmd, mark; int enabled; } GameItem;

static int game_items(maddr h, GameItem *out, int max) {
    maddr m = rd32(h) & ADDR_MASK;
    if (!m) return 0;
    uint32_t flags = rd32(m + 10);
    maddr p = m + 14 + 1 + rd8(m + 14);          /* skip the title */
    int n = 0;
    for (int k = 1; rd8(p) && n < max; k++) {
        GameItem it = { mac_str(p), 0, 0, 0 };
        maddr a = p + 1 + rd8(p);
        it.cmd = rd8(a + 1); it.mark = rd8(a + 2);
        it.enabled = (flags & 1) && (k >= 32 || (flags & (1u << k)));
        out[n++] = it;
        p = a + 4;
    }
    return n;
}

@interface PoPMenuTarget : NSObject
@end
@implementation PoPMenuTarget
- (void)choose:(NSMenuItem *)item {
    NSString *key = item.representedObject;
    if (key.length) event_menu_command_key((uint8_t)[key characterAtIndex:0]);
    else { pending = (uint32_t)item.tag; event_menu_click(); }
}
- (void)fullscreen:(id)sender { (void)sender; platform_toggle_fullscreen(); }
/* Extras */
- (void)toggleMod:(NSMenuItem *)item { mods_set((int)item.tag, !mods_get((int)item.tag)); platform_menu_sync(); }
- (void)gotoLevel:(NSMenuItem *)item { mods_goto_level((int)item.tag); }
- (void)nextLevel:(id)sender { (void)sender; mods_next_level(); platform_menu_sync(); }
- (void)addTime:(id)sender { (void)sender; mods_add_time(); }
- (void)addHealth:(id)sender { (void)sender; mods_add_health(); }
- (void)quickSave:(id)sender { (void)sender; mods_quick_save(); }
- (void)quickLoad:(id)sender { (void)sender; mods_quick_load(); }
- (void)saveStates:(id)sender { (void)sender; states_window(); }
- (void)overlayMenu:(id)sender { (void)sender; overlay_menu_request(); }
- (void)controls:(id)sender { (void)sender; controls_window(); }
- (void)displayMode:(NSMenuItem *)item { platform_set_display_mode((int)item.tag); platform_menu_sync(); }
/* Quit goes through the game's own Quit item when it has one (so it can save settings). */
- (void)quit:(id)sender {
    (void)sender;
    for (int i = 0; i < wm_menu_count(); i++) {
        maddr h = wm_menu_handle(i);
        GameItem items[64];
        int n = game_items(h, items, 64);
        for (int k = 0; k < n; k++) {
            if ([items[k].text hasPrefix:@"Quit"] && items[k].enabled) {
                if (items[k].cmd) event_menu_command_key(items[k].cmd);
                else { pending = (uint32_t)rd16(rd32(h)) << 16 | (uint32_t)(k + 1); event_menu_click(); }
                return;
            }
        }
    }
    runtime_quit();
}
@end

static PoPMenuTarget *target;

/* mine: the action is implemented by PoPMenuTarget (otherwise it goes to NSApp / the window) */
static NSMenuItem *add(NSMenu *m, NSString *title, SEL action, NSString *key, int mine) {
    NSMenuItem *it = [m addItemWithTitle:title action:action keyEquivalent:key ? key : @""];
    if (mine) it.target = target;
    return it;
}

static void fill_game_menu(NSMenu *menu, maddr h, int skip_quit) {
    int16_t id = (int16_t)rd16(rd32(h));
    GameItem items[64];
    int n = game_items(h, items, 64);
    for (int k = 0; k < n; k++) {
        GameItem *g = &items[k];
        if ([g->text hasPrefix:@"-"]) { [menu addItem:[NSMenuItem separatorItem]]; continue; }
        if (skip_quit && [g->text hasPrefix:@"Quit"]) {
            /* the app menu's Quit replaces it; drop a separator left at the end */
            if (menu.numberOfItems && menu.itemArray.lastObject.separatorItem) [menu removeItemAtIndex:menu.numberOfItems - 1];
            continue;
        }
        NSString *key = g->cmd ? [[NSString stringWithFormat:@"%c", g->cmd] lowercaseString] : @"";
        NSMenuItem *it = [menu addItemWithTitle:g->text action:@selector(choose:) keyEquivalent:key];
        it.target = target;
        it.tag = (NSInteger)((uint32_t)(uint16_t)id << 16 | (uint32_t)(k + 1));
        it.representedObject = g->cmd ? [NSString stringWithFormat:@"%c", g->cmd] : nil;
        it.enabled = g->enabled;
        it.state = g->mark ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

void platform_menu_update(void) {
    if (!dirty) return;
    dirty = 0;
    @autoreleasepool {
        if (!target) {
            target = [[PoPMenuTarget alloc] init];
        }
        NSString *app = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
        if (!app) app = [[NSProcessInfo processInfo] processName];
        NSMenu *bar = [[NSMenu alloc] init];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:app];
        appMenu.autoenablesItems = NO;
        /* the game's Apple menu ("About ...") goes at the top of the app menu */
        for (int i = 0; i < wm_menu_count(); i++) {
            maddr h = wm_menu_handle(i);
            maddr m = rd32(h) & ADDR_MASK;
            if (m && rd8(m + 14) == 1 && rd8(m + 15) == 0x14) { fill_game_menu(appMenu, h, 0); [appMenu addItem:[NSMenuItem separatorItem]]; }
        }
        add(appMenu, [@"Hide " stringByAppendingString:app], @selector(hide:), @"h", 0);
        NSMenuItem *others = add(appMenu, @"Hide Others", @selector(hideOtherApplications:), @"h", 0);
        others.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        add(appMenu, @"Show All", @selector(unhideAllApplications:), nil, 0);
        [appMenu addItem:[NSMenuItem separatorItem]];
        add(appMenu, [@"Quit " stringByAppendingString:app], @selector(quit:), @"q", 1);
        [bar addItemWithTitle:app action:NULL keyEquivalent:@""].submenu = appMenu;

        for (int i = 0; i < wm_menu_count(); i++) {
            maddr h = wm_menu_handle(i);
            maddr m = rd32(h) & ADDR_MASK;
            if (!m || (rd8(m + 14) == 1 && rd8(m + 15) == 0x14)) continue;
            NSString *title = mac_str(m + 14);
            NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
            menu.autoenablesItems = NO;
            fill_game_menu(menu, h, 1);
            NSMenuItem *top = [bar addItemWithTitle:title action:NULL keyEquivalent:@""];
            top.submenu = menu;
            top.enabled = (rd32(m + 10) & 1) != 0;
        }

        NSMenu *ex = [[NSMenu alloc] initWithTitle:@"Extras"];
        ex.autoenablesItems = NO;
        static NSString *const mod_titles[MOD_COUNT] = { @"Cheat Mode", @"Infinite Time", @"Infinite Health",
                                                          @"Rewind (hold Delete)", @"Skip Copy Protection" };
        const int order[] = { MOD_INFINITE_HEALTH, MOD_INFINITE_TIME, MOD_CHEAT_MODE, -1 };
        for (int i = 0; order[i] >= 0; i++) {
            NSMenuItem *it = add(ex, mod_titles[order[i]], @selector(toggleMod:), nil, 1);
            it.tag = order[i]; it.state = mods_get(order[i]) ? NSControlStateValueOn : NSControlStateValueOff;
        }
        NSMenuItem *nl = add(ex, @"Skip to Next Level", @selector(nextLevel:), @"n", 1);
        nl.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        NSMenu *lv = [[NSMenu alloc] initWithTitle:@"Go to Level"];
        lv.autoenablesItems = NO;
        for (int l = 1; l <= 14; l++) {
            NSMenuItem *it = add(lv, [NSString stringWithFormat:@"Level %d", l], @selector(gotoLevel:), nil, 1);
            it.tag = l;
        }
        NSMenuItem *lvi = [ex addItemWithTitle:@"Go to Level" action:NULL keyEquivalent:@""];
        lvi.submenu = lv;
        add(ex, @"Add 5 Minutes", @selector(addTime:), nil, 1);
        add(ex, @"Add One Health", @selector(addHealth:), nil, 1);
        [ex addItem:[NSMenuItem separatorItem]];
        NSMenuItem *qs = add(ex, [NSString stringWithFormat:@"Quick Save (%s)", input_key_name(input_action_key(ACT_QSAVE))],
                             @selector(quickSave:), @"s", 1);
        qs.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        NSMenuItem *ql = add(ex, [NSString stringWithFormat:@"Load Newest Save State (%s)", input_key_name(input_action_key(ACT_QLOAD))],
                             @selector(quickLoad:), @"l", 1);
        ql.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        add(ex, @"Save States\u2026", @selector(saveStates:), nil, 1);
        NSMenuItem *rw = add(ex, mod_titles[MOD_REWIND], @selector(toggleMod:), nil, 1);
        rw.tag = MOD_REWIND; rw.state = mods_get(MOD_REWIND) ? NSControlStateValueOn : NSControlStateValueOff;
        [ex addItem:[NSMenuItem separatorItem]];
        NSMenuItem *cp = add(ex, mod_titles[MOD_SKIP_COPYPROT], @selector(toggleMod:), nil, 1);
        cp.tag = MOD_SKIP_COPYPROT; cp.state = mods_get(MOD_SKIP_COPYPROT) ? NSControlStateValueOn : NSControlStateValueOff;
        add(ex, @"Controls\u2026", @selector(controls:), nil, 1);
        add(ex, [NSString stringWithFormat:@"On-Screen Menu (%s)", input_key_name(input_action_key(ACT_MENU))], @selector(overlayMenu:), nil, 1);
        [bar addItemWithTitle:@"Extras" action:NULL keyEquivalent:@""].submenu = ex;

        NSMenu *win = [[NSMenu alloc] initWithTitle:@"View"];   /* not "Window": AppKit would add its own items */
        win.autoenablesItems = NO;
        static NSString *const disp[DISPLAY_COUNT] = { @"Pixels", @"Sharp Pixels (whole multiples)", @"Smooth", @"CRT Scanlines" };
        for (int m = 0; m < DISPLAY_COUNT; m++) {
            NSMenuItem *it = add(win, disp[m], @selector(displayMode:), nil, 1);
            it.tag = m; it.state = platform_display_mode() == m ? NSControlStateValueOn : NSControlStateValueOff;
        }
        [win addItem:[NSMenuItem separatorItem]];
        NSMenuItem *fs = add(win, @"Toggle Full Screen", @selector(fullscreen:), @"f", 1);
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        [bar addItemWithTitle:@"View" action:NULL keyEquivalent:@""].submenu = win;
        [NSApp setMainMenu:bar];
        /* debug: POP2_MENU_DUMP=1 prints the menus; POP2_MENU_SELECT="Menu/Item" chooses an
         * item (by title prefix) the first time it exists and is enabled */
        if (getenv("POP2_MENU_DUMP"))
            for (NSMenuItem *t in bar.itemArray)
                for (NSMenuItem *it in t.submenu.itemArray)
                    fprintf(stderr, "[menu] %s / %s%s%s%s\n", t.title.UTF8String, it.separatorItem ? "-" : it.title.UTF8String,
                            it.keyEquivalent.length ? " (cmd-" : "", it.keyEquivalent.UTF8String, it.keyEquivalent.length ? ")" : "");
        static int selected;
        const char *sel = getenv("POP2_MENU_SELECT");
        if (sel && !selected && strchr(sel, '/')) {
            NSString *s = [NSString stringWithUTF8String:sel];
            NSArray *parts = [s componentsSeparatedByString:@"/"];
            for (NSMenuItem *t in bar.itemArray) if ([t.title hasPrefix:parts[0]])
                for (NSMenuItem *it in t.submenu.itemArray)
                    if (!it.separatorItem && it.enabled && [it.title hasPrefix:parts[1]] && it.target) {
                        fprintf(stderr, "[menu] choosing %s / %s\n", t.title.UTF8String, it.title.UTF8String);
                        selected = 1;
                        [NSApp sendAction:it.action to:it.target from:it];
                    }
        }
    }
}

/* Called before SDL creates the application: AppKit adds Dictation, Emoji & Symbols
 * (Cmd-E, the game's End Game) and AutoFill to any menu titled "Edit" unless disabled. */
void platform_menu_early_init(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{
            @"NSDisabledDictationMenuItem": @YES, @"NSDisabledCharacterPaletteMenuItem": @YES,
            @"NSDisabledAutoFillMenuItem": @YES, @"NSDisabledAutoFillHeuristicMenuItem": @YES }];
    }
}

/* ---- Extras > Controls...: one row per action, a keyboard and a controller column;
 * click a button, then press a key or a controller button ---- */
@interface PoPKeyCatcher : NSView
@property int waiting;           /* a capture is in progress */
@property(copy) void (^done)(int keycode);
@end
@implementation PoPKeyCatcher
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }
- (void)keyDown:(NSEvent *)e {
    if (!self.waiting) { [super keyDown:e]; return; }
    int k = e.keyCode;
    if (k == 0x35) k = -2;       /* Esc: cancel */
    self.done(k);
}
@end

@interface PoPControls : NSObject <NSWindowDelegate>
@property(strong) NSWindow *w;
@property(strong) NSMutableArray<NSButton *> *keys, *pads;
@property(strong) NSTextField *padName;
@property(strong) PoPKeyCatcher *catcher;
@property(strong) NSTimer *timer;
@property int padAction;         /* action waiting for a controller input, -1 = none */
@property int gen;
@end
@implementation PoPControls
- (void)enable:(BOOL)on {
    int have = input_pad_name() != NULL;
    for (NSButton *o in self.keys) o.enabled = on;
    for (NSButton *o in self.pads) o.enabled = on && have;
}
- (void)refresh {
    const char *n = input_pad_name();
    int cnt = input_pad_count();
    self.padName.stringValue = !n ? @"No controller connected. Connect one to assign its buttons."
        : cnt > 1 ? [NSString stringWithFormat:@"Controller: %s (and %d more; all use these buttons)", n, cnt - 1]
                  : [NSString stringWithFormat:@"Controller: %s", n];
    for (int a = 0; a < ACT_COUNT; a++) {
        self.keys[a].title = [NSString stringWithUTF8String:input_key_name(input_action_key(a))];
        if (a != self.padAction)
            self.pads[a].title = [NSString stringWithUTF8String:input_pad_code_name(input_action_pad(a))];
    }
}
- (void)finish {
    self.catcher.waiting = 0;
    self.padAction = -1;
    [self enable:YES];
    [self refresh];
}
- (void)pickKey:(NSButton *)b {
    int a = (int)b.tag;
    [self enable:NO];
    b.title = @"Press a key…";
    self.catcher.waiting = 1;
    __weak PoPControls *me = self;
    self.catcher.done = ^(int k) {
        if (k != -2) input_set_action_key(a, k);
        [me finish];
    };
    [self.w makeFirstResponder:self.catcher];
}
- (void)pickPad:(NSButton *)b {
    int a = (int)b.tag;
    [self enable:NO];
    b.title = @"Press a button…";
    self.padAction = a;
    self.catcher.waiting = 1;
    __weak PoPControls *me = self;
    self.catcher.done = ^(int k) { (void)k; [me finish]; };    /* any key: cancel */
    [self.w makeFirstResponder:self.catcher];
}
- (void)tick:(NSTimer *)t {
    (void)t;
    int c = input_pad_capture_poll();
    if (self.gen != input_pad_generation()) {          /* connected / removed */
        self.gen = input_pad_generation();
        if (self.padAction >= 0 && !input_pad_name()) [self finish];
        else { if (!self.catcher.waiting) [self enable:YES]; [self refresh]; }
    }
    if (c >= 0 && self.padAction >= 0) {
        input_set_action_pad(self.padAction, c);
        [self finish];
    }
}
- (void)defaults:(id)sender {
    (void)sender;
    for (int a = 0; a < ACT_COUNT; a++) { input_set_action_key(a, -1); input_set_action_pad(a, -1); }
    for (int a = 0; a < ACT_COUNT; a++) {
        input_set_action_key(a, input_action_default_key(a));
        input_set_action_pad(a, input_action_default_pad(a));
    }
    [self refresh];
}
- (void)close:(id)sender { (void)sender; [NSApp stopModal]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; [NSApp stopModal]; }
@end

static NSTextField *label(NSView *v, NSString *s, NSRect r, NSFont *f) {
    NSTextField *l = [NSTextField labelWithString:s];
    l.frame = r;
    if (f) l.font = f;
    [v addSubview:l];
    return l;
}

static void controls_window(void) {
    @autoreleasepool {
        PoPControls *c = [[PoPControls alloc] init];
        const CGFloat rowh = 30, wdt = 680, colk = 300, colp = 490, colw = 170;
        const CGFloat top = 100;                         /* info, controller name, column titles */
        CGFloat h = top + ACT_COUNT * rowh + 60;
        c.w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, wdt, h)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered defer:NO];
        c.w.title = @"Controls";
        c.w.releasedWhenClosed = NO;
        c.w.delegate = c;
        c.padAction = -1;
        c.catcher = [[PoPKeyCatcher alloc] initWithFrame:NSMakeRect(0, 0, wdt, h)];
        c.w.contentView = c.catcher;
        NSTextField *info = [NSTextField wrappingLabelWithString:
            @"Click a button, then press the new key or controller button (Esc cancels). "
             "The default keys keep working, Shift and Control stay the action and second buttons, "
             "and the left stick moves unless one of its directions is assigned."];
        info.frame = NSMakeRect(20, 12, wdt - 40, 34);
        info.font = [NSFont systemFontOfSize:11];
        [c.catcher addSubview:info];
        c.padName = label(c.catcher, @"", NSMakeRect(20, 52, wdt - 40, 20), [NSFont boldSystemFontOfSize:12]);
        NSFont *hf = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
        label(c.catcher, @"Keyboard", NSMakeRect(colk + 4, top - 26, colw, 18), hf);
        label(c.catcher, @"Controller", NSMakeRect(colp + 4, top - 26, colw, 18), hf);
        c.keys = [NSMutableArray array];
        c.pads = [NSMutableArray array];
        for (int a = 0; a < ACT_COUNT; a++) {
            CGFloat y = top + a * rowh;
            label(c.catcher, [NSString stringWithUTF8String:input_action_name(a)], NSMakeRect(20, y + 5, colk - 30, 20), nil);
            NSButton *k = [NSButton buttonWithTitle:@"" target:c action:@selector(pickKey:)];
            k.frame = NSMakeRect(colk, y, colw, 28); k.tag = a;
            [c.catcher addSubview:k]; [c.keys addObject:k];
            NSButton *p = [NSButton buttonWithTitle:@"" target:c action:@selector(pickPad:)];
            p.frame = NSMakeRect(colp, y, colw, 28); p.tag = a;
            [c.catcher addSubview:p]; [c.pads addObject:p];
        }
        NSButton *def = [NSButton buttonWithTitle:@"Restore Defaults" target:c action:@selector(defaults:)];
        def.frame = NSMakeRect(20, h - 44, 150, 30);
        [c.catcher addSubview:def];
        NSButton *ok = [NSButton buttonWithTitle:@"Done" target:c action:@selector(close:)];
        ok.frame = NSMakeRect(wdt - 120, h - 44, 100, 30);
        ok.keyEquivalent = @"\r";
        [c.catcher addSubview:ok];

        input_pad_capture(1);
        input_pad_capture_poll();                        /* controllers connected meanwhile */
        c.gen = input_pad_generation();
        [c enable:YES];
        [c refresh];
        c.timer = [NSTimer timerWithTimeInterval:1.0 / 60 target:c selector:@selector(tick:) userInfo:nil repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:c.timer forMode:NSRunLoopCommonModes];
        [c.w center];
        [NSApp runModalForWindow:c.w];
        [c.timer invalidate];
        [c.w orderOut:nil];
        input_pad_capture(0);
        event_release_all();
        platform_menu_sync();                            /* quick save / load key names */
    }
}

/* ---- Extras > Save States...: the history, newest first, with a thumbnail, a name that
 * can be edited, the level, the time left and the date; Load, Delete ---- */
@interface PoPStates : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate>
@property(strong) NSWindow *w;
@property(strong) NSTableView *table;
@property(strong) NSButton *load, *del;
@property(strong) NSTextField *empty;
@property(strong) NSDateFormatter *fmt;
@end
@implementation PoPStates
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { (void)t; return states_count(); }
- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    (void)col;
    StateInfo si;
    if (!states_info((int)row, &si)) return nil;
    NSTableCellView *v = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, t.bounds.size.width, 80)];
    NSImageView *iv = [[NSImageView alloc] initWithFrame:NSMakeRect(6, 4, 96, 72)];
    iv.imageScaling = NSImageScaleProportionallyUpOrDown;
    iv.wantsLayer = YES; iv.layer.borderWidth = 1; iv.layer.borderColor = NSColor.separatorColor.CGColor;
    uint8_t *px = malloc(STATE_THUMB_W * STATE_THUMB_H * 4);
    if (px && states_thumb((int)row, px)) {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:STATE_THUMB_W
            pixelsHigh:STATE_THUMB_H bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:STATE_THUMB_W * 4 bitsPerPixel:32];
        memcpy(rep.bitmapData, px, STATE_THUMB_W * STATE_THUMB_H * 4);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(STATE_THUMB_W, STATE_THUMB_H)];
        [img addRepresentation:rep];
        iv.image = img;
    }
    free(px);
    [v addSubview:iv];
    CGFloat x = 116, wd = t.bounds.size.width - x - 12;
    NSTextField *name = [NSTextField textFieldWithString:[NSString stringWithUTF8String:si.name]];
    name.placeholderString = [NSString stringWithFormat:@"Level %d", si.level];
    name.bordered = NO; name.drawsBackground = NO; name.editable = YES;
    name.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
    name.frame = NSMakeRect(x, 46, wd, 22);
    name.tag = row; name.delegate = self;
    name.toolTip = @"Click to rename";
    [v addSubview:name];
    NSString *left = si.minutes == 1 ? @"1 minute left" : [NSString stringWithFormat:@"%d minutes left", si.minutes];
    NSString *detail = si.name[0] ? [NSString stringWithFormat:@"Level %d · %@", si.level, left] : left;
    NSTextField *d1 = [NSTextField labelWithString:detail];
    d1.frame = NSMakeRect(x + 2, 26, wd, 18); d1.textColor = NSColor.secondaryLabelColor;
    [v addSubview:d1];
    NSDate *when = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)si.time];
    NSTextField *d2 = [NSTextField labelWithString:[self.fmt stringFromDate:when]];
    d2.frame = NSMakeRect(x + 2, 8, wd, 18); d2.textColor = NSColor.secondaryLabelColor;
    d2.font = [NSFont systemFontOfSize:11];
    [v addSubview:d2];
    return v;
}
- (void)controlTextDidEndEditing:(NSNotification *)n {
    NSTextField *f = n.object;
    StateInfo si;
    if (!states_info((int)f.tag, &si)) return;
    const char *s = f.stringValue.UTF8String ? f.stringValue.UTF8String : "";
    if (strcmp(s, si.name)) states_rename((int)f.tag, s);
}
- (void)tableViewSelectionDidChange:(NSNotification *)n { (void)n; [self update]; }
- (void)update {
    BOOL sel = self.table.selectedRow >= 0;
    self.load.enabled = sel; self.del.enabled = sel;
    self.empty.hidden = states_count() > 0;
}
- (void)loadState:(id)sender {
    (void)sender;
    NSInteger r = self.table.clickedRow >= 0 && sender == self.table ? self.table.clickedRow : self.table.selectedRow;
    if (r < 0) return;
    [self.w makeFirstResponder:nil];          /* end a rename in progress */
    states_request_load((int)r);
    [NSApp stopModal];
}
- (void)deleteState:(id)sender {
    (void)sender;
    NSInteger r = self.table.selectedRow;
    if (r < 0) return;
    [self.w makeFirstResponder:nil];
    states_delete((int)r);
    [self.table reloadData];
    if (states_count() > 0) [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:MIN(r, states_count() - 1)] byExtendingSelection:NO];
    [self update];
}
- (void)close:(id)sender { (void)sender; [self.w makeFirstResponder:nil]; [NSApp stopModal]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; [NSApp stopModal]; }
- (void)dump:(NSString *)path {        /* debug (POP2_WINDOW_DUMP): the window as a PNG, then close */
    NSView *v = self.w.contentView;
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    [NSApp stopModal];
}
@end

static void states_window(void) {
    @autoreleasepool {
        PoPStates *c = [[PoPStates alloc] init];
        c.fmt = [[NSDateFormatter alloc] init];
        c.fmt.dateStyle = NSDateFormatterMediumStyle; c.fmt.timeStyle = NSDateFormatterShortStyle;
        c.fmt.doesRelativeDateFormatting = YES;
        const CGFloat wdt = 460, h = 600;
        c.w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, wdt, h)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered defer:NO];
        c.w.title = @"Save States";
        c.w.releasedWhenClosed = NO;
        c.w.delegate = c;
        NSView *cv = c.w.contentView;
        NSString *how = [NSString stringWithFormat:
            @"The last %d save states, newest first (%s saves, %s loads the newest). Click a name to rename it. "
             "Loading from the title screen starts a game first.", STATES_MAX,
            input_key_name(input_action_key(ACT_QSAVE)), input_key_name(input_action_key(ACT_QLOAD))];
        NSTextField *info = [NSTextField wrappingLabelWithString:how];
        info.font = [NSFont systemFontOfSize:11];
        info.frame = NSMakeRect(20, h - 52, wdt - 40, 40);
        [cv addSubview:info];
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 60, wdt - 40, h - 124)];
        sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
        c.table = [[NSTableView alloc] initWithFrame:sv.bounds];
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"state"];
        col.width = sv.contentSize.width;
        col.resizingMask = NSTableColumnAutoresizingMask;
        c.table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
        [c.table addTableColumn:col];
        c.table.headerView = nil;
        c.table.rowHeight = 80;
        c.table.dataSource = c; c.table.delegate = c;
        c.table.target = c; c.table.doubleAction = @selector(loadState:);
        sv.documentView = c.table;
        [cv addSubview:sv];
        c.empty = [NSTextField wrappingLabelWithString:@"No save states yet. Save during play with Extras > Quick Save or its key."];
        c.empty.alignment = NSTextAlignmentCenter; c.empty.textColor = NSColor.secondaryLabelColor;
        c.empty.frame = NSMakeRect(40, h / 2 - 20, wdt - 80, 40);
        [cv addSubview:c.empty];
        c.del = [NSButton buttonWithTitle:@"Delete" target:c action:@selector(deleteState:)];
        c.del.frame = NSMakeRect(20, 16, 100, 30);
        [cv addSubview:c.del];
        NSButton *done = [NSButton buttonWithTitle:@"Done" target:c action:@selector(close:)];
        done.frame = NSMakeRect(wdt - 230, 16, 100, 30);
        done.keyEquivalent = @"\033";
        [cv addSubview:done];
        c.load = [NSButton buttonWithTitle:@"Load" target:c action:@selector(loadState:)];
        c.load.frame = NSMakeRect(wdt - 120, 16, 100, 30);
        c.load.keyEquivalent = @"\r";
        [cv addSubview:c.load];
        [c.table reloadData];
        [c.table sizeLastColumnToFit];
        if (states_count() > 0) [c.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [c update];
        [c.w center];
        if (getenv("POP2_WINDOW_DUMP"))
            [c performSelector:@selector(dump:) withObject:@(getenv("POP2_WINDOW_DUMP")) afterDelay:0.5
                       inModes:@[NSModalPanelRunLoopMode]];
        [NSApp runModalForWindow:c.w];
        [c.w orderOut:nil];
        event_release_all();
    }
}
