// SPDX-License-Identifier: GPL-3.0-or-later
#import "YargSetup.h"

#import <AppKit/AppKit.h>

static NSString *const AddonName = @"WiimoteGuitarBridge";
static NSString *const YargBundleId = @"in.yarg.game";

static NSString *SupportFolder(void) {
    NSURL *support = [NSFileManager.defaultManager URLForDirectory:NSApplicationSupportDirectory
                                                          inDomain:NSUserDomainMask
                                                 appropriateForURL:nil
                                                            create:YES
                                                             error:nil];
    return [support URLByAppendingPathComponent:@"wiimote-mac"].path;
}

static NSString *RecordPath(void) {
    return [SupportFolder() stringByAppendingPathComponent:@"yarg-setup.plist"];
}

static NSString *BackupFolder(void) {
    return [SupportFolder() stringByAppendingPathComponent:@"backup"];
}

static NSString *DataFolder(NSString *yarg) {
    return [yarg stringByAppendingPathComponent:@"Contents/Resources/Data"];
}

static NSString *AddonPath(NSString *yarg) {
    return [[[DataFolder(yarg) stringByAppendingPathComponent:@"Managed"] stringByAppendingPathComponent:AddonName]
        stringByAppendingPathExtension:@"dll"];
}

static NSArray<NSString *> *ChangedFiles(void) {
    return @[ @"ScriptingAssemblies.json", @"RuntimeInitializeOnLoads.json" ];
}

static NSDate *GameCodeDate(NSString *yarg) {
    NSString *code = [yarg stringByAppendingPathComponent:@"Contents/Resources/Data/Managed/Assembly-CSharp.dll"];
    return [NSFileManager.defaultManager attributesOfItemAtPath:code error:nil][NSFileModificationDate];
}

static NSString *FindLauncherYarg(void) {
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
        return [NSString stringWithFormat:@"This YARG's %@ looks different from what was expected.",
                                          path.lastPathComponent];
    }
    NSData *updated = [NSJSONSerialization dataWithJSONObject:json options:0 error:nil];
    if (![updated writeToFile:path atomically:YES]) {
        return [NSString stringWithFormat:@"Could not update %@.", path.lastPathComponent];
    }
    return nil;
}

static NSString *RestoreBackup(NSString *yarg) {
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *name in ChangedFiles()) {
        NSString *backup = [BackupFolder() stringByAppendingPathComponent:name];
        NSString *target = [DataFolder(yarg) stringByAppendingPathComponent:name];
        NSError *error;
        if (![files fileExistsAtPath:backup]) {
            return @"The backup of YARG's original files is missing.";
        }
        if (![files removeItemAtPath:target error:&error] || ![files copyItemAtPath:backup toPath:target error:&error]) {
            return error.localizedDescription;
        }
    }
    [files removeItemAtPath:AddonPath(yarg) error:nil];
    return nil;
}

static NSString *BackUpOriginals(NSString *yarg) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSError *error;
    [files removeItemAtPath:BackupFolder() error:nil];
    if (![files createDirectoryAtPath:BackupFolder() withIntermediateDirectories:YES attributes:nil error:&error]) {
        return error.localizedDescription;
    }
    for (NSString *name in ChangedFiles()) {
        NSString *source = [DataFolder(yarg) stringByAppendingPathComponent:name];
        if (![files copyItemAtPath:source toPath:[BackupFolder() stringByAppendingPathComponent:name] error:&error]) {
            return error.localizedDescription;
        }
    }
    return nil;
}

static NSString *RegisterAddon(NSString *yarg) {
    NSString *dllName = [AddonName stringByAppendingPathExtension:@"dll"];
    NSString *problem = UpdateJsonFile([DataFolder(yarg) stringByAppendingPathComponent:@"ScriptingAssemblies.json"],
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
    return UpdateJsonFile([DataFolder(yarg) stringByAppendingPathComponent:@"RuntimeInitializeOnLoads.json"],
                          ^BOOL(NSMutableDictionary *json) {
        NSMutableArray *root = json[@"root"];
        if (![root isKindOfClass:NSMutableArray.class]) {
            return NO;
        }
        [root addObject:@{
            @"assemblyName" : AddonName,
            @"nameSpace" : @"WiimoteGuitar",
            @"className" : @"Bridge",
            @"methodName" : @"Initialize",
            @"loadTypes" : @0,
            @"isUnityClass" : @NO,
        }];
        return YES;
    });
}

static BOOL YargIsRunning(void) {
    return [NSRunningApplication runningApplicationsWithBundleIdentifier:YargBundleId].count > 0;
}

static NSString *Add(NSString *yarg) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *addon = [NSBundle.mainBundle pathForResource:AddonName ofType:@"dll"];
    if (!addon) {
        return @"This app is missing its add-on file.";
    }
    if (YargIsRunning()) {
        return @"Quit YARG, then try again.";
    }

    NSError *error;
    if ([files fileExistsAtPath:AddonPath(yarg)]) {
        // Already there, so this just refreshes the add-on file.
        [files removeItemAtPath:AddonPath(yarg) error:nil];
        return [files copyItemAtPath:addon toPath:AddonPath(yarg) error:&error] ? nil : error.localizedDescription;
    }

    NSString *problem = BackUpOriginals(yarg);
    if (problem) {
        return problem;
    }
    if (![files copyItemAtPath:addon toPath:AddonPath(yarg) error:&error]) {
        return error.localizedDescription;
    }
    problem = RegisterAddon(yarg);
    if (problem) {
        RestoreBackup(yarg);
        return problem;
    }
    [@{@"yarg" : yarg} writeToFile:RecordPath() atomically:YES];
    return nil;
}

@implementation YargSetup

+ (NSString *)currentYarg {
    NSString *found = FindLauncherYarg();
    if (found) {
        return found;
    }
    NSString *chosen = [NSDictionary dictionaryWithContentsOfFile:RecordPath()][@"yarg"];
    return chosen && GameCodeDate(chosen) ? chosen : nil;
}

+ (BOOL)hasAddon:(NSString *)yarg {
    return [NSFileManager.defaultManager fileExistsAtPath:AddonPath(yarg)];
}

+ (void)addTo:(NSString *)yarg completion:(void (^)(NSString *_Nullable))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *problem = Add(yarg);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(problem);
        });
    });
}

+ (void)removeFrom:(NSString *)yarg completion:(void (^)(NSString *_Nullable))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *problem = YargIsRunning() ? @"Quit YARG, then try again." : RestoreBackup(yarg);
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(problem);
        });
    });
}

@end
