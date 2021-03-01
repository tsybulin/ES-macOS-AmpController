//
//  ESView.m
//  AIOController
//
//  Created by Pavel Tsybulin on 3/18/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "ESView.h"

IB_DESIGNABLE
@implementation ESView

- (void)prepareForInterfaceBuilder {
    [super prepareForInterfaceBuilder] ;
}

- (void)drawRect:(NSRect)dirtyRect {
    if (self.backgroundColor) {
        [self.backgroundColor setFill] ;
        NSRectFill(dirtyRect) ;
    }
        
    
    [super drawRect:dirtyRect] ;
}

@end
