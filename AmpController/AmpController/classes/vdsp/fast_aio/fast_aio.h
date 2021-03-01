//
//  fast_aiio.h
//  ESController
//
//  Created by Pavel Tsybulin on 05.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#ifndef fast_aio_h
#define fast_aio_h

#include "aiotrx_interface.h"

#include <semaphore.h>

#define FAST_AIO_DRV_NAME_ROOT_US "FastAIO."

typedef struct {
    AIOTRX_SHARED_MEMORY    *driverMem;
    AIO_OUTPUT_BY_CLIENT    *client_output;
    io_service_t            service;
    io_connect_t            driverConnection;
    int32_t                 ClientN;
    int                     ctl_sock;

    bool srv_is_US; //True if server is in user space
    //User-space server vars
    sem_t *WaitLock[2];
    //bool wait_locked;
    volatile atomic_long watchdog_timer;
    pthread_t watchdog_ptid;
    volatile bool watchdog_thread_done;
    //volatile atomic_ulong watchdog_abort;
} FAST_AIO_DRV ;

#endif
