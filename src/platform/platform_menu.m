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
#include "platform.h"

int   wm_menu_count(void);
maddr wm_menu_handle(int i);
void  event_menu_command_key(uint8_t ch);
void  event_menu_click(void);
void  platform_toggle_fullscreen(void);
void  runtime_quit(void);
void  event_release_all(void);
static void controls_window(void);

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
- (void)quickSave:(id)sender { (void)sender; if (mods_playing()) mods_quick_save(0); else NSBeep(); }
- (void)quickLoad:(id)sender { (void)sender; if (mods_playing() && mods_slot_used(0)) mods_quick_load(0); else NSBeep(); }
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
        [ex addItem:[NSMenuItem separatorItem]];
        NSMenuItem *qs = add(ex, @"Quick Save (F5)", @selector(quickSave:), @"s", 1);
        qs.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        NSMenuItem *ql = add(ex, @"Quick Load (F9)", @selector(quickLoad:), @"l", 1);
        ql.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        NSMenuItem *rw = add(ex, mod_titles[MOD_REWIND], @selector(toggleMod:), nil, 1);
        rw.tag = MOD_REWIND; rw.state = mods_get(MOD_REWIND) ? NSControlStateValueOn : NSControlStateValueOff;
        [ex addItem:[NSMenuItem separatorItem]];
        NSMenuItem *cp = add(ex, mod_titles[MOD_SKIP_COPYPROT], @selector(toggleMod:), nil, 1);
        cp.tag = MOD_SKIP_COPYPROT; cp.state = mods_get(MOD_SKIP_COPYPROT) ? NSControlStateValueOn : NSControlStateValueOff;
        add(ex, @"Controls\u2026", @selector(controls:), nil, 1);
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

/* ---- Extras > Controls...: one row per action, click a button then press a key ---- */
@interface PoPKeyCatcher : NSView
@property int action;            /* the action waiting for a key, -1 = none */
@property(copy) void (^done)(int keycode);
@end
@implementation PoPKeyCatcher
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e {
    if (self.action < 0) { [super keyDown:e]; return; }
    int k = e.keyCode;
    if (k == 0x35) k = -2;       /* Esc: cancel */
    self.done(k);
}
@end

@interface PoPControls : NSObject <NSWindowDelegate>
@property(strong) NSWindow *w;
@property(strong) NSMutableArray<NSButton *> *buttons;
@property(strong) PoPKeyCatcher *catcher;
@end
@implementation PoPControls
- (void)refresh {
    for (int a = 0; a < ACT_COUNT; a++)
        self.buttons[a].title = [NSString stringWithUTF8String:input_key_name(input_action_key(a))];
}
- (void)pick:(NSButton *)b {
    int a = (int)b.tag;
    for (NSButton *o in self.buttons) o.enabled = NO;
    b.title = @"Press a key\u2026";
    self.catcher.action = a;
    __weak PoPControls *me = self;
    self.catcher.done = ^(int k) {
        if (k != -2) input_set_action_key(a, k);
        me.catcher.action = -1;
        for (NSButton *o in me.buttons) o.enabled = YES;
        [me refresh];
    };
    [self.w makeFirstResponder:self.catcher];
}
- (void)defaults:(id)sender {
    (void)sender;
    for (int a = 0; a < ACT_COUNT; a++) input_set_action_key(a, -1);
    for (int a = 0; a < ACT_COUNT; a++) input_set_action_key(a, input_action_default_key(a));
    [self refresh];
}
- (void)close:(id)sender { (void)sender; [NSApp stopModal]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; [NSApp stopModal]; }
@end

static void controls_window(void) {
    @autoreleasepool {
        PoPControls *c = [[PoPControls alloc] init];
        const CGFloat rowh = 30, top = 70, wdt = 470;
        CGFloat h = top + ACT_COUNT * rowh + 60;
        c.w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, wdt, h)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered defer:NO];
        c.w.title = @"Controls";
        c.w.releasedWhenClosed = NO;
        c.w.delegate = c;
        c.catcher = [[PoPKeyCatcher alloc] initWithFrame:NSMakeRect(0, 0, wdt, h)];
        c.catcher.action = -1;
        c.w.contentView = c.catcher;
        NSTextField *info = [NSTextField wrappingLabelWithString:
            @"Click an action, then press its new key (Esc cancels). The default keys keep working, "
             "and Shift and Control stay the action and second buttons. "
             "Game controller: d-pad or stick, A action, B second button, Start pause, "
             "left shoulder rewind, right shoulder / Y quick save / load."];
        info.frame = NSMakeRect(20, h - top + 4, wdt - 40, top - 14);
        info.font = [NSFont systemFontOfSize:11];
        [c.catcher addSubview:info];
        c.buttons = [NSMutableArray array];
        for (int a = 0; a < ACT_COUNT; a++) {
            CGFloat y = h - top - (a + 1) * rowh;
            NSTextField *l = [NSTextField labelWithString:[NSString stringWithUTF8String:input_action_name(a)]];
            l.frame = NSMakeRect(20, y + 4, 260, 20);
            [c.catcher addSubview:l];
            NSButton *b = [NSButton buttonWithTitle:@"" target:c action:@selector(pick:)];
            b.frame = NSMakeRect(290, y, 160, 28);
            b.tag = a;
            [c.catcher addSubview:b];
            [c.buttons addObject:b];
        }
        NSButton *def = [NSButton buttonWithTitle:@"Restore Defaults" target:c action:@selector(defaults:)];
        def.frame = NSMakeRect(20, 14, 150, 30);
        [c.catcher addSubview:def];
        NSButton *ok = [NSButton buttonWithTitle:@"Done" target:c action:@selector(close:)];
        ok.frame = NSMakeRect(wdt - 120, 14, 100, 30);
        ok.keyEquivalent = @"\r";
        [c.catcher addSubview:ok];
        [c refresh];
        [c.w center];
        [NSApp runModalForWindow:c.w];
        [c.w orderOut:nil];
        event_release_all();
    }
}
