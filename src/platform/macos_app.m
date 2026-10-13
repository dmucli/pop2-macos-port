/* macOS application glue: locating the user's own copy of the game.
 *
 * The app bundle contains no game data. On first launch it asks for the user's disk
 * image of the original game (an HFS image such as PoP2.hfv) and remembers its path. */
#import <Cocoa/Cocoa.h>
#include <string.h>
#include <sys/stat.h>

static NSString *const kDiskImageKey = @"DiskImagePath";

static int file_exists(NSString *p) {
    struct stat st;
    return p && stat(p.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode);
}

/* Returns the path of the disk image to use, or NULL if the user cancelled. */
const char *macos_locate_disk_image(int force_ask) {
    static char path[4096];
    @autoreleasepool {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        NSString *p = [d stringForKey:kDiskImageKey];
        if (!force_ask && file_exists(p)) {
            strlcpy(path, p.fileSystemRepresentation, sizeof path);
            return path;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Choose your Prince of Persia 2 disk image";
        panel.message = @"Prince of Persia 2 runs from your own copy of the original Macintosh game. "
                         "Choose its HFS disk image (for example PoP2.hfv).";
        panel.prompt = @"Use This Disk";
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK || !panel.URL) return NULL;
        p = panel.URL.path;
        [d setObject:p forKey:kDiskImageKey];
        [d synchronize];
        strlcpy(path, p.fileSystemRepresentation, sizeof path);
        return path;
    }
}

/* Error alert for problems before the game window exists. */
void macos_alert(const char *title, const char *text) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = [NSString stringWithUTF8String:title];
        a.informativeText = [NSString stringWithUTF8String:text];
        [a runModal];
    }
}

/* Directory for the per-user writable overlay (saved games, options). */
const char *macos_support_dir(void) {
    static char path[4096];
    @autoreleasepool {
        NSArray *a = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        NSString *p = [a.firstObject stringByAppendingPathComponent:@"PoP2-port"];
        strlcpy(path, p.fileSystemRepresentation, sizeof path);
    }
    return path;
}

/* True when launched as an app bundle (not from the build directory). */
int macos_in_app_bundle(void) {
    @autoreleasepool {
        return [[[NSBundle mainBundle] bundlePath] hasSuffix:@".app"];
    }
}

/* ---- Standard File dialogs (saved games live inside the emulated disk, not on the
 * host file system, so these are simple native pickers over that directory) ---- */
static NSString *macroman(const char *s) {
    NSString *r = [[NSString alloc] initWithBytes:s length:strlen(s) encoding:NSMacOSRomanStringEncoding];
    return r ? r : @"";
}

/* Open: choose one of names[0..n-1]. Returns the index, or -1 if cancelled. */
int macos_choose_file(const char *prompt, const char *const *names, int n) {
    @autoreleasepool {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = (prompt && *prompt) ? macroman(prompt) : @"Open Game";
        a.informativeText = n ? @"Choose a saved game." : @"There is no saved game yet.";
        [a addButtonWithTitle:n ? @"Open" : @"OK"];
        if (n) [a addButtonWithTitle:@"Cancel"];
        NSPopUpButton *pop = nil;
        if (n) {
            pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 26) pullsDown:NO];
            for (int i = 0; i < n; i++) [pop addItemWithTitle:[NSString stringWithFormat:@"%d. %@", i + 1, macroman(names[i])]];
            a.accessoryView = pop;
        }
        if ([a runModal] != NSAlertFirstButtonReturn || !n) return -1;
        return (int)pop.indexOfSelectedItem;
    }
}

/* Save: ask for a file name (HFS: at most 31 characters, no colon). Returns 1 with the
 * name in out (Mac Roman), 0 if cancelled. exists() asks whether a name is taken. */
int macos_ask_save_name(const char *prompt, const char *def, char *out, int outsz, int (*exists)(const char *)) {
    @autoreleasepool {
        for (;;) {
            NSAlert *a = [[NSAlert alloc] init];
            a.messageText = (prompt && *prompt) ? macroman(prompt) : @"Save Game";
            a.informativeText = @"Name of the saved game:";
            [a addButtonWithTitle:@"Save"];
            [a addButtonWithTitle:@"Cancel"];
            NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
            f.stringValue = macroman(def ? def : "");
            a.accessoryView = f;
            a.window.initialFirstResponder = f;
            if ([a runModal] != NSAlertFirstButtonReturn) return 0;
            NSString *s = [f.stringValue stringByReplacingOccurrencesOfString:@":" withString:@"-"];
            s = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if (!s.length) continue;
            NSData *d = [s dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES];
            int len = (int)MIN(d.length, (NSUInteger)MIN(31, outsz - 1));
            memcpy(out, d.bytes, (size_t)len); out[len] = 0;
            if (exists && exists(out)) {
                NSAlert *r = [[NSAlert alloc] init];
                r.messageText = [NSString stringWithFormat:@"“%@” already exists. Replace it?", macroman(out)];
                [r addButtonWithTitle:@"Replace"];
                [r addButtonWithTitle:@"Cancel"];
                if ([r runModal] != NSAlertFirstButtonReturn) continue;
            }
            return 1;
        }
    }
}

/* Dock icon from RGBA pixels (extracted from the user's copy of the game). */
void macos_set_app_icon(const uint8_t *rgba, int w, int h) {
    @autoreleasepool {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:w * 4 bitsPerPixel:32];
        memcpy(rep.bitmapData, rgba, (size_t)w * h * 4);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
        [img addRepresentation:rep];
        [NSApplication sharedApplication].applicationIconImage = img;
        const char *dump = getenv("POP2_ICON_DUMP");     /* debug: write the icon as PNG */
        if (dump) [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(dump) atomically:YES];
    }
}

/* ---- settings (NSUserDefaults) ---- */
int macos_pref_int(const char *key, int def) {
    @autoreleasepool {
        NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
        NSString *k = [NSString stringWithUTF8String:key];
        return [d objectForKey:k] ? (int)[d integerForKey:k] : def;
    }
}
void macos_pref_set_int(const char *key, int v) {
    @autoreleasepool { [[NSUserDefaults standardUserDefaults] setInteger:v forKey:[NSString stringWithUTF8String:key]]; }
}
void macos_beep(void) { NSBeep(); }

/* A game alert (ALRT/DITL) shown as a native alert: text, then buttons in DITL order
 * (the first one is the default). Returns the index of the button chosen. */
int macos_game_alert(int kind, const char *text, const char *const *buttons, int n) {
    @autoreleasepool {
        NSAlert *a = [[NSAlert alloc] init];
        a.alertStyle = kind == 0 ? NSAlertStyleCritical : NSAlertStyleWarning;
        a.messageText = @"Prince of Persia 2";
        a.informativeText = macroman(text);
        for (int i = 0; i < n; i++) [a addButtonWithTitle:macroman(buttons[i])];
        if (n == 0) [a addButtonWithTitle:@"OK"];
        NSModalResponse r = [a runModal];
        int i = (int)(r - NSAlertFirstButtonReturn);
        return i >= 0 && i < (n ? n : 1) ? i : 0;
    }
}
