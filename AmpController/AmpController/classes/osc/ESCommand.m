//
//  ESCommand.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/12/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "ESCommand.h"

@interface ESCommand ()

@property (nonatomic, assign) NSString *path ;

@end

@implementation ESCommand

- (instancetype)initWithPath:(NSString *)path {
    if (self= [super init] ) {
        self.path = path ;
    }
    return self ;
}

- (nonnull id)copyWithZone:(nullable NSZone *)zone {
    ESCommand *command = [[ESCommand alloc] initWithPath:self.path] ;
    return command ;
}

@end

@implementation ESCBroadcast

+ (instancetype)command {
    return [[ESCBroadcast alloc] initWithPath:@"/zz"] ;
}

@end

@implementation ESCConfigurationRequest

+ (instancetype)command {
    return [[ESCConfigurationRequest alloc] initWithPath:@"/cr"] ;
}

@end

@implementation ESCInputConfigurationRequest

+ (instancetype)command {
    return [[ESCInputConfigurationRequest alloc] initWithPath:@"/ir"] ;
}

@end

@implementation ESCOutputConfigurationRequest

+ (instancetype)command {
    return [[ESCOutputConfigurationRequest alloc] initWithPath:@"/or"] ;
}

@end

@implementation ESCGain

+ (instancetype)commandForIdx:(NSUInteger)idx kind:(ESIOKind)kind {
    switch (kind) {
        case kIOKindInput:
            return [[ESCGain alloc] initWithPath:[NSString stringWithFormat:@"/ig/%lu", idx]] ;

        case kIOKindOutput:
            return [[ESCGain alloc] initWithPath:[NSString stringWithFormat:@"/og/%lu", idx]] ;

        default:
            return nil ;
    }
}

@end

@implementation ESCInputOverflow

+ (instancetype)command {
    return [[ESCInputOverflow alloc] initWithPath:@"/if"] ;
}

@end

@implementation ESCInputAutogain

+ (instancetype)commandForIdx:(NSUInteger)idx {
    return [[ESCInputAutogain alloc] initWithPath:[NSString stringWithFormat:@"/ia/%lu", idx]] ;
}

@end

@implementation ESCInputPhantomPower

+ (instancetype)commandForIdx:(NSUInteger)idx {
    return [[ESCInputPhantomPower alloc] initWithPath:[NSString stringWithFormat:@"/ip/%lu", idx]] ;
}

@end

@implementation ESCSyncFrequency

+ (instancetype)command {
    return [[ESCSyncFrequency alloc] initWithPath:@"/sf"] ;
}

@end

@implementation ESCInputMapping

+ (instancetype)commandForIdx:(NSUInteger)idx {
    return [[ESCInputMapping alloc] initWithPath:[NSString stringWithFormat:@"/im/%lu", idx]] ;
}

@end

@implementation ESCOutputMapping

+ (instancetype)commandForIdx:(NSUInteger)idx {
    return [[ESCOutputMapping alloc] initWithPath:[NSString stringWithFormat:@"/om/%lu", idx]] ;
}

@end

