//
//  MeterHandler.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "CommandHandler.h"

#import "F53OSC.h"
#import "ESDevice.h"
#import "ESManager.h"

@implementation MeterHandler

- (instancetype)init {
    if (self = [super init]) {
        handlers = @{
            @"/il/" : NSStringFromSelector(@selector(level:message:)),
            @"/ol/" : NSStringFromSelector(@selector(level:message:)),
            @"/if" : NSStringFromSelector(@selector(overflow:message:)),
            @"/of" : NSStringFromSelector(@selector(overflow:message:))
        } ;
    }
    return self;
}

- (void)ignore:(ESManager *)manager message:(F53OSCMessage *)message {
    // nothing to do here
}

- (void)level:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }

    ESIOKind kind = ([message.addressPattern hasPrefix:@"/il/"] ? kIOKindInput : kIOKindOutput) ;
    NSInteger idx = ((NSString *)message.addressParts[1]).integerValue ;
    
    if ([device iocount:kind] <= idx) {
        return ;
    }
    
    NSNumber *val = message.arguments[0] ;
    
    ESInputOutput *io = [device io:idx kind:kind] ;
    io.level = val.floatValue ;
    [manager notifyLevelDidChangeDevice:device io:io] ;
}

- (void)overflow:(ESManager *)manager message:(F53OSCMessage *)message {
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (!device) {
        return ;
    }
    
    ESIOKind kind = ([message.addressPattern hasPrefix:@"/if"] ? kIOKindInput : kIOKindOutput) ;
    NSNumber *val = message.arguments[0] ;
    NSInteger mask = 0b1 ;
    NSArray<ESInputOutput *> *ios = (kind == kIOKindInput ? device.inputs : device.outputs) ;
    for (ESInputOutput *io in ios) {
        if (io.idx == 0) {
            continue ;
        }
        io.overflow = ((val.integerValue & mask) > 0) ;
        mask = mask << 1 ;
        if (kind == kIOKindInput) {
            [manager notifyOverflowDidChangeDevice:device io:io] ;
        }
    }
}
@end
