//
//  AppDelegate.m
//  AmpController
//
//  Created by Pavel Tsybulin on 17.02.2021.
//

#import "AppDelegate.h"
#import "ESManager.h"
#import "VDSPManager.h"

@interface AppDelegate () {
    ESManager *mgr ;
    VDSPManager *vmgr ;
}


@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    self->mgr = ESManager.sharedManager;
    [self->mgr start] ;
    self->vmgr = VDSPManager.sharedManager ;
}


- (void)applicationWillTerminate:(NSNotification *)aNotification {
    [self->vmgr stop] ;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

@end
