//
//  VDSPPlugin.h
//  AIOController
//
//  Created by Pavel Tsybulin on 13.07.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, PluginType) {
    kPluginTypeVST3 = 0,
    kPluginTypeAU = 1
} ;


@interface VDSPPlugin : NSObject

@property (strong, readonly) NSUUID *uuid ;
@property (strong) NSString *name ;
@property PluginType pluginType ;
@property void *plugin ;
@property (strong) NSString *path ;

@end

NS_ASSUME_NONNULL_END
