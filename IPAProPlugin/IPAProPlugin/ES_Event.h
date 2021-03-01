//
//  ES_Event.h
//  AudioIO24
//
//  Created by Pavel Tsybulin on 2/14/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#ifndef ES_Event_h
#define ES_Event_h

#include <stdbool.h>
#include <pthread.h>

struct esevent_t {
    pthread_mutex_t mutex ;
    pthread_cond_t cond ;
    bool triggered ;
} ;

typedef struct esevent_t esevent_t ;
typedef struct esevent_t* ESEvent  ;

ESEvent ESEvent_Init(void) ;
void    ESEvent_Set(ESEvent event) ;
void    ESEvent_Clear(ESEvent event) ;
void    ESEvent_Wait(ESEvent event) ;
void    ESEvent_WaitAndReset(ESEvent event);
void    ESEvent_Free(ESEvent event) ;

#endif /* ES_Event_h */
