//
//  ESCommand.h
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "ESDevice.h"

NS_ASSUME_NONNULL_BEGIN

@interface ESCommand : NSObject <NSCopying>

@property (nonatomic, readonly) NSString *path ;

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithPath:(NSString *)path NS_DESIGNATED_INITIALIZER ;

@end

@interface ESCBroadcast : ESCommand

+ (instancetype)command ;

@end

@interface ESCConfigurationRequest : ESCommand

+ (instancetype)command ;

@end

@interface ESCInputConfigurationRequest : ESCommand

+ (instancetype)command ;

@end

@interface ESCOutputConfigurationRequest : ESCommand

+ (instancetype)command ;

@end

@interface ESCGain : ESCommand

+ (instancetype)commandForIdx:(NSUInteger)idx kind:(ESIOKind)kind ;

@end

@interface ESCInputOverflow : ESCommand

+ (instancetype)command ;

@end

@interface ESCInputAutogain : ESCommand

+ (instancetype)commandForIdx:(NSUInteger)idx ;

@end

@interface ESCInputPhantomPower : ESCommand

+ (instancetype)commandForIdx:(NSUInteger)idx ;

@end

@interface ESCSyncFrequency : ESCommand

+ (instancetype)command ;

@end

@interface ESCInputMapping : ESCommand

+ (instancetype)commandForIdx:(NSUInteger)idx ;

@end

@interface ESCOutputMapping : ESCommand

+ (instancetype)commandForIdx:(NSUInteger)idx ;

@end

NS_ASSUME_NONNULL_END
