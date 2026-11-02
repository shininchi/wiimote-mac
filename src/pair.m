// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pairing approach based on WiimotePair, Copyright 2024 Dolphin Emulator Project,
// licensed under GPL-2.0-or-later: https://github.com/dolphin-emu/WiimotePair
//
// Pairs a Wiimote with this Mac. Wiimotes need a PIN made of raw bytes, which the
// public IOBluetooth API can't send, so this uses the same private calls that
// WiimotePair relies on. They are checked before use, so a macOS update
// that removes them gives an error instead of a crash.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <mach/mach_error.h>

#include "pair.h"
#include "wiimote.h"

// 0x002594 is what an original Wiimote reports on macOS. The others are from its
// published Bluetooth class and the Wii Remote Plus's.
#define CONNECT_TIMEOUT_SECONDS 15
#define WIIMOTE_CLASS 0x002594
#define WIIMOTE_PUBLISHED_CLASS 0x002504
#define WIIMOTE_PLUS_CLASS 0x000508

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
    pair_status_fn _status;
    CBCentralManager *_central;
    IOBluetoothDeviceInquiry *_inquiry;
    IOBluetoothDevicePair *_pair;
    IOBluetoothDevice *_connecting;
}

- (instancetype)initWithStatus:(pair_status_fn)status {
    if ((self = [super init])) {
        _status = status;
    }
    return self;
}

- (void)start {
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
}

- (void)stop {
    _connecting = nil;
    _central.delegate = nil;
    _central = nil;
    _inquiry.delegate = nil;
    [_inquiry stop];
    _inquiry = nil;
}

- (void)report:(pair_event)event message:(NSString *)message {
    _status(event, message.UTF8String);
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
        [self report:PAIR_STOPPED message:@"This version of macOS no longer has the Bluetooth calls needed for pairing."];
        return;
    }
    _inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:self];
    _inquiry.searchType = kIOBluetoothDeviceSearchClassic;
    IOReturn result = [_inquiry start];
    if (result != kIOReturnSuccess) {
        [self report:PAIR_STOPPED
             message:[NSString stringWithFormat:@"Could not start the Bluetooth search: %s", mach_error_string(result)]];
        return;
    }
    [self report:PAIR_PROGRESS message:@"Searching. Press the red SYNC button under the Wiimote's battery cover."];
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    switch (central.state) {
    case CBManagerStatePoweredOn:
        [self startSearching];
        break;
    case CBManagerStatePoweredOff:
        [self report:PAIR_STOPPED message:@"Bluetooth is turned off. Turn it on to pair the Wiimote."];
        break;
    case CBManagerStateUnauthorized:
        [self report:PAIR_STOPPED
             message:@"macOS blocked Bluetooth access. Allow it in System Settings > Privacy & Security > Bluetooth, "
                     @"then try again."];
        break;
    case CBManagerStateUnsupported:
        [self report:PAIR_STOPPED message:@"This Mac reports that Bluetooth isn't supported."];
        break;
    default:
        break;
    }
}

- (void)considerDevice:(IOBluetoothDevice *)device {
    if (_pair || _connecting) {
        return;
    }
    [self report:PAIR_DETAIL
         message:[NSString stringWithFormat:@"Saw %@, class 0x%06X, paired: %d", device.name ?: @"(no name yet)",
                                            device.classOfDevice, device.isPaired]];
    // Names can arrive well after a device is found, or not at all while Bluetooth is
    // busy with another Wiimote, so the device class is enough to recognize one.
    if (![device.name containsString:@"Nintendo RVL-CNT-01"] && device.classOfDevice != WIIMOTE_CLASS &&
        device.classOfDevice != WIIMOTE_PUBLISHED_CLASS && device.classOfDevice != WIIMOTE_PLUS_CLASS) {
        return;
    }
    if (device.isPaired) {
        if (!device.isConnected) {
            [self connectToPairedDevice:device];
        }
        return;
    }

    [self report:PAIR_PROGRESS message:[NSString stringWithFormat:@"Found %@. Pairing...", device.name]];
    [_inquiry stop];
    _pair = [IOBluetoothDevicePair pairWithDevice:device];
    _pair.delegate = self;
    [_pair setUserDefinedPincode:YES];
    IOReturn result = [_pair start];
    if (result != kIOReturnSuccess) {
        [self report:PAIR_PROGRESS
             message:[NSString stringWithFormat:@"Could not start pairing: %s", mach_error_string(result)]];
        [self searchAgain];
    }
}

// A paired Wiimote in sync mode only needs connecting. Unlike a press of its other buttons,
// this also works when another Wiimote is already connected, because the Mac starts it.
- (void)connectToPairedDevice:(IOBluetoothDevice *)device {
    [self report:PAIR_PROGRESS message:[NSString stringWithFormat:@"Found %@. Connecting...", device.name ?: @"a Wiimote"]];
    [_inquiry stop];
    _connecting = device;
    IOReturn result = [device openConnection:self];
    if (result != kIOReturnSuccess) {
        [self connectionFailed:result];
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, CONNECT_TIMEOUT_SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (self->_connecting == device) {
            [self connectionFailed:kIOReturnTimeout];
        }
    });
}

- (void)connectionComplete:(IOBluetoothDevice *)device status:(IOReturn)status {
    if (device != _connecting) {
        return;
    }
    if (status != kIOReturnSuccess) {
        [self connectionFailed:status];
        return;
    }
    _connecting = nil;
    [self report:PAIR_PAIRED message:@"Connected!"];
}

- (void)connectionFailed:(IOReturn)error {
    _connecting = nil;
    [self report:PAIR_PROGRESS
         message:[NSString stringWithFormat:@"Could not connect: Bluetooth error 0x%02X", (unsigned)error]];
    [self searchAgain];
}

- (void)searchAgain {
    _pair = nil;
    [_inquiry clearFoundDevices];
    [_inquiry start];
    [self report:PAIR_PROGRESS message:@"Searching again. Press SYNC on the Wiimote."];
}

- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)sender device:(IOBluetoothDevice *)device {
    [self considerDevice:device];
}

- (void)deviceInquiryDeviceNameUpdated:(IOBluetoothDeviceInquiry *)sender
                                device:(IOBluetoothDevice *)device
                      devicesRemaining:(uint32_t)devicesRemaining {
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
        [self report:PAIR_PROGRESS message:@"Could not read this Mac's Bluetooth address."];
        return;
    }
    [self report:PAIR_DETAIL message:@"Sending the PIN"];
    uint64_t key = wm_sync_pairing_key(address.data);
    id coordinator = [(id)NSClassFromString(@"IOBluetoothCoreBluetoothCoordinator") sharedInstance];
    [coordinator pairPeer:[[pair device] classicPeer] forType:[pair currentPairingType] withKey:@(key)];
}

- (void)devicePairingFinished:(id)sender error:(IOReturn)error {
    if (error != kIOReturnSuccess) {
        [self report:PAIR_PROGRESS
             message:[NSString stringWithFormat:@"Pairing failed: Bluetooth error 0x%02X. Make sure you press the red SYNC "
                                                @"button, not 1 + 2.",
                                                (unsigned)error]];
        [_pair stop];
        [self searchAgain];
        return;
    }
    // Keep the pair object without stopping it: stopping disconnects the Wiimote,
    // which can abort the connection it is still setting up.
    [self report:PAIR_PAIRED message:@"Paired! Connecting to it now..."];
}

@end

static WiimotePairer *pairer;

void pair_start(pair_status_fn status) {
    [pairer stop];
    pairer = [[WiimotePairer alloc] initWithStatus:status];
    [pairer start];
}

void pair_stop(void) {
    [pairer stop];
    pairer = nil;
}
