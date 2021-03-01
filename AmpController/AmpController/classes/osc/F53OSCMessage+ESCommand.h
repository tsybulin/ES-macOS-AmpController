//
//  F53OSCMessage+ESCommand.h
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "F53OSCMessage.h"
#import "ESCommand.h"

NS_ASSUME_NONNULL_BEGIN

@interface F53OSCMessage (ESCommand)

+ (F53OSCMessage *) messageWithCommand:(ESCommand *)command ;
+ (F53OSCMessage *) messageWithCommand:(ESCommand *)command args:(NSArray *)args ;

@end

NS_ASSUME_NONNULL_END
