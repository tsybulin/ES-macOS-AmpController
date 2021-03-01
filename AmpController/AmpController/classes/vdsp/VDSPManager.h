//
//  VDSPManager.h
//  AIOController
//
//  Created by Pavel Tsybulin on 12.08.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "FastAioThread.h"
#import "VDSPBus.h"

NS_ASSUME_NONNULL_BEGIN

@class VDSPManager ;

@protocol VDSPManagerObserver <NSObject>

- (void)manager:(VDSPManager *)manager didChangeLevel:(float)level channel:(UInt32)channel ;
- (void)manager:(VDSPManager *)manager didChangeGainDbm:(float)level channel:(UInt32)channel ;
- (void)manager:(VDSPManager *)manager didChangeBus:(UInt32)busIdx ;

@end

@interface VDSPManager : NSObject

@property (assign) id<VDSPManagerObserver> observer ;

@property (nonatomic) UInt32 engineBufferSize  ;
@property UInt32 sampleFreq ;

+ (instancetype)sharedManager ;
- (VDSPBus *)busWithIdx:(UInt32)idx ;
- (BOOL)started ;
- (void)start ;
- (void)stop ;
- (void)setOutput:(UInt32)output forBus:(UInt32)bus stereo:(BOOL)stereo  ;
- (void)setInput:(UInt32)input forBus:(UInt32)bus stereo:(BOOL)stereo ;
- (void)setGain:(float)level forBus:(UInt32)bus stereo:(BOOL)stereo ;
- (UInt32)numberOfInputs ;
- (UInt32)numberOfOutputs ;

- (void)addPlugin:(VDSPPlugin *)plugin toBus:(UInt32)busIdx ;
- (void)removePlugin:(VDSPPlugin *)plugin fromBus:(UInt32)busIdx ;
- (void)movePluginFromRow:(NSInteger)fromRow toRow:(NSInteger)toRow bus:(UInt32)busIdx ;

@end

NS_ASSUME_NONNULL_END
