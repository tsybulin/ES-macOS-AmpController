//
//  FastAioUtils.h
//  AIOController
//
//  Created by Pavel Tsybulin on 05.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "fast_aio.h"

NS_ASSUME_NONNULL_BEGIN

@interface FastAioUtils : NSObject

+ (BOOL)openDriver:(FAST_AIO_DRV *)drv ;
+ (void)closeDriver:(FAST_AIO_DRV *)drv ;

@end

NS_ASSUME_NONNULL_END
