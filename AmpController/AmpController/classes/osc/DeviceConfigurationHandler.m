//
//  DeviceConfigurationHandler.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "CommandHandler.h"

#import "F53OSC.h"
#import "ESDevice.h"
#import "ESManager.h"
#import "F53OSCMessage+ESCommand.h"

@implementation DeviceConfigurationHandler

- (instancetype)init {
    if (self = [super init]) {
        handlers = @{
            @"/dn" : NSStringFromSelector(@selector(deviceName:message:)),
            @"/sa" : NSStringFromSelector(@selector(soundAddress:message:)),
            @"/ac" : NSStringFromSelector(@selector(clientAddress:message:)),
            @"/rv" : NSStringFromSelector(@selector(fwRevision:message:)),
            @"/dt" : NSStringFromSelector(@selector(deviceType:message:)),
            @"/sf" : NSStringFromSelector(@selector(syncFrequency:message:)),
            @"/sm" : NSStringFromSelector(@selector(syncMode:message:)),
            @"/am" : NSStringFromSelector(@selector(audioStreamMode:message:)),
            @"/ti" : NSStringFromSelector(@selector(totalIOs:message:)),
            @"/to" : NSStringFromSelector(@selector(totalIOs:message:))
        } ;
    }
    return self;
}

//- (void)handleMessage:(ESManager *)manager command:(ESCommand *)command message:(F53OSCMessage *)message {
//    NSLog(@"handleMessage %@", message.addressPattern) ;
//    [super handleMessage:manager command:command message:message] ;
//}

- (void)ignore:(ESManager *)manager message:(F53OSCMessage *)message {
    // nothing to do here
}

- (void)deviceName:(ESManager *)manager message:(F53OSCMessage *)message  {
    NSString *title = message.arguments[0] ;
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device || [title isEqualToString:device.title]) {
        return ;
    }

    device.title = title ;
    [manager notifyDeviceDidChangeName:device] ;
}

- (void)soundAddress:(ESManager *)manager message:(F53OSCMessage *)message  {
    NSString *ip = message.arguments[0] ;
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    device.soundAddress = ip ;
    for (ESDevice *oldDevice in manager.devices) {
        if ([oldDevice.soundAddress isEqual:device.soundAddress] && ![oldDevice.ip isEqual:device.ip]) {
            NSLog(@"DeviceConfigurationHandler.soundAddress Replace osc:%@ with osc:%@ for sa:%@", oldDevice.ip, device.ip, oldDevice.soundAddress) ;
            [manager replaceIp:device.ip forDevice:oldDevice] ;
            break ;
        }
    }
}

- (void)clientAddress:(ESManager *)manager message:(F53OSCMessage *)message  {
    NSString *ip = message.arguments[0] ;
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSString *oldIp = device.clientAddress ;
    device.clientAddress = ip ;
    
    if (![oldIp isEqualToString:ip]) {
        [manager notifyDeviceDidChangeClientAddress:device] ;
    }
}

- (void)deviceType:(ESManager *)manager message:(F53OSCMessage *)message  {
    NSNumber *val = message.arguments[0] ;
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (device) {
        if (val.integerValue == 1) {
            device.deviceType = @"IO24" ;
        } else if (val.integerValue == 2) {
            device.deviceType = @"IO88 old" ;
        } else if (val.integerValue == 3) {
            device.deviceType = @"IO88" ;
        } else if (val.integerValue == 4) {
            device.deviceType = @"IO24 v0" ;
        } else {
            device.deviceType = @"unknown" ;
        }
    }
}

- (void)syncFrequency:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    device.syncFrequency = val.integerValue ;
    [manager notifySyncFreqDidChangeDevice:device] ;
}

- (void)fwRevision:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    device.fwRevision = message.arguments[0] ;
}

- (void)audioStreamMode:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    if (val.integerValue == 0) {
        device.audioStreamMode = @"Not connected" ;
    } else {
        NSString *audioStreamMode = @"" ;
        if (val.integerValue & 0x1) {
            audioStreamMode = [audioStreamMode stringByAppendingString:@"TX"] ;
        }
        if (val.integerValue & 0x2) {
            audioStreamMode = [audioStreamMode stringByAppendingString:@" RX"] ;
        }
        if (val.integerValue & 0x4) {
            audioStreamMode = [audioStreamMode stringByAppendingString:@" Inet"] ;
        }
        if (val.integerValue & 0x8) {
            audioStreamMode = [audioStreamMode stringByAppendingString:@" MIDI"] ;
        }
        device.audioStreamMode = audioStreamMode ;
    }
}

- (void)syncMode:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    device.master = (val.intValue == 1) ;
}

- (void)totalIOs:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/ti"] ? kIOKindInput : kIOKindOutput) ;
    [device setTotalIO:kind total:val.integerValue] ;
    
    device.confTimeout = 0 ;

    if (kind == kIOKindOutput && !device.configured) {
        if (device.inpcount > 0) {
            F53OSCMessage *reply = [F53OSCMessage messageWithCommand:[ESCInputConfigurationRequest command]] ;
            [manager sendMessage:reply toHost:device.ip onPort:device.port] ;
        } else if (device.outcount > 0) {
            F53OSCMessage *reply = [F53OSCMessage messageWithCommand:[ESCOutputConfigurationRequest command]] ;
            [manager sendMessage:reply toHost:device.ip onPort:device.port] ;
        }
    }
}

@end
