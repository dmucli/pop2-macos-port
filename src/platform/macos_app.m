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
