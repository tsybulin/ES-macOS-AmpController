//
//  VDSPThread.h
//  AIOController
//
//  Created by Pavel Tsybulin on 13.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import "VDSPBus.h"

NS_ASSUME_NONNULL_BEGIN

@interface VDSPThread : NSObject

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithBus:(VDSPBus *)bus NS_DESIGNATED_INITIALIZER ;

- (BOOL)isRunning ;
- (void)start ;
- (void)stop ;

- (void)preloopSamples:(UInt32)samples left:(float *)left right:(float *)right ;
- (void)wakeup ;
- (void)loopSamples:(UInt32)samples audioBufferList:(AudioBufferList *)abl ;
- (void)postloopSamples:(UInt32)samples left:(float *)left right:(float *)right ;

- (void)setupPlugin:(VDSPPlugin *)plugin ;

@end

NS_ASSUME_NONNULL_END
