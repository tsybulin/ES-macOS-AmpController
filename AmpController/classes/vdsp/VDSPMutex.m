//
//  VDSPMutex.m
//  AIOController
//
//  Created by Pavel Tsybulin on 13.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "VDSPMutex.h"

@interface VDSPMutex () {
    dispatch_semaphore_t sema ;
    UInt32 initialCount ;
    volatile UInt32 count ;
    NSObject *mreset, *mwait, *msignal ;
}

@end

@implementation VDSPMutex

- (instancetype)initWithCount:(UInt32)count {
    if (self = [super init]) {
        self->initialCount = count ;
        self->count = count ;
        self->sema = dispatch_semaphore_create(0) ;
        self->mreset = [[NSObject alloc] init] ;
        self->mwait = [[NSObject alloc] init] ;
        self->msignal = [[NSObject alloc] init] ;
    }
    return self;
}

- (BOOL)active {
    return self->count > 0 ;
}

- (void)reset {
    @synchronized (self->mreset) {
        self->count = self->initialCount ;
    }
}

- (void)signal {
    @synchronized (self->msignal) {
        if (self->count > 0) {
            self->count-- ;
        }
        
        if (self->count == 0) {
            dispatch_semaphore_signal(self->sema) ;
        }
    }
}


- (UInt32)wait {
    @synchronized (self->mwait) {
        if (dispatch_semaphore_wait(self->sema, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1 * NSEC_PER_SEC))) > 0) {
            NSLog(@"4xmutex wait timeout") ;
        }
        return self->count ;
    }
}
    
@end
