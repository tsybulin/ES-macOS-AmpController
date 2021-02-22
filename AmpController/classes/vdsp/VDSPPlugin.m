//
//  VDSPPlugin.m
//  AIOController
//
//  Created by Pavel Tsybulin on 13.07.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "VDSPPlugin.h"

@interface VDSPPlugin ()

@property (strong) NSUUID *uuid ;

@end

@implementation VDSPPlugin

- (instancetype)init {
    if (self = [super init]) {
        self.uuid = [NSUUID UUID] ;
    }
    return self;
}

- (void)dealloc {
    if (self.plugin) {
//        free(self.component) ;
    }
}

@end
