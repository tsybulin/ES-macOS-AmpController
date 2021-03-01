//
//  BroadcastHandler.m
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

@implementation BroadcastHandler

- (void)handleMessage:(ESManager *)manager command:(ESCommand *)command message:(F53OSCMessage *)message {
    if (message.arguments.count == 1) {
        NSNumber *deviceType = message.arguments[0] ;
        
        // Broadcast from another Workstation
        if (deviceType.integerValue == 5) {
            return ;
        }
    }
    
    ESDevice *device = [manager device:message.replySocket.host] ;
    if (device != nil) {
        device.timestamp = [NSDate date] ;
        
        if (device.confTimeout > 0) {
//            NSLog(@"Device %@ confTimeout %d", device.ip, device.confTimeout) ;
            if (--device.confTimeout == 0) {
//                NSLog(@"Device %@ reconfiguration", device.ip) ;
                F53OSCMessage *reply = [F53OSCMessage messageWithCommand:[ESCConfigurationRequest command]] ;
                [manager sendMessage:reply toHost:message.replySocket.host onPort:DEFAULT_DEVICE_PORT] ;
                return ;
            }
        }
        
        F53OSCMessage *reply = [F53OSCMessage messageWithCommand:command args:@[@(5)]] ;
        [manager sendMessage:reply toHost:message.replySocket.host onPort:DEFAULT_DEVICE_PORT] ;
    } else {
        device = [[ESDevice alloc] initWithTitle:message.replySocket.host ip:message.replySocket.host port:DEFAULT_DEVICE_PORT] ;
        [manager addDevice:device] ;

        device.confTimeout = 5 ;
        F53OSCMessage *reply = [F53OSCMessage messageWithCommand:[ESCConfigurationRequest command]] ;
        [manager sendMessage:reply toHost:message.replySocket.host onPort:DEFAULT_DEVICE_PORT] ;
    }
}

@end
