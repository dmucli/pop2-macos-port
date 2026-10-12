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

int   wm_menu_count(void);
maddr wm_menu_handle(int i);
void  event_menu_command_key(uint8_t ch);
void  event_menu_click(void);
void  platform_toggle_fullscreen(void);
void  runtime_quit(void);

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

        NSMenu *win = [[NSMenu alloc] initWithTitle:@"View"];   /* not "Window": AppKit would add its own items */
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
