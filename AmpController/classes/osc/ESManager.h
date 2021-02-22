//
//  ESManager.h
//  AIOController
//
//  Created by Pavel Tsybulin on 2/23/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

#import "ESDevice.h"

#define DEFAULT_SERVER_PORT 44529

NS_ASSUME_NONNULL_BEGIN

@class ESManager ;
@class F53OSCMessage ;

@protocol DevicesObserver <NSObject>

- (void)deviceDidFoundManager:(ESManager *)manager device:(ESDevice *)device ;
- (void)deviceDidLostManager:(ESManager *)manager device:(ESDevice *)device ;

@optional

- (void)deviceDidChangeNameManager:(ESManager *)manager device:(ESDevice *)device ;
- (void)deviceDidChangeIONameManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;

@end

@protocol DeviceObserver <NSObject>

- (BOOL)containsDevice:(ESDevice *)device ;
- (void)levelDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)gainDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)gainDbmDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)overflowDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)phantomPowerDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)syncFreqDidChangeManager:(ESManager *)manager device:(ESDevice *)device ;
- (void)mappingDidChangeManager:(ESManager *)manager device:(ESDevice *)device io:(ESInputOutput *)io ;

@optional
- (void)clientAddressDidChangeManager:(ESManager *)manager device:(ESDevice *)device ;

@end

@interface ESManager : NSObject

+ (instancetype)sharedManager ;
- (ESDevice *)device:(NSString *)ip ;
- (NSArray<ESDevice *>*)devices ;
- (void)addDevice:(ESDevice *)device ;
- (void)replaceIp:(NSString *)ip forDevice:(ESDevice *)device ;
- (void)start ;
- (void)stop ;
- (void)sendMessage:(F53OSCMessage *)message toHost:(NSString *)host onPort:(UInt16)port ;
- (void)addDevicesObserver:(id<DevicesObserver>)observer ;
- (void)addDeviceObserver:(id<DeviceObserver>)observer ;
- (void)removeDevicesObserver:(id<DevicesObserver>)observer ;
- (void)removeDeviceObserver:(id<DeviceObserver>)observer ;

- (void)notifyDeviceDidFound:(ESDevice *)device ;
- (void)notifyDeviceDidChangeName:(ESDevice *)device ;
- (void)notifyDeviceDidChangeIOName:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifyDeviceDidChangeClientAddress:(ESDevice *)device ;
- (void)notifyLevelDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifyGainDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifyGainDbmDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifyOverflowDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifyPhantomPowerDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;
- (void)notifySyncFreqDidChangeDevice:(ESDevice *)device ;
- (void)notifyMappingDidChangeDevice:(ESDevice *)device io:(ESInputOutput *)io ;

@end

NS_ASSUME_NONNULL_END
