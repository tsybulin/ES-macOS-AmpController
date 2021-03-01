//
//  VDSPBus.m
//  AIOController
//
//  Created by Pavel Tsybulin on 13.07.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "VDSPBus.h"

#import "VDSPManager.h"
#import "VDSPThread.h"

@interface VDSPBus () {
    UInt32 inputL, inputR, outputL, outputR, totalInputs, totalOutputs ;
    float aioBufferL[ENGINE_MAX_BUFFER_SIZE] ;
    float aioBufferR[ENGINE_MAX_BUFFER_SIZE] ;
    volatile float meterL ;
    volatile float meterR ;
    NSTimer *meterTimer ;
    VDSPThread *thread ;
    volatile BOOL internalMute ;
}

@property  UInt32 idx ;
@property (strong) NSString *name ;
@property (strong) NSMutableArray<VDSPPlugin *> *plugins ;

@end

@implementation VDSPBus

- (instancetype)initWithIdx:(UInt32)idx name:(NSString *)name totalInputs:(UInt32)totalInputs totalOutputs:(UInt32)totalOutputs {
    if (self = [super init]) {
        self.idx = idx ;
        self.name = name ;
        self.mute = NO ;
        self->internalMute = NO ;
        self.plugins = [NSMutableArray array] ;
        self->inputL = NO_INOUT ;
        self->inputR = NO_INOUT ;
        self->outputL = NO_INOUT ;
        self->outputR = NO_INOUT ;
        self->totalInputs = totalInputs ;
        self->totalOutputs = totalOutputs ;
        self.gain = 1.0 ;
        self->thread = [[VDSPThread alloc] initWithBus:self] ;
        self->meterTimer = [NSTimer timerWithTimeInterval:0.066 target:self selector:@selector(onMeter:) userInfo:nil repeats:YES] ;
        [NSRunLoop.mainRunLoop addTimer:self->meterTimer forMode:NSDefaultRunLoopMode] ;
    }
    return self ;
}

- (void)setInput:(UInt32)input stereo:(BOOL)stereo {
    UInt32 inputL = input ;
    UInt32 inputR = input ;
    if (stereo) {
        inputR++ ;
    }

    if (inputL == self->inputL && inputR == self->inputR) {
        return ;
    }

    self->inputL = inputL ;
    self->inputR = inputR ;
    self->meterL = 0 ;
    self->meterR = 0 ;
    bzero(self->aioBufferL, sizeof(float) * ENGINE_MAX_BUFFER_SIZE) ;
    bzero(self->aioBufferR, sizeof(float) * ENGINE_MAX_BUFFER_SIZE) ;
}

- (void)setOutput:(UInt32)output stereo:(BOOL)stereo {
    UInt32 outputL = output ;
    UInt32 outputR = output ;
    if (stereo) {
        outputR++ ;
    }

    if (outputL == self->outputL && outputR == self->outputR) {
        return ;
    }

    if (!self.mute) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->internalMute = YES ;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.16 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                self->internalMute = NO ;
                self->outputL = outputL ;
                self->outputR = outputR ;
                self->meterL = 0 ;
                self->meterR = 0 ;
            });
        }) ;
    } else {
        self->outputL = outputL ;
        self->outputR = outputR ;
        self->meterL = 0 ;
        self->meterR = 0 ;
    }
}

- (void)stop {
    [self->thread stop] ;
    self->meterL = 0 ;
    self->meterR = 0 ;
}

- (void)start {
    [self->thread start] ;
}

- (void)onMeter:(NSTimer *)timer {
    dispatch_async(dispatch_get_main_queue(), ^{
        VDSPManager *mgr = [VDSPManager sharedManager] ;

        float dbm = -160 ;
        float v = self->meterL ;
        if (v) {
            dbm = 20.0f * log10f(v) ;
        }
        [mgr.observer manager:mgr didChangeLevel:dbm channel:self.idx * 2] ;

        dbm = -160 ;
        v = self->meterR ;

        if (v) {
            dbm = 20.0f * log10f(v) ;
        }
        [mgr.observer manager:mgr didChangeLevel:dbm channel:self.idx * 2 + 1] ;
    }) ;
}

- (UInt32)inputLeft {
    return self->inputL ;
}

- (UInt32)inputRight {
    return self->inputR ;
}

- (UInt32)outputLeft {
    return self->outputL ;
}

- (UInt32)outputRight {
    return self->outputR ;
}

//MARK: - <FastAioThreadDelegate>

- (BOOL)listenForChannel:(UInt32)channel {
    return channel == self->inputL || channel == self->inputR ;
}

- (void)inputSamples:(UInt32)samples buffer:(float *)buffer channel:(UInt32)channel {
    if (self.mute || self->internalMute) {
        if (channel == self->inputL) {
            bzero(self->aioBufferL, sizeof(float) * samples) ;
        }
        
        if (channel == self->inputR) {
            bzero(self->aioBufferR, sizeof(float) * samples) ;
        }
        
        return ;
    }
    
    if (channel == self->inputL) {
        memcpy(self->aioBufferL, buffer, sizeof(float) * samples) ;
    }
    
    if (channel == self->inputR) {
        memcpy(self->aioBufferR, buffer, sizeof(float) * samples) ;
    }
}

- (BOOL)outToChannel:(UInt32)channel {
    return channel == self->outputL || channel == self->outputR ;
}

- (void)outputSamples:(UInt32)samples buffer:(float *)buffer channel:(UInt32)channel sum:(BOOL)sum {
    float gain = self.gain ;
    
    if (self.mute || self->internalMute) {
        gain = 0.0 ;
    }
    
    if (sum) {
        if (channel == self->outputL) {
            for (UInt32 i = 0; i < samples; i++) {
                buffer[i] += self->aioBufferL[i] * gain ;

                self->meterL -= self->meterL * 0.0002f ;
                float f = fabsf(self->aioBufferL[i] * gain) ;
                if (f > self->meterL) {
                    self->meterL = f ;
                }
            }
        } else if (channel == self->outputR) {
            for (UInt32 i = 0; i < samples; i++) {
                buffer[i] += self->aioBufferR[i] * gain ;

                self->meterR -= self->meterR * 0.0002f ;
                float f = fabsf(self->aioBufferR[i] * gain) ;
                if (f > self->meterR) {
                    self->meterR = f ;
                }
            }
        }
        
        return ;
    }
    
    if (channel == self->outputL) {
        for (UInt32 i = 0; i < samples; i++) {
            buffer[i] = self->aioBufferL[i] * gain ;

            self->meterL -= self->meterL * 0.0002f ;
            float f = fabsf(self->aioBufferL[i] * gain) ;
            if (f > self->meterL) {
                self->meterL = f ;
            }
        }
    } else if (channel == self->outputR) {
        for (UInt32 i = 0; i < samples; i++) {
            buffer[i] = self->aioBufferR[i] * gain ;

            self->meterR -= self->meterR * 0.0002f ;
            float f = fabsf(self->aioBufferR[i] * gain) ;
            if (f > self->meterR) {
                self->meterR = f ;
            }
        }
    }
}

- (void)preloopSamples:(UInt32)samples {
    if (self->thread && [self->thread isRunning]) {
        [self->thread preloopSamples:samples left:self->aioBufferL right:self->aioBufferR] ;
    }
}

- (void)wakeup {
    if (self->thread && [self->thread isRunning]) {
        [self->thread wakeup] ;
    }
}

- (void)postloopSamples:(UInt32)samples {
    if (self->thread && [self->thread isRunning]) {
        [self->thread postloopSamples:samples left:self->aioBufferL right:self->aioBufferR] ;
    }
}

- (void)setupPlugin:(VDSPPlugin *)plugin {
    if (plugin.pluginType == kPluginTypeAU && self->thread) {
        [self->thread setupPlugin:plugin] ;
    }
}

@end
