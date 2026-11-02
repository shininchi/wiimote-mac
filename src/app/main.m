// SPDX-License-Identifier: GPL-3.0-or-later
// Menu bar app: pairs with and connects to a Wiimote, and sends its guitar to YARG.
#import <AppKit/AppKit.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "GameSetup.h"

#include "../pair.h"
#include "../session.h"

static void WriteLog(NSString *message) {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/wiimote-mac.log"];
    NSDateFormatter *format = [[NSDateFormatter alloc] init];
    format.dateFormat = @"HH:mm:ss.SSS";
    NSString *line = [NSString stringWithFormat:@"%@ %@\n", [format stringFromDate:NSDate.date], message];
    NSFileHandle *file = [NSFileHandle fileHandleForWritingAtPath:path];
    if (!file) {
        [line writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil];
        return;
    }
    [file seekToEndOfFile];
    [file writeData:[line dataUsingEncoding:NSUTF8StringEncoding]];
    [file closeFile];
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
- (void)refresh;
- (void)log:(NSString *)message;
- (void)inputAccessBlocked;
- (void)wiimoteConnected;
- (void)pairingEvent:(pair_event)event message:(NSString *)message;
@end

static AppDelegate *delegate;

static void OnStateChange(void *ctx, int player) {
    (void)ctx, (void)player;
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate refresh];
    });
}

static void OnConnected(void *ctx, int player) {
    (void)ctx, (void)player;
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate wiimoteConnected];
    });
}

// Logs what the motion sensor read when tilt changed, to help diagnose false triggers.
static void OnInput(void *ctx, int player, const wiimote *wm, bool tilted) {
    (void)ctx;
    static bool last_tilted[SESSION_MAX_WIIMOTES];
    if (tilted == last_tilted[player]) {
        return;
    }
    last_tilted[player] = tilted;
    NSString *message = [NSString stringWithFormat:@"Player %d tilt %@, motion %u %u %u", player + 1, tilted ? @"on" : @"off",
                                                   wm->accel_x, wm->accel_y, wm->accel_z];
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate log:message];
    });
}

static void OnExtensionChanged(void *ctx, int player, const wiimote *wm) {
    (void)ctx;
    NSString *name = @(wm_ext_name(wm->ext));
    int number = player + 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate log:[NSString stringWithFormat:@"Plugged into Wiimote %d: %@", number, name]];
        [delegate refresh];
    });
}

static void OnProblem(void *ctx, const char *message) {
    (void)ctx;
    NSString *text = @(message);
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate log:text];
    });
}

static void OnPairingStatus(pair_event event, const char *message) {
    [delegate pairingEvent:event message:@(message)];
}

// The Wiimote gets its own high-priority thread, so that nothing the interface does,
// such as keeping a menu open, can delay the guitar.
static void RunSession(void) {
    session_callbacks callbacks = {
        .connected = OnConnected,
        .disconnected = OnStateChange,
        .extension_changed = OnExtensionChanged,
        .input = OnInput,
        .problem = OnProblem,
    };
    IOReturn started = session_start(&callbacks, true, SESSION_MAX_WIIMOTES, CFRunLoopGetCurrent());
    if (started != kIOReturnSuccess) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate log:[NSString stringWithFormat:@"Could not listen for devices: 0x%08X", started]];
            [delegate inputAccessBlocked];
        });
        return;
    }
    CFRunLoopRun();
}

@implementation AppDelegate {
    NSStatusItem *_statusItem;
    NSMenuItem *_wiimoteLines[SESSION_MAX_WIIMOTES];
    NSMenuItem *_waitingLine, *_yargLine, *_noticeLine;
    NSMenuItem *_pairItem, *_loginItem;
    NSArray<GameSetup *> *_games;
    NSMapTable<GameSetup *, NSMenuItem *> *_openItems, *_setupItems, *_removeItems;
    NSString *_notice, *_lastLoggedYargLine;
    BOOL _pairing, _settingUp, _inputBlocked;
    uint64_t _lastReports;
    BOOL _receiving;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    delegate = self;
    [self buildMenu];
    [self log:@"Started"];

    NSThread *thread = [[NSThread alloc] initWithBlock:^{
        RunSession();
    }];
    thread.qualityOfService = NSQualityOfServiceUserInteractive;
    thread.name = @"Wiimote";
    [thread start];

    NSTimer *timer = [NSTimer timerWithTimeInterval:1.0
                                            repeats:YES
                                              block:^(__unused NSTimer *t) {
        [self refresh];
    }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    [self refresh];

    // For testing setup without the menu.
    NSArray *arguments = NSProcessInfo.processInfo.arguments;
    for (GameSetup *game in _games) {
        NSString *flagName = [[game.displayName lowercaseString] stringByReplacingOccurrencesOfString:@" " withString:@""];
        NSUInteger add = [arguments indexOfObject:[@"--add-to-" stringByAppendingString:flagName]];
        NSUInteger remove = [arguments indexOfObject:[@"--remove-from-" stringByAppendingString:flagName]];
        NSUInteger flag = add != NSNotFound ? add : remove;
        if (flag == NSNotFound) {
            continue;
        }
        NSString *app = flag + 1 < arguments.count ? arguments[flag + 1] : [game currentApp];
        if (add != NSNotFound) {
            [self addGuitarToGame:game app:app];
        } else {
            [self removeFromGame:game app:app];
        }
    }
}

- (NSMenuItem *)addInfoLineTo:(NSMenu *)menu {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    item.enabled = NO;
    [menu addItem:item];
    return item;
}

- (NSMenuItem *)addActionTo:(NSMenu *)menu title:(NSString *)title action:(SEL)action {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    [menu addItem:item];
    return item;
}

- (void)buildMenu {
    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    NSMenu *menu = [[NSMenu alloc] init];
    menu.autoenablesItems = NO;
    menu.delegate = self;

    _waitingLine = [self addInfoLineTo:menu];
    for (int i = 0; i < SESSION_MAX_WIIMOTES; i++) {
        _wiimoteLines[i] = [self addInfoLineTo:menu];
    }
    _yargLine = [self addInfoLineTo:menu];
    _noticeLine = [self addInfoLineTo:menu];
    [menu addItem:NSMenuItem.separatorItem];
    _games = @[ GameSetup.yarg ];
    _openItems = [NSMapTable strongToStrongObjectsMapTable];
    _setupItems = [NSMapTable strongToStrongObjectsMapTable];
    _removeItems = [NSMapTable strongToStrongObjectsMapTable];
    for (GameSetup *game in _games) {
        NSString *name = game.displayName;
        NSMenuItem *open = [self addActionTo:menu title:[@"Open " stringByAppendingString:name] action:@selector(openGame:)];
        NSMenuItem *setup = [self addActionTo:menu title:@"" action:@selector(chooseAndAddGuitar:)];
        NSMenuItem *remove = [self addActionTo:menu
                                         title:[@"Remove from " stringByAppendingString:name]
                                        action:@selector(removeFromGameItem:)];
        open.representedObject = setup.representedObject = remove.representedObject = game;
        [_openItems setObject:open forKey:game];
        [_setupItems setObject:setup forKey:game];
        [_removeItems setObject:remove forKey:game];
        [menu addItem:NSMenuItem.separatorItem];
    }
    _pairItem = [self addActionTo:menu title:@"Add a Wiimote" action:@selector(togglePairing)];
    [menu addItem:NSMenuItem.separatorItem];
    _loginItem = [self addActionTo:menu title:@"Start at Login" action:@selector(toggleLogin)];
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"Quit Wiimote Mac" action:@selector(terminate:) keyEquivalent:@"q"];
    [menu addItem:quit];
    _statusItem.menu = menu;
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [self refresh];
}

- (void)log:(NSString *)message {
    WriteLog(message);
}

- (void)refresh {
    BOOL connected = session_any_connected();
    BOOL guitar = NO;
    for (int i = 0; i < SESSION_MAX_WIIMOTES; i++) {
        session_wiimote status = session_status(i);
        _wiimoteLines[i].hidden = !status.connected;
        if (!status.connected) {
            continue;
        }
        guitar = guitar || wm_ext_plays_as_guitar(status.extension);
        NSString *state = status.extension == WM_EXT_GUITAR ? @"Guitar ready"
                          : wm_ext_plays_as_guitar(status.extension)
                              ? [NSString stringWithFormat:@"%s ready", wm_ext_name(status.extension)]
                          : status.extension == WM_EXT_NONE ? @"Plug in a guitar"
                                                            : [NSString stringWithFormat:@"%s isn't supported yet",
                                                                                         wm_ext_name(status.extension)];
        NSString *address = @(status.address);
        NSString *shortAddress = address.length >= 5 ? [address substringFromIndex:address.length - 5] : address;
        _wiimoteLines[i].title = [NSString stringWithFormat:@"Player %d: %@", i + 1, state];
        _wiimoteLines[i].toolTip = [NSString stringWithFormat:@"Shown in YARG as Wii Guitar %@", shortAddress];
    }

    uint64_t reports = session_input_reports();
    _receiving = reports != _lastReports;
    _lastReports = reports;

    _waitingLine.hidden = connected && !_inputBlocked;
    _waitingLine.title = _inputBlocked ? @"Wiimote: blocked by macOS" : @"Press a button on your Wiimote to connect it";

    if (!guitar || !_receiving) {
        _yargLine.title = @"Game: waiting for a guitar";
    } else {
        _yargLine.title = session_yarg_listening() ? @"Game: receiving the guitar" : @"Game: not open, or it doesn't have the add-on";
    }
    if (![_yargLine.title isEqualToString:_lastLoggedYargLine]) {
        _lastLoggedYargLine = _yargLine.title;
        [self log:_yargLine.title];
    }

    NSString *notice = _notice;
    BOOL anyAddon = NO;
    for (GameSetup *game in _games) {
        NSString *app = [game currentApp];
        BOOL hasAddon = app && [game hasAddon:app];
        BOOL current = hasAddon && [game addonIsCurrent:app];
        anyAddon = anyAddon || hasAddon;
        if (!notice && hasAddon && !current) {
            notice = [NSString stringWithFormat:@"The add-on in %@ is out of date", game.displayName];
        }
        NSString *name = game.displayName;
        NSMenuItem *setup = [_setupItems objectForKey:game], *remove = [_removeItems objectForKey:game];
        [_openItems objectForKey:game].enabled = !_settingUp && app != nil;
        setup.enabled = !_settingUp;
        setup.hidden = current;
        setup.title = [NSString stringWithFormat:hasAddon ? @"Update Add-on in %@" : @"Add Guitar to %@", name];
        remove.enabled = !_settingUp;
        remove.hidden = !hasAddon;
    }
    if (!notice && !anyAddon) {
        notice = @"Add the guitar to a game to get started";
    }
    _noticeLine.title = notice ?: @"";
    _noticeLine.hidden = notice == nil;

    _pairItem.title = _pairing ? @"Cancel" : @"Add a Wiimote";

    if (@available(macOS 13.0, *)) {
        _loginItem.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled ? NSControlStateValueOn
                                                                                            : NSControlStateValueOff;
    } else {
        _loginItem.hidden = YES;
    }

    NSString *symbol = guitar ? @"guitars.fill" : @"guitars";
    NSImage *image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:@"Wiimote Mac"];
    _statusItem.button.image = image;
    _statusItem.button.appearsDisabled = !connected;
    if (_pairing) {
        _statusItem.button.title = @" Press SYNC";
    } else {
        _statusItem.button.title = image ? @"" : @"Wii";
    }
}

// A Wiimote can connect from its own button while the app is waiting for SYNC, which also
// answers the request, so there's nothing left to wait for.
- (void)wiimoteConnected {
    if (_pairing) {
        [self pairingEvent:PAIR_PAIRED message:@"Connected!"];
    }
    [self refresh];
}

- (void)inputAccessBlocked {
    _inputBlocked = YES;
    _notice = @"Allow this app under Privacy & Security > Input Monitoring";
    IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    [self refresh];
}

- (void)pairingEvent:(pair_event)event message:(NSString *)message {
    [self log:message];
    if (event == PAIR_DETAIL) {
        return;
    }
    _notice = message;
    if (event != PAIR_PROGRESS) {
        _pairing = NO;
        pair_stop();
        NSString *finished = message;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if ([self->_notice isEqualToString:finished]) {
                self->_notice = nil;
                [self refresh];
            }
        });
    }
    [self refresh];
}

- (void)togglePairing {
    if (_pairing) {
        pair_stop();
        _pairing = NO;
        _notice = nil;
        [self log:@"Pairing cancelled"];
    } else {
        _pairing = YES;
        _notice = @"Starting Bluetooth...";
        pair_start(OnPairingStatus);
    }
    [self refresh];
}

- (void)openGame:(NSMenuItem *)item {
    NSString *app = [(GameSetup *)item.representedObject currentApp];
    if (app) {
        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:app]
                                            configuration:[NSWorkspaceOpenConfiguration configuration]
                                        completionHandler:nil];
    }
}

- (void)chooseAndAddGuitar:(NSMenuItem *)item {
    GameSetup *game = item.representedObject;
    NSString *found = [game currentApp];
    if (found) {
        [self addGuitarToGame:game app:found];
        return;
    }
    [NSApp activateIgnoringOtherApps:YES];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = [NSString stringWithFormat:@"Choose your %@ app.", game.displayName];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    panel.allowedContentTypes = @[ UTTypeApplicationBundle ];
    if ([panel runModal] == NSModalResponseOK) {
        [self addGuitarToGame:game app:panel.URL.path];
    }
}

- (void)addGuitarToGame:(GameSetup *)game app:(NSString *)app {
    if (!app) {
        [self log:[@"Could not find " stringByAppendingString:game.displayName]];
        return;
    }
    _settingUp = YES;
    _notice = [NSString stringWithFormat:@"Adding the guitar to %@...", game.displayName];
    [self log:[@"Adding to " stringByAppendingString:app]];
    [self refresh];
    [game addTo:app
        completion:^(NSString *error) {
        [self setupFinished:error action:@"Adding"];
    }];
}

- (void)removeFromGameItem:(NSMenuItem *)item {
    GameSetup *game = item.representedObject;
    [self removeFromGame:game app:[game currentApp]];
}

- (void)removeFromGame:(GameSetup *)game app:(NSString *)app {
    if (!app) {
        return;
    }
    _settingUp = YES;
    [self log:[@"Removing from " stringByAppendingString:app]];
    [self refresh];
    [game removeFrom:app
          completion:^(NSString *error) {
        [self setupFinished:error action:@"Removing"];
    }];
}

- (void)setupFinished:(NSString *)error action:(NSString *)action {
    _settingUp = NO;
    _notice = error;
    [self log:error ? [NSString stringWithFormat:@"%@ failed: %@", action, error]
                    : [action stringByAppendingString:@" finished"]];
    [self refresh];
}

- (void)toggleLogin {
    if (@available(macOS 13.0, *)) {
        SMAppService *service = SMAppService.mainAppService;
        NSError *error;
        BOOL worked = service.status == SMAppServiceStatusEnabled ? [service unregisterAndReturnError:&error]
                                                                  : [service registerAndReturnError:&error];
        if (!worked) {
            _notice = [@"Could not change Start at Login: " stringByAppendingString:error.localizedDescription];
            [self log:_notice];
        }
    }
    [self refresh];
}

@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        AppDelegate *appDelegate = [[AppDelegate alloc] init];
        app.delegate = appDelegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
