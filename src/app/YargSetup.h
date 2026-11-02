#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Adds the guitar add-on to the user's YARG, keeping a backup of what it changes so
// that it can be removed again. No game code is changed.
@interface YargSetup : NSObject

// The YARG to use: the newest one from the YARC Launcher, otherwise the one the user
// chose earlier. Nil if there isn't one.
+ (nullable NSString *)currentYarg;

// Whether this YARG has the add-on. A YARG update removes it.
+ (BOOL)hasAddon:(NSString *)yarg;

// These run in the background and call back on the main thread, with nil on success.
+ (void)addTo:(NSString *)yarg completion:(void (^)(NSString *_Nullable error))completion;
+ (void)removeFrom:(NSString *)yarg completion:(void (^)(NSString *_Nullable error))completion;

@end

NS_ASSUME_NONNULL_END
