//
//  IOConfigurationHandler.m
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

@implementation IOConfigurationHandler

- (instancetype)init {
    if (self = [super init]) {
        handlers = @{
            @"/ib/" : NSStringFromSelector(@selector(ioMinDbm:message:)),
            @"/it/" : NSStringFromSelector(@selector(ioMaxDbm:message:)),
            @"/ig/" : NSStringFromSelector(@selector(ioGain:message:)),
            @"/ic/" : NSStringFromSelector(@selector(ioGainDbm:message:)),
            @"/ip/" : NSStringFromSelector(@selector(phantomPower:message:)),
            @"/ia/" : NSStringFromSelector(@selector(autogain:message:)),

            @"/ob/" : NSStringFromSelector(@selector(ioMinDbm:message:)),
            @"/ot/" : NSStringFromSelector(@selector(ioMaxDbm:message:)),
            @"/og/" : NSStringFromSelector(@selector(ioGain:message:)),
            @"/oc/" : NSStringFromSelector(@selector(ioGainDbm:message:)),

            @"/im/" : NSStringFromSelector(@selector(mapping:message:)),
            @"/om/" : NSStringFromSelector(@selector(mapping:message:)),

            @"/in/" : NSStringFromSelector(@selector(ioName:message:)),
            @"/on/" : NSStringFromSelector(@selector(ioName:message:))
        } ;
    }
    
    return self;
}

- (void)ignore:(ESManager *)manager message:(F53OSCMessage *)message {
    // nothing to do here
}

- (void)ioMinDbm:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/ib/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;

    if ([device iocount:kind] <= idx) {
        return ;
    }

    NSNumber *val = message.arguments[0] ;
    
    [device io:idx kind:kind].minDbm = val.floatValue ;
}

- (void)ioMaxDbm:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/it/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;

    if ([device iocount:kind] <= idx) {
        return ;
    }

    NSNumber *val = message.arguments[0] ;
    
    [device io:idx kind:kind].maxDbm = val.floatValue ;
}

- (void)ioGain:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/ig/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;

    if ([device iocount:kind] <= idx) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    if (isnan(val.floatValue)) {
        val = [NSNumber numberWithFloat:0] ;
    }
    
    ESInputOutput *io = [device io:idx kind:kind] ;
    @synchronized (io) {
        io.gain = val.floatValue ;
        io.lastDeviceGainTime = [NSDate date] ;
    }
    [manager notifyGainDidChangeDevice:device io:io] ;
}

- (void)ioGainDbm:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/ic/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;

    if ([device iocount:kind] <= idx) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    
    ESInputOutput *io = [device io:idx kind:kind] ;
    io.gainDbm = val.floatValue ;
    [manager notifyGainDbmDidChangeDevice:device io:io] ;

    if (kind == kIOKindOutput && idx == device.outputs.count -1 && !device.configured) {
        [manager notifyDeviceDidFound:device] ;
    }
}

- (void)phantomPower:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;

    if ([device iocount:kIOKindInput] <= idx) {
        return ;
    }
    
    ESInputOutput *io = [device io:idx kind:kIOKindInput] ;
    NSNumber *val = message.arguments[0] ;
    io.phantomPower = (val.integerValue == 1) ;
    [manager notifyPhantomPowerDidChangeDevice:device io:io] ;
}

- (void)autogain:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;
    
    if ([device iocount:kIOKindInput] <= idx) {
        return ;
    }
    
    ESInputOutput *io = [device io:idx kind:kIOKindInput] ;
    NSNumber *val = message.arguments[0] ;
    io.autogain = !(val.integerValue == 0) ;
    [manager notifyPhantomPowerDidChangeDevice:device io:io] ;
}

- (void)mapping:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/im/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;
    
    if ([device iocount:kind] <= idx) {
        return ;
    }
    
    ESInputOutput *io = [device io:idx kind:kind] ;
    NSNumber *val = message.arguments[0] ;
    io.mapping = val.unsignedIntegerValue ;
    [manager notifyMappingDidChangeDevice:device io:io] ;
}

- (void)ioName:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/in/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;
    
    if ([device iocount:kind] <= idx) {
        return ;
    }
    
    NSString *title = message.arguments[0] ;
    ESInputOutput *io = [device io:idx kind:kind] ;
    if ([io.title isEqualToString:title]) {
        return ;
    }
    io.title = title ;
    
    if (kind == kIOKindInput && idx == device.inputs.count -1 && !device.configured) {
        if (device.outcount > 0) {
            F53OSCMessage *reply = [F53OSCMessage messageWithCommand:[ESCOutputConfigurationRequest command]] ;
            [manager sendMessage:reply toHost:device.ip onPort:device.port] ;
        } else {
            device.configured = YES ;
        }
    } else if (kind == kIOKindOutput && idx == device.outputs.count -1) {
        device.configured = YES ;
    }

    [manager notifyDeviceDidChangeIOName:device io:io] ;
}

@end
