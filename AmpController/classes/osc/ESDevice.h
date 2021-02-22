//
//  ESDevice.h
//  AIOController
//
//  Created by Pavel Tsybulin on 2/23/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#define DEFAULT_DEVICE_PORT 44528
#define INDICATOR_MIN_LVL -60
#define INDICATOR_MAX_LVL 0

#define IO_GAIN_THRESHOLD_SEC 0.3
#define IO_GAIN_THRESHOLD_RESEND 0.3
#define IO_GAIN_THRESHOLD_MAIN 1.0

#define DEVICE_OUT_TIMEOUT_MS 0.005

typedef NS_ENUM(NSInteger, ESIOKind) {
    kIOKindOutput = 0,
    kIOKindInput = 1,
    kIOKindHole = 9
} ;

@interface ESInputOutput : NSObject <NSCopying>

@property (nonatomic, readonly) NSInteger idx ;
@property (nonatomic, strong) NSString *title ;
@property (nonatomic, readonly) ESIOKind kind ;
@property (nonatomic) float minDbm, maxDbm, gain, gainDbm, level ;
@property (nonatomic) NSUInteger mapping ;
@property (nonatomic) BOOL phantomPower, overflow ;
@property (nonatomic) NSDate *lastDeviceGainTime ;
@property (nonatomic) NSDate *lastHumanGainTime ;
@property (nonatomic) float lastHumanGain ;
@property (nonatomic) BOOL autogain ;

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithIdx:(NSInteger)idx title:(NSString *)title kind:(ESIOKind)kind NS_DESIGNATED_INITIALIZER ;
- (BOOL)stereo ;
- (void)setStereo:(BOOL)val ;
- (BOOL)main ;
- (void)setMain:(BOOL)val ;
- (BOOL)mute ;
- (void)setMute:(BOOL)val ;
- (BOOL)solo ;
- (void)setSolo:(BOOL)val ;

@end

@interface ESDevice : NSObject

@property (nonatomic, strong) NSString *title ;
@property (nonatomic, strong) NSString *ip ;
@property (nonatomic, strong) NSString *soundAddress ;
@property (nonatomic, strong) NSString *clientAddress ;
@property (nonatomic, strong) NSString *deviceType ;
@property (nonatomic, strong) NSString *audioStreamMode ;
@property (nonatomic, readonly) NSInteger port ;
@property (nonatomic) BOOL master, configured ;
@property (nonatomic) NSInteger syncFrequency ;
@property (nonatomic) NSNumber *fwRevision ;
@property (nonatomic) NSArray<ESInputOutput *> *inputs ;
@property (nonatomic) NSArray<ESInputOutput *> *outputs ;
@property (nonatomic, strong) NSDate *timestamp ;
@property (nonatomic, strong) NSDate *lastOutDate ;
@property BOOL visible ;
@property (nonatomic) int confTimeout ;
@property (strong) NSNumber *configPosition ;

- (instancetype)init NS_UNAVAILABLE ;
- (instancetype)initWithTitle:(NSString *)title ip:(NSString *)ip port:(NSInteger)port NS_DESIGNATED_INITIALIZER ;
- (void)setTotalIO:(ESIOKind)kind total:(NSInteger)count ;
- (ESInputOutput *)io:(NSInteger)idx kind:(ESIOKind)kind ;
- (NSUInteger)iocount:(ESIOKind)kind ;
- (NSUInteger)inpcount ;
- (NSUInteger)outcount ;
- (NSString *)syncMode ;

@end

NS_ASSUME_NONNULL_END
