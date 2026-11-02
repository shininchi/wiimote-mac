// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Adds the guitar add-on to a game, keeping a backup of what it changes so that it can be
// removed again. No game code is changed.
@interface GameSetup : NSObject

@property(class, readonly) GameSetup *yarg;

@property(readonly) NSString *displayName;

// Where the game is installed: the expected place, otherwise the one the user chose earlier.
// Nil if it can't be found.
- (nullable NSString *)currentApp;

// Whether this copy of the game has the add-on. A game update can remove it.
- (BOOL)hasAddon:(NSString *)app;

// Whether its add-on is the one included with this version of Wiimote Mac.
- (BOOL)addonIsCurrent:(NSString *)app;

// These run in the background and call back on the main thread, with nil on success.
- (void)addTo:(NSString *)app completion:(void (^)(NSString *_Nullable error))completion;
- (void)removeFrom:(NSString *)app completion:(void (^)(NSString *_Nullable error))completion;

@end

NS_ASSUME_NONNULL_END
