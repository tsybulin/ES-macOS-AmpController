//
//  ES_Event.c
//  AudioIO24
//
//  Created by Pavel Tsybulin on 2/14/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#include "ES_Event.h"

#include <stdlib.h>

ESEvent ESEvent_Init(void) {
    ESEvent event = malloc(sizeof(esevent_t)) ;

    pthread_mutex_init(&event->mutex, 0) ;
    pthread_cond_init(&event->cond, 0) ;
    event->triggered = false ;
    
    return event ;
}

void ESEvent_Set(ESEvent event) {
    pthread_mutex_lock(&event->mutex) ;
    event->triggered = true ;
    pthread_cond_signal(&event->cond) ;
    pthread_mutex_unlock(&event->mutex) ;
}

void ESEvent_Clear(ESEvent event) {
    pthread_mutex_lock(&event->mutex) ;
    event->triggered = false ;
    pthread_mutex_unlock(&event->mutex) ;
}

void ESEvent_Wait(ESEvent event) {
    pthread_mutex_lock(&event->mutex) ;
    while (!event->triggered) {
        pthread_cond_wait(&event->cond, &event->mutex) ;
    }
    pthread_mutex_unlock(&event->mutex) ;
}

void ESEvent_WaitAndReset(ESEvent event) {
    pthread_mutex_lock(&event->mutex) ;
    while (!event->triggered) {
        pthread_cond_wait(&event->cond, &event->mutex) ;
    }
    event->triggered=false;
    pthread_mutex_unlock(&event->mutex) ;
}

void ESEvent_Free(ESEvent event) {
    free(event) ;
}
