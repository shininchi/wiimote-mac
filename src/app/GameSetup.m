// SPDX-License-Identifier: GPL-3.0-or-later
#import "GameSetup.h"

#import <AppKit/AppKit.h>

static NSString *SupportFolder(void) {
    NSURL *support = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                          inDomain:NSUserDomainMask
                                                 appropriateForURL:nil
                                                            create:YES
                                                             error:nil];
    return [support URLByAppendingPathComponent:@"wiimote-mac"].path;
}

static NSArray<NSString *> *ChangedFiles(void) {
    return @[ @"ScriptingAssemblies.json", @"RuntimeInitializeOnLoads.json" ];
}

static NSString *DataFolder(NSString *app) {
    return [app stringByAppendingPathComponent:@"Contents/Resources/Data"];
}

static NSDate *GameCodeDate(NSString *app) {
    NSString *code = [app stringByAppendingPathComponent:@"Contents/Resources/Data/Managed/Assembly-CSharp.dll"];
    return [NSFileManager.defaultManager attributesOfItemAtPath:code error:nil][NSFileModificationDate];
}

static NSString *FindYarg(void) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *support = [SupportFolder() stringByDeletingLastPathComponent];
    NSData *data =
        [NSData dataWithContentsOfFile:[support stringByAppendingPathComponent:@"in.yarg.launcher/settings.json"]];
    NSDictionary *settings = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *downloads = [settings isKindOfClass:NSDictionary.class] ? settings[@"downloadLocation"] : nil;
    if (![downloads isKindOfClass:NSString.class]) {
        return nil;
    }

    NSString *installs = [downloads stringByAppendingPathComponent:@"YARG Installs"];
    NSString *newest;
    NSDate *newestDate;
    for (NSString *name in [files contentsOfDirectoryAtPath:installs error:nil]) {
        NSString *app =
            [[installs stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"installation/YARG.app"];
        NSDate *date = GameCodeDate(app);
        if (date && (!newestDate || [date compare:newestDate] == NSOrderedDescending)) {
            newest = app;
            newestDate = date;
        }
    }
    return newest;
}

static NSString *UpdateJsonFile(NSString *path, BOOL (^change)(NSMutableDictionary *json)) {
    NSData *data = [NSData dataWithContentsOfFile:path];
    NSMutableDictionary *json = data ? [NSJSONSerialization JSONObjectWithData:data
                                                                       options:NSJSONReadingMutableContainers
                                                                         error:nil]
                                     : nil;
    if (![json isKindOfClass:NSDictionary.class] || !change(json)) {
        return [NSString stringWithFormat:@"This game's %@ looks different from what was expected.",
                                          path.lastPathComponent];
    }
    NSData *updated = [NSJSONSerialization dataWithJSONObject:json options:0 error:nil];
    if (![updated writeToFile:path atomically:YES]) {
        return [NSString stringWithFormat:@"Could not update %@.", path.lastPathComponent];
    }
    return nil;
}

@implementation GameSetup {
    NSString *_bundleId;
    NSString *_addonName;
    NSString *_addonNamespace;
    NSString *_addonClass;
    NSString *_recordName;
    NSString *_backupName;
    NSString *(*_find)(void);
}

+ (GameSetup *)yarg {
    static GameSetup *game;
    if (!game) {
        game = [[GameSetup alloc] init];
        game->_displayName = @"YARG";
        game->_bundleId = @"in.yarg.game";
        game->_addonName = @"WiimoteGuitarBridge";
        game->_addonNamespace = @"WiimoteGuitar";
        game->_addonClass = @"Bridge";
        game->_recordName = @"yarg-setup.plist";
        game->_backupName = @"backup";
        game->_find = FindYarg;
    }
    return game;
}

- (NSString *)recordPath {
    return [SupportFolder() stringByAppendingPathComponent:_recordName];
}

- (NSString *)backupFolder {
    return [SupportFolder() stringByAppendingPathComponent:_backupName];
}

- (NSString *)addonPathIn:(NSString *)app {
    return [[[DataFolder(app) stringByAppendingPathComponent:@"Managed"] stringByAppendingPathComponent:_addonName]
        stringByAppendingPathExtension:@"dll"];
}

- (BOOL)isRunning {
    return [NSRunningApplication runningApplicationsWithBundleIdentifier:_bundleId].count > 0;
}

- (NSString *)currentApp {
    NSString *found = _find();
    if (found) {
        return found;
    }
    NSString *chosen = [NSDictionary dictionaryWithContentsOfFile:[self recordPath]][@"app"];
    return chosen && GameCodeDate(chosen) ? chosen : nil;
}

- (BOOL)hasAddon:(NSString *)app {
    return [NSFileManager.defaultManager fileExistsAtPath:[self addonPathIn:app]];
}

- (BOOL)addonIsCurrent:(NSString *)app {
    NSString *addon = [NSBundle.mainBundle pathForResource:_addonName ofType:@"dll"];
    return addon && [NSFileManager.defaultManager contentsEqualAtPath:addon andPath:[self addonPathIn:app]];
}

- (NSString *)restoreBackupIn:(NSString *)app {
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *name in ChangedFiles()) {
        NSString *backup = [[self backupFolder] stringByAppendingPathComponent:name];
        NSString *target = [DataFolder(app) stringByAppendingPathComponent:name];
        NSError *error;
        if (![files fileExistsAtPath:backup]) {
            return @"The backup of the game's original files is missing.";
        }
        if (![files removeItemAtPath:target error:&error] || ![files copyItemAtPath:backup toPath:target error:&error]) {
            return error.localizedDescription;
        }
    }
    [files removeItemAtPath:[self addonPathIn:app] error:nil];
    return nil;
}

- (NSString *)backUpOriginalsIn:(NSString *)app {
    NSFileManager *files = NSFileManager.defaultManager;
    NSError *error;
    [files removeItemAtPath:[self backupFolder] error:nil];
    if (![files createDirectoryAtPath:[self backupFolder] withIntermediateDirectories:YES attributes:nil error:&error]) {
        return error.localizedDescription;
    }
    for (NSString *name in ChangedFiles()) {
        NSString *source = [DataFolder(app) stringByAppendingPathComponent:name];
        if (![files copyItemAtPath:source toPath:[[self backupFolder] stringByAppendingPathComponent:name] error:&error]) {
            return error.localizedDescription;
        }
    }
    return nil;
}

- (NSString *)registerAddonIn:(NSString *)app {
    NSString *dllName = [_addonName stringByAppendingPathExtension:@"dll"];
    NSString *problem = UpdateJsonFile([DataFolder(app) stringByAppendingPathComponent:@"ScriptingAssemblies.json"],
                                       ^BOOL(NSMutableDictionary *json) {
        NSMutableArray *names = json[@"names"], *types = json[@"types"];
        if (![names isKindOfClass:NSMutableArray.class] || ![types isKindOfClass:NSMutableArray.class]) {
            return NO;
        }
        [names addObject:dllName];
        [types addObject:@16];
        return YES;
    });
    if (problem) {
        return problem;
    }
    return UpdateJsonFile([DataFolder(app) stringByAppendingPathComponent:@"RuntimeInitializeOnLoads.json"],
                          ^BOOL(NSMutableDictionary *json) {
        NSMutableArray *root = json[@"root"];
        if (![root isKindOfClass:NSMutableArray.class]) {
            return NO;
        }
        [root addObject:@{
            @"assemblyName" : self->_addonName,
            @"nameSpace" : self->_addonNamespace,
            @"className" : self->_addonClass,
            @"methodName" : @"Initialize",
            @"loadTypes" : @0,
            @"isUnityClass" : @NO,
        }];
        return YES;
    });
}

- (NSString *)addAddonTo:(NSString *)app {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *addon = [NSBundle.mainBundle pathForResource:_addonName ofType:@"dll"];
    if (!addon) {
        return @"This app is missing its add-on file.";
    }
    if ([self isRunning]) {
        return [NSString stringWithFormat:@"Quit %@, then try again.", _displayName];
    }

    NSError *error;
    if ([files fileExistsAtPath:[self addonPathIn:app]]) {
        // Already there, so this just refreshes the add-on file.
        [files removeItemAtPath:[self addonPathIn:app] error:nil];
        return [files copyItemAtPath:addon toPath:[self addonPathIn:app] error:&error] ? nil : error.localizedDescription;
    }

    NSString *problem = [self backUpOriginalsIn:app];
    if (problem) {
        return problem;
    }
    if (![files copyItemAtPath:addon toPath:[self addonPathIn:app] error:&error]) {
        return error.localizedDescription;
    }
    problem = [self registerAddonIn:app];
    if (problem) {
        [self restoreBackupIn:app];
        return problem;
    }
    [@{@"app" : app} writeToFile:[self recordPath] atomically:YES];
    return nil;
}

- (void)addTo:(NSString *)app completion:(void (^)(NSString *_Nullable))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *problem = [self addAddonTo:app];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(problem);
        });
    });
}

- (void)removeFrom:(NSString *)app completion:(void (^)(NSString *_Nullable))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *problem = [self isRunning] ? [NSString stringWithFormat:@"Quit %@, then try again.", self->_displayName]
                                             : [self restoreBackupIn:app];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(problem);
        });
    });
}

@end
