//
//  CommandHandler.h
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class ESManager ;
@class ESCommand ;
@class F53OSCMessage ;

@interface CommandHandler : NSObject {
    NSDictionary<NSString*, NSString*> *handlers ;
}

- (void)handleMessage:(ESManager *)manager command:(ESCommand *)command message:(F53OSCMessage *)message ;

@end

@interface BroadcastHandler : CommandHandler

@end

@interface DeviceConfigurationHandler : CommandHandler

@end

@interface IOConfigurationHandler : CommandHandler

@end

@interface MeterHandler : CommandHandler

@end

NS_ASSUME_NONNULL_END
