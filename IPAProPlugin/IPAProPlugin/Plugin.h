//
//  Plugin.h
//  AudioIO24
//
//  Created by Pavel Tsybulin on 2/5/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#ifndef Plugin_h
#define Plugin_h

#include <stdio.h>
#include <MacTypes.h>

UInt32 IPAProPlugin_GetDeviceOutDelay(void) ;
void IPAProPlugin_SetDeviceOutDelay(UInt32 outDelay) ;

void IPAProPlugin_GetDeviceIP(size_t idx, char *buffer, size_t len) ;

#endif
