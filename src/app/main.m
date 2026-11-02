// SPDX-License-Identifier: GPL-3.0-or-later
// Menu bar app: pairs with and connects to a Wiimote, and sends its guitar to YARG.
#import <AppKit/AppKit.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "YargSetup.h"

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
- (void)pairingEvent:(pair_event)event message:(NSString *)message;
@end

static AppDelegate *delegate;

static void OnStateChange(void *ctx, int player) {
    (void)ctx, (void)player;
    dispatch_async(dispatch_get_main_queue(), ^{
        [delegate refresh];
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
        .connected = OnStateChange,
        .disconnected = OnStateChange,
        .extension_changed = OnExtensionChanged,
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
    NSMenuItem *_pairItem, *_connectItem, *_openYargItem, *_setupItem, *_removeItem, *_loginItem;
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
    if ([arguments containsObject:@"--add-to-yarg"]) {
        [self addGuitarTo:[YargSetup currentYarg]];
    } else if ([arguments containsObject:@"--remove-from-yarg"]) {
        [self removeFromYarg];
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
    _openYargItem = [self addActionTo:menu title:@"Open YARG" action:@selector(openYarg)];
    _setupItem = [self addActionTo:menu title:@"Add Guitar to YARG" action:@selector(chooseAndAddGuitar)];
    _removeItem = [self addActionTo:menu title:@"Remove from YARG" action:@selector(removeFromYarg)];
    _connectItem = [self addActionTo:menu title:@"Connect Another Wiimote" action:@selector(connectAnother)];
    _pairItem = [self addActionTo:menu title:@"Pair a New Wiimote" action:@selector(togglePairing)];
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
        NSString *state = status.extension == WM_EXT_GUITAR ? @"guitar ready"
                          : wm_ext_plays_as_guitar(status.extension)
                              ? [NSString stringWithFormat:@"%s ready, playing as a guitar", wm_ext_name(status.extension)]
                          : status.extension == WM_EXT_NONE ? @"nothing plugged in"
                                                            : [NSString stringWithFormat:@"%s isn't supported yet",
                                                                                         wm_ext_name(status.extension)];
        NSString *address = @(status.address);
        NSString *shortAddress = address.length >= 5 ? [address substringFromIndex:address.length - 5] : address;
        _wiimoteLines[i].title = [NSString stringWithFormat:@"Player %d: %@ (%@)", i + 1, state, shortAddress];
    }

    uint64_t reports = session_input_reports();
    _receiving = reports != _lastReports;
    _lastReports = reports;

    _waitingLine.hidden = connected && !_inputBlocked;
    _waitingLine.title = _inputBlocked ? @"Wiimote: blocked by macOS" : @"Wiimote: press a button on it to connect";

    if (!guitar || !_receiving) {
        _yargLine.title = @"YARG: waiting for the guitar";
    } else {
        _yargLine.title = session_yarg_listening() ? @"YARG: receiving the guitar" : @"YARG: not running with the add-on";
    }
    if (![_yargLine.title isEqualToString:_lastLoggedYargLine]) {
        _lastLoggedYargLine = _yargLine.title;
        [self log:_yargLine.title];
    }

    NSString *yarg = [YargSetup currentYarg];
    BOOL hasAddon = yarg && [YargSetup hasAddon:yarg];
    NSString *notice = _notice;
    BOOL addonCurrent = hasAddon && [YargSetup addonIsCurrent:yarg];
    if (!notice && !hasAddon) {
        notice = @"Choose Add Guitar to YARG to get started, or again after a YARG update";
    } else if (!notice && !addonCurrent) {
        notice = @"The add-on in YARG is out of date. Quit YARG and choose Update Add-on";
    }
    _noticeLine.title = notice ?: @"";
    _noticeLine.hidden = notice == nil;

    _openYargItem.enabled = !_settingUp && yarg != nil;
    _setupItem.enabled = !_settingUp;
    _setupItem.hidden = addonCurrent;
    _setupItem.title = hasAddon ? @"Update Add-on in YARG" : @"Add Guitar to YARG";
    _removeItem.enabled = !_settingUp;
    _removeItem.hidden = !hasAddon;
    _pairItem.title = _pairing ? @"Cancel Pairing" : @"Pair a New Wiimote";

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

- (void)connectAnother {
    int waiting = pair_connect_paired();
    [self log:[NSString stringWithFormat:@"Connecting from the Mac to %d paired Wiimotes", waiting]];
    NSString *notice = waiting > 0 ? @"Now press a button on the Wiimote you want to connect"
                                   : @"No other paired Wiimotes to connect";
    _notice = notice;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if ([self->_notice isEqualToString:notice]) {
            self->_notice = nil;
            [self refresh];
        }
    });
    [self refresh];
}

- (void)openYarg {
    NSString *path = [YargSetup currentYarg];
    if (path) {
        [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:path]
                                            configuration:[NSWorkspaceOpenConfiguration configuration]
                                        completionHandler:nil];
    }
}

- (void)chooseAndAddGuitar {
    NSString *found = [YargSetup currentYarg];
    if (found) {
        [self addGuitarTo:found];
        return;
    }
    [NSApp activateIgnoringOtherApps:YES];
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose your YARG app.";
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    panel.allowedContentTypes = @[ UTTypeApplicationBundle ];
    if ([panel runModal] == NSModalResponseOK) {
        [self addGuitarTo:panel.URL.path];
    }
}

- (void)addGuitarTo:(NSString *)yarg {
    if (!yarg) {
        [self log:@"Could not find YARG"];
        return;
    }
    _settingUp = YES;
    _notice = @"Adding the guitar to YARG...";
    [self log:[@"Adding to " stringByAppendingString:yarg]];
    [self refresh];
    [YargSetup addTo:yarg
          completion:^(NSString *error) {
        [self setupFinished:error action:@"Adding"];
    }];
}

- (void)removeFromYarg {
    NSString *yarg = [YargSetup currentYarg];
    if (!yarg) {
        return;
    }
    _settingUp = YES;
    [self refresh];
    [YargSetup removeFrom:yarg
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
