//
//  CommandHandler.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "CommandHandler.h"

#import "F53OSC.h"
#import "ESCommand.h"
#import "ESDevice.h"
#import "ESManager.h"
#import "F53OSCMessage+ESCommand.h"

@implementation CommandHandler

- (instancetype)init {
    if (self = [super init]) {
        handlers = @{} ;
    }
    return self;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"

- (void)handleMessage:(ESManager *)manager command:(ESCommand *)command message:(F53OSCMessage *)message {
    __block BOOL handled = false ;
    
    [handlers enumerateKeysAndObjectsUsingBlock:^(NSString *path, NSString *sel, BOOL *stop) {
        if ([message.addressPattern hasPrefix:path]) {
            *stop = true ;
            handled = true ;
            SEL aSel = NSSelectorFromString(sel) ;
            if ([self respondsToSelector:aSel]) {
                [self performSelector:aSel withObject:manager withObject:message] ;
            }
        } else {
            *stop = false ;
        }
    }] ;
    
    if (!handled) {
        NSLog(@"CommandHandler.handle Unhandled %@", message.addressPattern) ;
    }
}

#pragma clang diagnostic pop

@end
