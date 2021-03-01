//
//  VDSPBus.h
//  AIOController
//
//  Created by Pavel Tsybulin on 13.07.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "VDSPPlugin.h"
#import "FastAioThread.h"

#define NO_INOUT 999

NS_ASSUME_NONNULL_BEGIN

@class VDSPBus ;

@protocol VDSPBusObserver <NSObject>

- (void)didFinishLoopBus:(VDSPBus *)bus ;

@end

@interface VDSPBus : NSObject <FastAioThreadDelegate>

@property (strong) id<VDSPBusObserver> observer ;

@property (readonly) UInt32 idx ;
@property (strong, readonly) NSString *name ;
@property (strong, readonly) NSMutableArray<VDSPPlugin *> *plugins ;
@property float gain ;
@property volatile BOOL mute ;

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithIdx:(UInt32)idx name:(NSString *)name totalInputs:(UInt32)totalInputs totalOutputs:(UInt32)totalOutputs NS_DESIGNATED_INITIALIZER ;

- (UInt32)inputLeft ;
- (UInt32)inputRight ;
- (UInt32)outputLeft ;
- (UInt32)outputRight ;

- (void)setInput:(UInt32)input stereo:(BOOL)stereo ;
- (void)setOutput:(UInt32)output stereo:(BOOL)stereo ;
- (void)start ;
- (void)stop ;

- (void)setupPlugin:(VDSPPlugin *)plugin ;

@end

NS_ASSUME_NONNULL_END
