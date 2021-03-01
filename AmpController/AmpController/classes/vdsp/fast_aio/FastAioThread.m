//
//  EngineThread.m
//  AIOController
//
//  Created by Pavel Tsybulin on 04.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "FastAioThread.h"

#import <IOKit/IOKitLib.h>

#import <sys/socket.h>
#import <sys/sys_domain.h>
#import <pthread/pthread.h>
#import "VDSPMutex.h"
#import <mach/mach_time.h>
#import <mach/thread_act.h>

@interface FastAioThread () {
    NSThread *thread ;
    FAST_AIO_DRV *drv ;
    float aioBuffer[ENGINE_MAX_BUFFER_SIZE] ;
    BOOL running ;
    VDSPMutex *mutex ;
}

@end

@implementation FastAioThread

- (instancetype)initWithFastAio:(FAST_AIO_DRV *)drv {
    if (self = [super init]) {
        self.delegates = [NSMutableSet set] ;
        self->drv = drv ;
        self->running = NO ;
        self.engineBufferSize = ENGINE_DEFAULT_BUFFER_SIZE ;
        self->mutex = [[VDSPMutex alloc] initWithCount:BUS_COUNT] ;
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
    self->thread.name = @"FastAio.Thread" ;
    
    [self->mutex reset] ;
    
    [self->thread start] ;
}

- (void)stop {
    if (self->thread && self->thread.executing) {
        [self->thread cancel] ;
    }
    
    self->thread = nil ;
}

- (BOOL)isRunning {
    return self->running ;
}

- (BOOL)configurationChanged {
    unsigned int flag = 1 ;
    atomic_compare_exchange_strong(self->drv->driverMem->ConfigurationChanged+drv->ClientN, &flag, 0) ;
    return flag == 1 ;
}

- (BOOL)startTRX {
    if (drv->srv_is_US) {
        uint32_t inputSEQ;
        uint32_t timeout;
        timeout=10000;
        inputSEQ=atomic_load(&drv->driverMem->readySEQ);
        atomic_store(&drv->driverMem->client_states[drv->ClientN],2);
        do {
            if (!(--timeout)) {
                return NO;
            }
            usleep(100);
        } while(inputSEQ==atomic_load(&drv->driverMem->readySEQ));
        
        inputSEQ=atomic_load(&drv->driverMem->readySEQ);
        
        do {
            if (!(--timeout)) {
                return NO;
            }
            usleep(100);
        } while(inputSEQ==atomic_load(&drv->driverMem->readySEQ));

        return YES;
    }
    
    return IOConnectCallMethod(self->drv->driverConnection, kAIOdriver_start, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) == 0 ;
}

- (BOOL)stopTRX {
    if (drv->srv_is_US) {
        bzero(drv->client_output->output_channels,sizeof(drv->client_output->output_channels));
        atomic_store(&drv->driverMem->client_states[drv->ClientN],1);
        return YES;
    }

    return IOConnectCallMethod(self->drv->driverConnection, kAIOdriver_stop, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL) == 0 ;
}

- (BOOL)waitRXseq:(uint32_t)seq {
    uint32_t ClientN = self->drv->ClientN ;
    
    if (drv->srv_is_US) {
        int32_t d;
        if (!drv->WaitLock[1]) {
            return YES;
        }
        sem_post(drv->WaitLock[1]); //Notify to transmitter that data is ready
        //Wait until right sequence number reached
        for(;;) {
            d=atomic_load(&drv->driverMem->curtx_SEQ)-seq;
            if (d>MAX_AIO_BLOCKS)
            {
                NSLog(@"TOO LATE");
                return NO;
            }
            d=atomic_load(&drv->driverMem->readySEQ)-seq;
            if (d>=0) return YES;
            atomic_store(&drv->watchdog_timer,2);
            //atomic_store(&drv->watchdog_abort,0);
            atomic_store(&drv->driverMem->waitSEQ[ClientN],seq);
            if (sem_wait(drv->WaitLock[0])) {
                if (errno==EINTR) {
                    atomic_store(&drv->watchdog_timer,-1);
                    NSLog(@"EINTR");
                    return NO;
                }
                atomic_store(&drv->watchdog_timer,-1);
                NSLog(@"sem_wait error %d",errno);
                return NO;
            }
            //atomic_store(&drv->watchdog_timer,0);
            if (atomic_load(&drv->watchdog_timer)<0)
            {
                NSLog(@"WDT abort");
                return NO;
            }
        }
    }

    if (self->drv->ctl_sock >= 0) {
        self->drv->driverMem->waitSEQ[ClientN] = seq ;
        return setsockopt(self->drv->ctl_sock, SYSPROTO_CONTROL, FAST_WAIT_RX_SEQ + ClientN, NULL, 0) == 0 ;
    } else {
        uint64_t scalarIn[1] ;
        scalarIn[0] = seq ;
        return IOConnectCallScalarMethod(self->drv->driverConnection, kAIOdriver_wait_rx_seq, scalarIn, 1, NULL, NULL) == 0 ;
    }
}

- (void)getSamples:(float *)dest seq:(uint32_t)seq channel:(uint32_t)channel blocks:(uint32_t)blocks {
    AIOTRX_SHARED_MEMORY *driverMem = self->drv->driverMem ;
    size_t chidx = channel ;
    const double g = 1.0 / 0x7fffffff ;

    if (chidx < driverMem->num_inputs) {
        AIO_CHANNEL *sch = driverMem->input_channels + chidx ;
        while(blocks) {
            int32_t *src = sch->blocks[seq % MAX_AIO_BLOCKS].samples ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            *dest++ = (float)(g * *src++) ;
            seq++ ;
            blocks-- ;
        }
    } else {
        bzero(dest, sizeof(int32_t) * AIO_BLOCK_SZ * blocks) ;
    }
}

int limit(double lowerLimit, double upperLimit, double valueToConstrain) {
    return valueToConstrain < lowerLimit ? lowerLimit : (upperLimit < valueToConstrain ? upperLimit : valueToConstrain) ;
}

- (void)putSamples:(float *)src seq:(uint32_t)seq channel:(uint32_t)channel blocks:(uint32_t)blocks {
    AIOTRX_SHARED_MEMORY *driverMem = self->drv->driverMem ;
    AIO_OUTPUT_BY_CLIENT *client_output = self->drv->client_output;
    size_t chidx = channel ;
    const double maxVal = (double) 0x7fffffff ;
    
    if (chidx >= driverMem->num_outputs) {
        return ;
    }
    
    AIO_CHANNEL *dch = client_output->output_channels + chidx ;
    while(blocks) {
        int32_t *dest = dch->blocks[seq % MAX_AIO_BLOCKS].samples ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        *dest++ = limit(-maxVal, maxVal, maxVal * (double)(*src++)) ;
        seq++ ;
        blocks-- ;
    }
}

- (void)executor:(id)argument {
    NSLog(@"FastAioThread start") ;
    
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

    AIOTRX_SHARED_MEMORY *driverMem = drv->driverMem ;
    uint32_t ClientN = drv->ClientN ;
    uint32_t inputSEQ;
    uint32_t outputSEQ ;
    uint32_t BlocksNum = self.engineBufferSize / AIO_BLOCK_SZ ;

    [self startTRX] ;
    
    inputSEQ = atomic_load(&driverMem->readySEQ) ;
    
    while (self->thread && !self->thread.cancelled) {
        
        if (pric == 0) {
            if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&ttcp, THREAD_TIME_CONSTRAINT_POLICY_COUNT) != 0) {
                NSLog(@"FastAioThread set_realtime failed") ;
            }
        }

        if (pric++ >= 1000) {
            pric = 0 ;
        }

        if ([self configurationChanged]) {
            inputSEQ = atomic_load(&driverMem->readySEQ) ;
        }
        
        if ([self waitRXseq:(inputSEQ + BlocksNum)]) {
            outputSEQ = inputSEQ + BlocksNum * 2 -1 ;
            
            for (UInt32 channel = 0;  channel < driverMem->num_inputs; channel++) {
                bool got = false ;

                for (id<FastAioThreadDelegate> dlg in self.delegates) {
                    if ([dlg listenForChannel:channel]) {
                        if (!got) {
                            [self getSamples:self->aioBuffer seq:inputSEQ channel:channel blocks:BlocksNum] ;
                            got = true ;
                        }
                        
                        [dlg inputSamples:BlocksNum * AIO_BLOCK_SZ buffer:self->aioBuffer channel:channel] ;
                    }
                }
            }
            
            // 1. Cock 4x-mutex
            if (!self->thread || self->thread.cancelled) {
                break ;
            }

            [self->mutex reset] ;

            // 2. Signal all bus threads
            for (id<FastAioThreadDelegate> dlg in self.delegates) {
                [dlg preloopSamples:BlocksNum * AIO_BLOCK_SZ] ;
                [dlg wakeup] ;
            }
            
            // 3. Wait for 4x-mutex

            while (self->thread && !self->thread.cancelled && [self->mutex wait] > 0) {}

            if (!self->thread || self->thread.cancelled) {
                break ;
            }

            for (UInt32 channel = 0;  channel < driverMem->num_outputs; channel++) {
                bool put = false ;
                bool sum = false ;
                
                for (id<FastAioThreadDelegate> dlg in self.delegates) {
                    bool postloop = false ;
                    if ([dlg outToChannel:channel]) {
                        if (!postloop) {
                            [dlg postloopSamples:BlocksNum * AIO_BLOCK_SZ] ;
                            postloop = true ;
                        }
                        [dlg outputSamples:BlocksNum * AIO_BLOCK_SZ buffer:self->aioBuffer channel:channel sum:sum] ;
                        put = true ;
                        sum = true ;
                    }
                }
                
                if (put) {
                    [self putSamples:self->aioBuffer seq:outputSEQ channel:channel blocks:BlocksNum] ;
                }
            }

            //Now notify driver about output data ready
            atomic_store(&driverMem->clients_output[ClientN].readySEQ, outputSEQ + BlocksNum) ; //New blocks now availble for output (from outputSEQ to outputSEQ+BlockNum)
            atomic_store(&driverMem->clients_output[ClientN].need_wait, 1) ; //Enable driver waiting for output data

            inputSEQ += BlocksNum ; //Next SEQ
        } else {
            NSLog(@"*** No data (timeout) ***") ;
            inputSEQ = atomic_load(&driverMem->readySEQ) ;
        }
    }
    
    [self stopTRX] ;

    self->running = NO ;

    NSLog(@"FastAioThread stop") ;
}

- (void)didFinishLoopBus {
    [self->mutex signal] ;
}

@end
