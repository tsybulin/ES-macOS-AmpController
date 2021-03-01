//
//  VDSPThread.m
//  AIOController
//
//  Created by Pavel Tsybulin on 13.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "VDSPThread.h"

#import "FastAioThread.h"
#import <pthread/pthread.h>
#import "VDSPMutex.h"

typedef NS_ENUM(NSInteger, OneTwo) {
    kOne = 0,
    kTwo = 1
} ;

@interface VDSPThread () {
    VDSPBus *bus ;
    NSThread *thread ;
    volatile BOOL running ;
    AudioBufferList *abl_one, *abl_two ;
    dispatch_semaphore_t sema ;
    UInt32 samples ;
    OneTwo whichBuffer ;
}

@end

static OSStatus auRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
    VDSPThread *vt = (__bridge VDSPThread *) inRefCon ;
    [vt loopSamples:inNumberFrames audioBufferList:ioData] ;
    return noErr ;
}

@implementation VDSPThread

- (AudioBufferList *)createAudioBufferList {
    AudioBufferList *abl = NULL ;
    abl = calloc(1, offsetof(AudioBufferList, mBuffers) + (sizeof(AudioBuffer) * 2)) ;
    abl->mNumberBuffers = 2 ;
    
    for(int i = 0; i < abl->mNumberBuffers; i++) {
        abl->mBuffers[i].mData = malloc(sizeof(float) * ENGINE_MAX_BUFFER_SIZE) ;
        abl->mBuffers[i].mDataByteSize = sizeof(float) * 8;
        abl->mBuffers[i].mNumberChannels = 1 ;
    }
    
    return abl;
}

- (instancetype)initWithBus:(VDSPBus *)bus {
    if (self = [super init]) {
        self->bus = bus ;
        self->running = NO ;
        self->whichBuffer = kOne ;
        self->sema = dispatch_semaphore_create(0) ;
        
        self->abl_one = [self createAudioBufferList] ;
        self->abl_two = [self createAudioBufferList] ;
    }
    return self;
}

- (void)start {
    if (self->thread) {
        if (self->thread.executing) {
            [self->thread cancel] ;
        }
        self->thread = nil ;
    }
    
    self->thread = [[NSThread alloc] initWithTarget:self selector:@selector(executor:) object:nil] ;
    self->thread.threadPriority = 1.0 ;
    self->thread.name = [NSString stringWithFormat:@"VDSPThread.%d.Thread", self->bus.idx] ;

    [self->thread start] ;
}

- (void)stop {
    if (self->thread && self->thread.executing) {
        [self->thread cancel] ;
        [self wakeup] ;
    }
    
    self->thread = nil ;
}

- (BOOL)isRunning {
    return self->running ;
}

- (void)wakeup {
    dispatch_semaphore_signal(self->sema) ;
}

- (void)preloopSamples:(UInt32)samples left:(float *)left right:(float *)right {
    memcpy(self->abl_one->mBuffers[0].mData, left, samples * sizeof(float)) ;
    memcpy(self->abl_one->mBuffers[1].mData, right, samples * sizeof(float)) ;
    self->abl_one->mBuffers[0].mDataByteSize = samples * sizeof(float) ;
    self->abl_one->mBuffers[1].mDataByteSize = samples * sizeof(float) ;

    self->whichBuffer = kOne ;
    self->samples = samples ;
}

- (void)loopSamples:(UInt32)samples audioBufferList:(AudioBufferList *)abl {
    for (UInt32 i = 0; i < abl->mNumberBuffers && i < 2; i++) {
        if (self->whichBuffer == kOne) {
            memcpy(abl->mBuffers[i].mData, self->abl_one->mBuffers[i].mData, samples * sizeof(float)) ;
        } else {
            memcpy(abl->mBuffers[i].mData, self->abl_two->mBuffers[i].mData, samples * sizeof(float)) ;
        }
    }
}

- (void)postloopSamples:(UInt32)samples left:(float *)left right:(float *)right {
    if (self->whichBuffer == kOne) {
        memcpy(left, self->abl_one->mBuffers[0].mData, samples * sizeof(float)) ;
        memcpy(right, self->abl_one->mBuffers[1].mData, samples * sizeof(float)) ;
    } else {
        memcpy(left, self->abl_two->mBuffers[0].mData, samples * sizeof(float)) ;
        memcpy(right, self->abl_two->mBuffers[1].mData, samples * sizeof(float)) ;
    }

}

- (void)executor:(id)argument {
    NSLog(@"VDSPThread %d start", self->bus.idx) ;
    
    self->running = YES ;
    
    struct mach_timebase_info theTimeBaseInfo ;
    mach_timebase_info(&theTimeBaseInfo) ;
    Float64 theHostClockFrequency = (Float64) theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer ;
    theHostClockFrequency *= 1000000000.0 ;
    struct thread_time_constraint_policy ttcp ;
    ttcp.period = theHostClockFrequency / 1000 ;
    ttcp.computation = theHostClockFrequency / 2000 ;
    ttcp.constraint = theHostClockFrequency / 1500 ;
    ttcp.preemptible = 0 ;
    
    int pric = 0;

    while (self->thread && !self->thread.cancelled) {
        if (pric == 0) {
            if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&ttcp, THREAD_TIME_CONSTRAINT_POLICY_COUNT) != 0) {
                NSLog(@"VDSPThread set_realtime failed") ;
            }
        }
        
        if (pric++ >= 1000) {
            pric = 0 ;
        }
        
        if (dispatch_semaphore_wait(self->sema, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC))) != 0) {
            NSLog(@"VDSPThread.%d sema_wait timeout", self->bus.idx) ;
        }

        if (!self->thread || self->thread.cancelled) {
            break ;
        }
        
        @synchronized (self->bus.plugins) {
            for (VDSPPlugin *plugin in self->bus.plugins) {
                AudioUnitRenderActionFlags rf = kAudioOfflineUnitRenderAction_Render ;
                AudioTimeStamp ts = {0} ;
                AVAudioUnit *avAudioUnit ;
                OSStatus result = noErr ;
                
                if (plugin.pluginType == kPluginTypeAU) {
                    avAudioUnit = (AVAudioUnit *)plugin.plugin ;
                    struct mach_timebase_info theTimeBaseInfo ;
                    mach_timebase_info(&theTimeBaseInfo) ;
                    double absTime = mach_absolute_time() ;
                    double d = (absTime / theTimeBaseInfo.denom) * theTimeBaseInfo.numer ;
                    ts.mHostTime = d;
                    ts.mSampleTime = d;
                    ts.mRateScalar = 1;
                    ts.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid | kAudioTimeStampRateScalarValid ;
                }
                
                abl_one->mBuffers[0].mDataByteSize = sizeof(float) * samples ;
                abl_one->mBuffers[1].mDataByteSize = sizeof(float) * samples ;
                abl_two->mBuffers[0].mDataByteSize = sizeof(float) * samples ;
                abl_two->mBuffers[1].mDataByteSize = sizeof(float) * samples ;

                if (self->whichBuffer == kOne) {
                    bzero(self->abl_two->mBuffers[0].mData, samples * sizeof(float)) ;
                    bzero(self->abl_two->mBuffers[1].mData, samples * sizeof(float)) ;
                    
                    if (plugin.pluginType == kPluginTypeAU) {
                        // render to second buffer
                        result = AudioUnitRender(avAudioUnit.audioUnit, &rf, &ts, 0, self->samples, self->abl_two) ;
                    }
                } else {
                    bzero(self->abl_one->mBuffers[0].mData, self->samples * sizeof(float)) ;
                    bzero(self->abl_one->mBuffers[1].mData, self->samples * sizeof(float)) ;
                    
                    if (plugin.pluginType == kPluginTypeAU) {
                        // render to second buffer
                        result = AudioUnitRender(avAudioUnit.audioUnit, &rf, &ts, 0, self->samples, self->abl_one) ;
                    }
                }
                
                if (plugin.pluginType == kPluginTypeAU && result != noErr) {
                    NSLog(@"VDSPThread.executor AudioUnitRender error: %d", result) ;
                }
                
                self->whichBuffer = self->whichBuffer == kOne ? kTwo : kOne ;
            }
        }
        
        [self->bus.observer didFinishLoopBus:self->bus] ;
    }

    self->running = NO ;

    [self->bus.observer didFinishLoopBus:self->bus] ;

    NSLog(@"VDSPThread %d stop", self->bus.idx) ;
}

- (void)setupPlugin:(VDSPPlugin *)plugin {
    if (plugin.pluginType == kPluginTypeAU) {
        AURenderCallbackStruct aurcs = {
            .inputProc = &auRenderCallback,
            .inputProcRefCon  = (__bridge void * _Nullable)(self)
        } ;
        
        AVAudioUnit *avAudioUnit = (AVAudioUnit *)plugin.plugin ;
        
        OSStatus result = AudioUnitSetProperty(avAudioUnit.audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &aurcs, sizeof (aurcs)) ;
        if (result != noErr) {
            NSLog(@"VDSPThread.setupPlugin AudioUnitSet RenderCallback error: %d", result) ;
        }
    }
}

@end
