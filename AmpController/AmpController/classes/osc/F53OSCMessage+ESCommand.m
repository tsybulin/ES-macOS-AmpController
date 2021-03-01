//
//  F53OSCMessage+ESCommand.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "F53OSCMessage+ESCommand.h"

@implementation F53OSCMessage (ESCommand)

+ (F53OSCMessage *) messageWithCommand:(ESCommand *)command {
    return [F53OSCMessage messageWithAddressPattern:command.path arguments:NULL] ;
}

+ (F53OSCMessage *) messageWithCommand:(ESCommand *)command args:(NSArray *)args {
    return [F53OSCMessage messageWithAddressPattern:command.path arguments:args] ;
}

@end
