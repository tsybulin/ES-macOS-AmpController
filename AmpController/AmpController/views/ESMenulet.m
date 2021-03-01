//
//  ESMenulet.m
//  AIOController
//
//  Created by Pavel Tsybulin on 6/3/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#import "ESMenulet.h"

#import "ViewController.h"

@interface ESMenulet () {
    NSStatusItem *statusItem ;
}

@end

@implementation ESMenulet

- (void)awakeFromNib {
    [super awakeFromNib] ;
    
    statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength] ;
    NSStatusBarButton *btn = statusItem.button ;
    btn.enabled = YES ;
    btn.title = @"" ;
    btn.toolTip = @"AmpController" ;
    btn.image = [NSImage imageNamed:@"menucon"] ;
    btn.target = self ;
    btn.action = @selector(menuletDidClick:) ;
}

- (IBAction)menuletDidClick:(id)sender {
    for (NSWindow *w in NSApp.windows) {
        if ([w.contentViewController isKindOfClass:ViewController.class]) {
            [w makeKeyAndOrderFront:self];
            break;
        }
    }
}

@end
