//
//  VDSPManager.m
//  AIOController
//
//  Created by Pavel Tsybulin on 12.08.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "VDSPManager.h"
#import "FastAioUtils.h"

#define DEVICE_NAME @"IPAPro"

@interface VDSPManager () <VDSPBusObserver> {
    NSArray<VDSPBus *> *buses ;
    FAST_AIO_DRV drv ;
    FastAioThread *fastAioThread ;
    UInt32 totalInputs, totalOutputs ;
}

@end

@implementation VDSPManager

- (instancetype)init {
    if (self = [super init]) {
        totalInputs = 0 ;
        totalOutputs = 0 ;
        self.sampleFreq = 44100 ;

        if ([FastAioUtils openDriver:&self->drv]) {
            AIOTRX_SHARED_MEMORY *driverMem = (&self->drv)->driverMem ;
            self->totalInputs = driverMem->num_inputs ;
            self->totalOutputs = driverMem->num_outputs ;
            [FastAioUtils closeDriver:&self->drv] ;
        }

        self->fastAioThread = [[FastAioThread alloc] initWithFastAio:&self->drv] ;

        NSMutableArray<VDSPBus *> *buses = [NSMutableArray array] ;
        for (UInt32 idx = 0; idx < BUS_COUNT; idx++) {
            VDSPBus *bus = [[VDSPBus alloc] initWithIdx:idx name:[NSString stringWithFormat:@"VBUS %u/%u", idx*2+1, idx*2+2] totalInputs:self->totalInputs totalOutputs:self->totalOutputs] ;
            [buses addObject:bus] ;
            [self->fastAioThread.delegates addObject:bus] ;
            bus.observer = self ;
        }

        self->buses = buses ;
        
        [self prepareEngine] ;
        
    }
    return self ;
}

+ (instancetype)sharedManager {
    static VDSPManager *sharedInstance = nil ;
    @synchronized (self) {
        if (!sharedInstance) {
            sharedInstance = [[VDSPManager alloc] init] ;
        }
        
        return sharedInstance ;
    }
}

- (UInt32)engineBufferSize {
    if (self->fastAioThread) {
        return self->fastAioThread.engineBufferSize ;
    }
    
    return ENGINE_DEFAULT_BUFFER_SIZE ;
}

- (void)setEngineBufferSize:(UInt32)engineBufferSize {
    if (self->fastAioThread) {
        if (self->fastAioThread.isRunning) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self stop] ;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                    self->fastAioThread.engineBufferSize = engineBufferSize ;
                    [self start] ;
                });
            }) ;

        } else {
            self->fastAioThread.engineBufferSize = engineBufferSize ;
        }
    }
}

- (void)prepareEngine {
//TODO: implement
}

- (VDSPBus *)busWithIdx:(UInt32)idx {
    return self->buses[idx] ;
}

- (void)setupPlugin:(VDSPPlugin *)plugin bus:(VDSPBus *)bus {
    if (plugin.pluginType == kPluginTypeAU) {
        [bus setupPlugin:plugin] ;
    }
}

- (void)start {
    for (VDSPBus *bus in self->buses) {
        for (VDSPPlugin *plugin in bus.plugins) {
            [self setupPlugin:plugin bus:bus] ;
        }

        [bus start] ;
    }
    
    if ([FastAioUtils openDriver:&self->drv]) {
        [self->fastAioThread start] ;
    }

    if (self.observer) {
        for (UInt32 bn = 0; bn < BUS_COUNT; bn++) {
            const float maxDbm = 0 ;
            const float minDbm = -60 ;
            float newDbm = minDbm + (maxDbm - minDbm) * self->buses[bn].gain ;
            [self.observer manager:self didChangeGainDbm:newDbm channel:bn*2] ;
            [self.observer manager:self didChangeGainDbm:newDbm channel:bn*2+1] ;
        }
    }
}

- (void)setOutput:(UInt32)output forBus:(UInt32)bus stereo:(BOOL)stereo {
    [self->buses[bus] setOutput:output stereo:stereo] ;
}

- (void)setInput:(UInt32)input forBus:(UInt32)bus stereo:(BOOL)stereo {
    [self->buses[bus] setInput:input stereo:stereo] ;
}

- (void)setGain:(float)level forBus:(UInt32)bus stereo:(BOOL)stereo {
    self->buses[bus].gain = level ;
    
    if (self.observer) {
        const float maxDbm = 0 ;
        const float minDbm = -60 ;
        float newDbm = minDbm + (maxDbm - minDbm) * level ;

        [self.observer manager:self didChangeGainDbm:newDbm channel:(bus * 2)] ;
    }
}

- (void)stop {
    [self->fastAioThread stop] ;

    for (VDSPBus *bus in self->buses) {
        [bus stop] ;
    }
    
    while ([self->fastAioThread isRunning]) {
        [NSThread sleepForTimeInterval:0.1] ;
    }
    
    [FastAioUtils closeDriver:&self->drv] ;
}

- (BOOL)started {
    return [self->fastAioThread isRunning] ;
}

- (UInt32)numberOfInputs {
    return self->totalInputs ;
}

- (UInt32)numberOfOutputs {
    return self->totalOutputs ;
}

- (void)addPlugin:(VDSPPlugin *)plugin toBus:(UInt32)busIdx {
    VDSPBus *bus = self->buses[busIdx] ;
    
    @synchronized (bus.plugins) {
        [bus.plugins addObject:plugin] ;
        
        if ([self started]) {
            [self setupPlugin:plugin bus:bus] ;
        }
    }

    if (self.observer) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.observer manager:self didChangeBus:busIdx] ;
        }) ;
    }
}

- (void)removePlugin:(VDSPPlugin *)plugin fromBus:(UInt32)busIdx {
    VDSPBus *bus = self->buses[busIdx] ;

    @synchronized (bus.plugins) {
        //[VSTUtils setProcessing:NO plugin:plugin] ;

        //[VSTUtils setActive:NO plugin:plugin] ;
        //[VSTUtils terminatePlugin:plugin] ;
        
        [bus.plugins removeObject:plugin] ;
    }

    if (self.observer) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.observer manager:self didChangeBus:busIdx] ;
        }) ;
    }
}

- (void)movePluginFromRow:(NSInteger)fromRow toRow:(NSInteger)toRow bus:(UInt32)busIdx {
    VDSPBus *bus = self->buses[busIdx] ;
    
    @synchronized (bus.plugins) {
        VDSPPlugin *plugin = bus.plugins[fromRow] ;

        NSMutableArray<VDSPPlugin *> *plugins = [NSMutableArray array] ;

        for (VDSPPlugin *plugin in bus.plugins) {
            [plugins addObject:plugin] ;
        }
        [bus.plugins removeAllObjects] ;

        [plugins insertObject:plugin atIndex:toRow] ;

        if (fromRow < toRow) {
            [plugins removeObjectAtIndex:fromRow] ;
        } else {
            [plugins removeObjectAtIndex:fromRow + 1] ;
        }

        for (VDSPPlugin *plugin in plugins) {
            [bus.plugins addObject:plugin] ;
        }
    }

    if (self.observer) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.observer manager:self didChangeBus:busIdx] ;
        }) ;
    }
}

//MARK: - <VDSPBusObserver>

- (void)didFinishLoopBus:(VDSPBus *)bus {
    [self->fastAioThread didFinishLoopBus] ;
}

@end
