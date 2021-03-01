//
//  ESDevice.m
//  AIOController
//
//  Created by Pavel Tsybulin on 2/23/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "ESDevice.h"

@interface ESInputOutput ()

@property (nonatomic) NSInteger idx ;
@property (nonatomic) ESIOKind kind ;

@end

@interface ESDevice ()

@property (nonatomic) NSInteger port ;

@end

@implementation ESInputOutput

- (instancetype)initWithIdx:(NSInteger)idx title:(NSString *)title kind:(ESIOKind)kind {
    if (self = [super init]) {
        self.idx = idx ;
        self.title = title ;
        self.kind = kind ;
        self.minDbm = -60 ;
        self.maxDbm = 0 ;
        self.gain = 0 ;
        self.gainDbm = self.minDbm ;
        self.level = self.minDbm ;
        self.phantomPower = NO ;
        self.overflow = NO ;
        self.mapping = 0 ;
        self.autogain = NO ;
    }
    return self;
}

- (BOOL)stereo {
    return (self.mapping & 0b1) > 0 ;
}

- (void)setStereo:(BOOL)val {
    if (val) {
        self.mapping |= 0b01 ;
    } else {
        self.mapping &= 0b1110 ;
    }
}

- (BOOL)main {
    return self.kind == kIOKindOutput && (self.mapping & 0b10) > 0 ;
}

- (void)setMain:(BOOL)val {
    if (val) {
        self.mapping |= 0b10 ;
    } else {
        self.mapping &= 0b1101 ;
    }
}

- (BOOL)mute {
    return (self.mapping & 0b100) > 0 ;
}

- (void)setMute:(BOOL)val {
    if (val) {
        self.mapping |= 0b100 ;
    } else {
        self.mapping &= 0b1011 ;
    }
}

- (BOOL)solo {
    return (self.mapping & 0b1000) > 0 ;
}

- (void)setSolo:(BOOL)val {
    if (val) {
        self.mapping |= 0b1000 ;
    } else {
        self.mapping &= 0b0111 ;
    }
}

- (instancetype)copyWithZone:(nullable NSZone *)zone {
    ESInputOutput *io = [[ESInputOutput alloc] initWithIdx:self.idx title:self.title kind:self.kind] ;
    io.minDbm = self.minDbm ;
    io.maxDbm = self.maxDbm ;
    io.gain = self.gain ;
    io.gainDbm = self.gainDbm ;
    io.level = self.level ;
    io.phantomPower = self.phantomPower ;
    io.overflow = self.overflow ;
    io.mapping = self.mapping ;

    return io ;
}

@end

@implementation ESDevice

- (instancetype)initWithTitle:(NSString *)title ip:(NSString *)ip port:(NSInteger)port {
    if (self = [super init]) {
        self.title = title ;
        self.soundAddress = @"0.0.0.0" ;
        self.clientAddress = @"0.0.0.0" ;
        self.ip = ip ;
        self.port = port ;
        self.master = NO ;
        self.syncFrequency = 44100 ;
        self.timestamp = [NSDate date] ;
        
        self.inputs = [[NSMutableArray alloc] initWithObjects:[[ESInputOutput alloc] initWithIdx:0 title:@"INP 0" kind:kIOKindHole], nil] ;
        self.outputs = [[NSMutableArray alloc] initWithObjects:[[ESInputOutput alloc] initWithIdx:0 title:@"OUT 0" kind:kIOKindHole], nil] ;
        self.configured = NO ;
        
        self.lastOutDate = [NSDate date] ;
        self.visible = NO ;
        self.confTimeout = 0 ;
        self.configPosition = [NSNumber numberWithInt:666] ;
    }
    return self ;
}

- (void)setTotalIO:(ESIOKind)kind total:(NSInteger)count {
    if (kind == kIOKindInput) {
        if (count < self.inputs.count) {
            return ;
        }
        
        for (NSInteger i = 1; i <= count; i++) {
            [((NSMutableArray *)self.inputs) addObject:[[ESInputOutput alloc] initWithIdx:i title:[NSString stringWithFormat:@"INP %lu", i] kind:kind]] ;
        }
    } else if (kind == kIOKindOutput) {
        if (count < self.outputs.count) {
            return ;
        }

        for (NSInteger i = 1; i <= count; i++) {
            [((NSMutableArray *)self.outputs) addObject:[[ESInputOutput alloc] initWithIdx:i title:[NSString stringWithFormat:@"OUT %lu", i] kind:kind]] ;
        }
    }
}

- (ESInputOutput *)io:(NSInteger)idx kind:(ESIOKind)kind {
    switch (kind) {
        case kIOKindInput:
            return self.inputs[idx] ;
            
        case kIOKindOutput:
            return self.outputs[idx] ;

        default:
            return nil ;
    }
}

- (NSUInteger)iocount:(ESIOKind)kind {
    switch (kind) {
        case kIOKindInput:
            return self.inputs.count ;
            
        case kIOKindOutput:
            return self.outputs.count ;
            
        default:
            return 0 ;
    }
}

- (NSUInteger)inpcount {
    return [self iocount:kIOKindInput] -1 ;
}

- (NSUInteger)outcount {
    return [self iocount:kIOKindOutput] -1 ;
}

- (NSString *)syncMode {
    return self.master ?  @"Master" : @"Slave" ;
}

@end
