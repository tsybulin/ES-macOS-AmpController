//
//  EngineThread.h
//  AIOController
//
//  Created by Pavel Tsybulin on 04.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "fast_aio.h"

#define ENGINE_DEFAULT_BUFFER_SIZE 8
#define ENGINE_MAX_BUFFER_SIZE 1032
#define BUS_COUNT 1

NS_ASSUME_NONNULL_BEGIN

@protocol FastAioThreadDelegate <NSObject>

- (BOOL)listenForChannel:(UInt32)channel ;
- (void)inputSamples:(UInt32)samples buffer:(float *)buffer channel:(UInt32)channel ;

- (BOOL)outToChannel:(UInt32)channel ;
- (void)outputSamples:(UInt32)samples buffer:(float *)buffer channel:(UInt32)channel sum:(BOOL)sum  ;

- (void)preloopSamples:(UInt32)samples ;
- (void)wakeup ;
- (void)postloopSamples:(UInt32)samples ;

@end

@interface FastAioThread : NSObject

@property (strong) NSMutableSet<id<FastAioThreadDelegate>> *delegates ;

@property UInt32 engineBufferSize ;

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithFastAio:(FAST_AIO_DRV *)drv NS_DESIGNATED_INITIALIZER ;

- (void)start ;
- (void)stop ;
- (BOOL)isRunning ;
- (void)didFinishLoopBus ;

@end

NS_ASSUME_NONNULL_END
