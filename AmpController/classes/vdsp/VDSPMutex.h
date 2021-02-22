//
//  VDSPMutex.h
//  AIOController
//
//  Created by Pavel Tsybulin on 13.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VDSPMutex : NSObject

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithCount:(UInt32)count NS_DESIGNATED_INITIALIZER ;

- (BOOL)active ;
- (void)reset ;
- (void)signal ;
- (UInt32)wait ;

@end

NS_ASSUME_NONNULL_END
