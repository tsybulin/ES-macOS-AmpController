//
//  ESManager.m
//  AIOController
//
//  Created by Pavel Tsybulin on 2/23/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "ESManager.h"

#import "F53OSC.h"
#import "ESCommand.h"
#import "CommandHandler.h"

@interface ESManager () <F53OSCPacketDestination> {
    F53OSCServer *server ;
    F53OSCClient *client ;
    
    NSMutableDictionary<NSString*, ESDevice*> *devices ;
    NSDictionary<ESCommand*, CommandHandler*> *handlers ;
    NSOperationQueue *tq, *cq ;
    NSMutableArray<id<DevicesObserver>> *devicesObservers ;
    NSMutableArray<id<DeviceObserver>> *deviceObservers ;
}

@end

@implementation ESManager

- (instancetype)init {
    if (self = [super init]) {
        server = [[F53OSCServer alloc] init] ;
        server.port = DEFAULT_SERVER_PORT ;
        server.delegate = self ;
        client = [[F53OSCClient alloc] init] ;
        devices = [NSMutableDictionary dictionary] ;
        
        DeviceConfigurationHandler *dch = [[DeviceConfigurationHandler alloc] init] ;
        IOConfigurationHandler *ioch = [[IOConfigurationHandler alloc] init] ;
        MeterHandler *mh = [[MeterHandler alloc] init] ;
        
        handlers = [NSDictionary dictionaryWithObjectsAndKeys:[
                    [BroadcastHandler alloc] init], [ESCBroadcast command],
                    dch, [[ESCommand alloc] initWithPath:@"/dn"],
                    dch, [[ESCommand alloc] initWithPath:@"/sa"],
                    dch, [[ESCommand alloc] initWithPath:@"/ac"],
                    dch, [[ESCommand alloc] initWithPath:@"/rv"],
                    dch, [[ESCommand alloc] initWithPath:@"/dt"],
                    dch, [[ESCommand alloc] initWithPath:@"/sf"],
                    dch, [[ESCommand alloc] initWithPath:@"/sm"],
                    dch, [[ESCommand alloc] initWithPath:@"/am"],
                    dch, [[ESCommand alloc] initWithPath:@"/ti"],
                    dch, [[ESCommand alloc] initWithPath:@"/to"],

                    ioch, [[ESCommand alloc] initWithPath:@"/ib/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/it/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/ig/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/ic/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/ip/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/ia/"],

                    ioch, [[ESCommand alloc] initWithPath:@"/ob/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/ot/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/og/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/oc/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/im/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/om/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/in/"],
                    ioch, [[ESCommand alloc] initWithPath:@"/on/"],

                    mh, [[ESCommand alloc] initWithPath:@"/il/"],
                    mh, [[ESCommand alloc] initWithPath:@"/ol/"],
                    mh, [[ESCommand alloc] initWithPath:@"/if"],
                    mh, [[ESCommand alloc] initWithPath:@"/of"],

                    nil] ;
        
        tq = [[NSOperationQueue alloc] init] ;
        tq.name = @"DeviceTimeoutQueue" ;
        tq.maxConcurrentOperationCount = 1 ;
        tq.qualityOfService = NSQualityOfServiceUserInitiated ;
        
        cq = [[NSOperationQueue alloc] init] ;
        cq.name = @"DeviceTimeoutQueue" ;
        cq.maxConcurrentOperationCount = 1 ;
        cq.qualityOfService = NSQualityOfServiceUserInitiated ;

        devicesObservers = [NSMutableArray array] ;
        deviceObservers = [NSMutableArray array] ;
    }
    
    return self ;
}

- (void)setupTimeout {
    NSBlockOperation *op = [[NSBlockOperation alloc] init] ;
    __weak NSBlockOperation *cop = op ;
    
    [op addExecutionBlock:^{
        if (cop.cancelled) {
            return ;
        }
        
        NSDate *now = [NSDate date] ;
        
        for (NSString *ip in self->devices.allKeys) {
            ESDevice *device = self->devices[ip] ;
            if ([now timeIntervalSinceDate:device.timestamp] > 5) {
                [self->devices removeObjectForKey:ip] ;
                
                for (id<DevicesObserver> delegate in self->devicesObservers) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [delegate deviceDidLostManager:self device:device] ;
                    }) ;
                }
            }
        }
        
        int count = 0 ;
        do {
            NSThread.currentThread.name = @"ESManager.timeoutThread" ;
            [NSThread sleepForTimeInterval:0.1] ;
            count++ ;
        } while (!op.cancelled && count < 50) ;
    }] ;
    
    op.completionBlock = ^{
        if (!cop.cancelled) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setupTimeout] ;
            }) ;
        }
    } ;
    
    [tq addOperation:op] ;
}

+ (instancetype)sharedManager {
    static ESManager *sharedInstance = nil ;
    
    @synchronized (self) {
        if (!sharedInstance) {
            sharedInstance = [[ESManager alloc] init] ;
        }
        
        return sharedInstance ;
    }
}

- (void)start {
    [server startListening] ;
    [self setupTimeout] ;
}

- (void)stop {
    [server stopListening] ;
    [devices removeAllObjects] ;
    [tq cancelAllOperations] ;
}

- (ESDevice *)device:(NSString *)ip {
    return devices[ip] ;
}

- (NSArray<ESDevice *>*)devices {
    return [devices allValues] ;
}

- (void)addDevice:(ESDevice *)device {
    devices[device.ip] = device ;
}

- (void)replaceIp:(NSString *)ip forDevice:(ESDevice *)device {
    [devices removeObjectForKey:device.ip] ;
    device.ip = ip ;
    devices[ip] = device ;
}


- (void)notifyDeviceDidFound:(ESDevice *)device {
    for (id<DevicesObserver> delegate in self->devicesObservers) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate deviceDidFoundManager:self device:device] ;
        }) ;
    }
}

- (void)notifyDeviceDidChangeName:(ESDevice *)device {
    for (id<DevicesObserver> delegate in self->devicesObservers) {
        if ([delegate respondsToSelector:@selector(deviceDidChangeNameManager:device:)]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate deviceDidChangeNameManager:self device:device] ;
            }) ;
        }
    }
}

- (void)notifyDeviceDidChangeClientAddress:(ESDevice *)device {
    for (id<DeviceObserver> delegate in self->deviceObservers) {
        if ([delegate respondsToSelector:@selector(clientAddressDidChangeManager:device:)]) {
            if ([delegate containsDevice:device]) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [delegate clientAddressDidChangeManager:self device:device] ;
                }) ;
            }
        }
    }
}

- (void)notifyDeviceDidChangeIOName:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DevicesObserver> delegate in self->devicesObservers) {
        if ([delegate respondsToSelector:@selector(deviceDidChangeIONameManager:device:io:)]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate deviceDidChangeIONameManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)addDevicesObserver:(id<DevicesObserver>)observer {
    if ([devicesObservers containsObject:observer]) {
        return ;
    }
    
    [devicesObservers addObject:observer] ;

    for (ESDevice *device in devices.allValues) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [observer deviceDidFoundManager:self device:device] ;
        }) ;
    }
}

- (void)addDeviceObserver:(id<DeviceObserver>)observer {
    if ([deviceObservers containsObject:observer]) {
        return ;
    }
    
    [deviceObservers addObject:observer] ;
}

- (void)removeDevicesObserver:(id<DevicesObserver>)observer {
    [devicesObservers removeObject:observer] ;
}

- (void)removeDeviceObserver:(id<DeviceObserver>)observer {
    [deviceObservers removeObject:observer] ;
}

- (void)notifyLevelDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate levelDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifyGainDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate gainDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifyGainDbmDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate gainDbmDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifyOverflowDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate overflowDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifyPhantomPowerDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate phantomPowerDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifyMappingDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate mappingDidChangeManager:self device:device io:io] ;
            }) ;
        }
    }
}

- (void)notifySyncFreqDidChangeDevice:(ESDevice *)device {
    for (id<DeviceObserver> delegate in deviceObservers) {
        if ([delegate containsDevice:device]) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate syncFreqDidChangeManager:self device:device] ;
            }) ;
        }
    }
}


#pragma mark - Commands

- (void)sendMessage:(F53OSCMessage *)message toHost:(NSString *)host onPort:(UInt16)port {
    [cq addOperationWithBlock:^{
        ESDevice *device = [self device:host] ;
        if (device) {
            NSTimeInterval loi = [[NSDate date] timeIntervalSinceDate:device.lastOutDate] ;
            if (loi < DEVICE_OUT_TIMEOUT_MS) {
                double delay = DEVICE_OUT_TIMEOUT_MS - loi ;
//                long nsec = delay * 1000000 ;
//                NSLog(@"out loi: %f delay: %f nsec: %ld", loi, delay, nsec) ;
                struct timespec ts ;
                ts.tv_sec = 0 ;
                ts.tv_nsec = delay * 1000000 ;
                nanosleep(&ts, NULL) ;
            }
            device.lastOutDate = [NSDate date] ;
        }
//        if ([message.addressPattern compare:@"/zz"] != kCFCompareEqualTo) {
//            NSLog(@"SendPacket %@ arg:%@ to:%@", message.addressPattern, message.arguments, host) ;
//        }
        [self->client sendPacket:message toHost:host onPort:port] ;


    }] ;
    //NSLog(@"qsz=%lu", self->cq.operationCount) ;
}

#pragma mark - <F53OSCPacketDestination>

- (void)takeMessage:(F53OSCMessage *)message {
    __block BOOL handled = NO ;
    
    if ([message.replySocket.host hasPrefix:@"::"]) {
        return ;
    }

    [handlers enumerateKeysAndObjectsUsingBlock:^(ESCommand *command, CommandHandler *handler, BOOL *stop) {
        if ([message.addressPattern hasPrefix:command.path]) {
            *stop = true ;
            handled = YES ;
//            if (![handler isKindOfClass:MeterHandler.class]) {
//                NSLog(@"handle message %@ %@", handler.className, command.path) ;
//            }
            [handler handleMessage:self command:command message:message] ;
        } else {
            *stop = false ;
        }
    }] ;
    
    if (!handled) {
        NSLog(@"ESManager.takeMessage Unhandled message %@", message.addressPattern) ;
    }
}

@end
