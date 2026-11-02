// Pairs a Wiimote with this Mac. Wiimotes need a PIN made of raw bytes, which the
// public IOBluetooth API can't send, so this uses the same private calls that
// Dolphin's WiimotePair relies on. They are checked before use, so a macOS update
// that removes them gives an error instead of a crash.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <mach/mach_error.h>

#include "pair.h"
#include "wiimote.h"

@interface IOBluetoothDevice (WiimotePrivate)
- (id)classicPeer;
@end

@interface IOBluetoothDevicePair (WiimotePrivate)
- (void)setUserDefinedPincode:(BOOL)enabled;
- (NSUInteger)currentPairingType;
@end

@protocol WiimoteBluetoothCoordinator <NSObject>
+ (id)sharedInstance;
- (void)pairPeer:(id)peer forType:(NSUInteger)type withKey:(NSNumber *)key;
@end

@interface WiimotePairer : NSObject <CBCentralManagerDelegate, IOBluetoothDeviceInquiryDelegate,
                                     IOBluetoothDevicePairDelegate>
@end

@implementation WiimotePairer {
    CBCentralManager *_central;
    IOBluetoothDeviceInquiry *_inquiry;
    IOBluetoothDevicePair *_pair;
    BOOL _announcedAlreadyPaired;
}

- (void)start {
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
}

- (BOOL)privateCallsAvailable {
    Class coordinator = NSClassFromString(@"IOBluetoothCoreBluetoothCoordinator");
    return coordinator && [coordinator respondsToSelector:@selector(sharedInstance)] &&
           [coordinator instancesRespondToSelector:@selector(pairPeer:forType:withKey:)] &&
           [IOBluetoothDevice instancesRespondToSelector:@selector(classicPeer)] &&
           [IOBluetoothDevicePair instancesRespondToSelector:@selector(setUserDefinedPincode:)] &&
           [IOBluetoothDevicePair instancesRespondToSelector:@selector(currentPairingType)];
}

- (void)startSearching {
    if (_inquiry) {
        return;
    }
    if (![self privateCallsAvailable]) {
        printf("This version of macOS no longer has the Bluetooth calls needed for pairing.\n");
        return;
    }
    _inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:self];
    _inquiry.searchType = kIOBluetoothDeviceSearchClassic;
    IOReturn result = [_inquiry start];
    if (result != kIOReturnSuccess) {
        printf("Could not start the Bluetooth search: %s\n", mach_error_string(result));
        return;
    }
    printf("Searching. Press the red SYNC button under the Wiimote's battery cover.\n");
    fflush(stdout);
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    switch (central.state) {
    case CBManagerStatePoweredOn:
        [self startSearching];
        break;
    case CBManagerStatePoweredOff:
        printf("Bluetooth is turned off. Turn it on to pair the Wiimote.\n");
        break;
    case CBManagerStateUnauthorized:
        printf("macOS blocked Bluetooth access. Allow your terminal app in\n");
        printf("System Settings > Privacy & Security > Bluetooth, then run this again.\n");
        break;
    case CBManagerStateUnsupported:
        printf("This Mac reports that Bluetooth isn't supported.\n");
        break;
    default:
        break;
    }
    fflush(stdout);
}

- (void)considerDevice:(IOBluetoothDevice *)device {
    if (_pair) {
        return;
    }
    if (![device.name containsString:@"Nintendo RVL-CNT-01"]) {
        // The name may not have arrived yet, so allow this device to be found again.
        [_inquiry clearFoundDevices];
        return;
    }
    if (device.isPaired) {
        if (!_announcedAlreadyPaired) {
            _announcedAlreadyPaired = YES;
            printf("This Wiimote is already paired. Press a button on it to connect.\n");
            printf("If it won't connect, remove it in System Settings > Bluetooth and pair again.\n");
            fflush(stdout);
        }
        return;
    }

    printf("Found %s. Pairing...\n", device.name.UTF8String);
    fflush(stdout);
    [_inquiry stop];
    _pair = [IOBluetoothDevicePair pairWithDevice:device];
    _pair.delegate = self;
    [_pair setUserDefinedPincode:YES];
    IOReturn result = [_pair start];
    if (result != kIOReturnSuccess) {
        printf("Could not start pairing: %s\n", mach_error_string(result));
        [self searchAgain];
    }
}

- (void)searchAgain {
    _pair = nil;
    [_inquiry clearFoundDevices];
    [_inquiry start];
    printf("Searching again. Press SYNC on the Wiimote.\n");
    fflush(stdout);
}

- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)sender device:(IOBluetoothDevice *)device {
    [self considerDevice:device];
}

- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)sender error:(IOReturn)error aborted:(BOOL)aborted {
    if (!aborted) {
        [sender clearFoundDevices];
        [sender start];
    }
}

- (void)devicePairingPINCodeRequest:(id)sender {
    IOBluetoothDevicePair *pair = sender;
    NSString *hostAddress = [[IOBluetoothHostController defaultController] addressAsString];
    BluetoothDeviceAddress address;
    if (IOBluetoothNSStringToDeviceAddress(hostAddress, &address) != kIOReturnSuccess) {
        printf("Could not read this Mac's Bluetooth address.\n");
        return;
    }
    uint64_t key = wm_sync_pairing_key(address.data);
    id coordinator = [(id)NSClassFromString(@"IOBluetoothCoreBluetoothCoordinator") sharedInstance];
    [coordinator pairPeer:[[pair device] classicPeer] forType:[pair currentPairingType] withKey:@(key)];
}

- (void)devicePairingFinished:(id)sender error:(IOReturn)error {
    if (error != kIOReturnSuccess) {
        printf("Pairing failed: %s\n", mach_error_string(error));
        printf("Make sure you press the red SYNC button, not 1 + 2.\n");
        [_pair stop];
        [self searchAgain];
        return;
    }
    // Keep the pair object without stopping it: stopping disconnects the Wiimote,
    // which can abort the connection it is still setting up.
    printf("Paired! Connecting to it now...\n");
    fflush(stdout);
}

@end

static WiimotePairer *pairer;

void pair_start(void) {
    pairer = [[WiimotePairer alloc] init];
    [pairer start];
}
