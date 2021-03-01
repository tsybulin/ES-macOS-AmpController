//
//  fast_aio_drv.hpp
//  UserSpaceClient
//
//  Created by Dmytro Oparin on 3/26/20.
//

#ifndef fast_aio_drv_hpp
#define fast_aio_drv_hpp

#include <IOKit/IOKitLib.h>
#include "aiotrx_interface.h"

#include <semaphore.h>
#include <pthread.h>



#define FAST_AIO_DRV_NAME_ROOT "com_es_fastaio_"
#define FAST_AIO_DRV_NAME_ROOT_DOT "com.es.fastaio."
#define FAST_AIO_DRV_NAME_LEN (15)

#define FAST_AIO_DRV_NAME_ROOT_US "FastAIO."


typedef struct
{
    AIOTRX_SHARED_MEMORY *driverMem;
    AIO_OUTPUT_BY_CLIENT *client_output;
    io_service_t        service;
    io_connect_t    driverConnection;
    int32_t ClientN;
    int ctl_sock;
    bool srv_is_US; //True if server is in user space
    //User-space server vars
    sem_t *WaitLock[2];
    bool wait_locked;
    volatile uint32_t watchdog_timer;
    pthread_t watchdog_ptid;
    volatile bool watchdog_thread_done;
    volatile bool watchdog_abort;
}FAST_AIO_DRV;

kern_return_t           OpenDriver(FAST_AIO_DRV *drv, const io_name_t name);
void                    CloseDriver(FAST_AIO_DRV *drv);

kern_return_t           StartTRX(FAST_AIO_DRV *drv);
kern_return_t           StopTRX(FAST_AIO_DRV *drv);

unsigned long IsConfigurationChanged(FAST_AIO_DRV *drv);

kern_return_t           WaitRXseq(FAST_AIO_DRV *drv, uint32_t seq);
void                    GetSamples(FAST_AIO_DRV *drv, int32_t *dest, uint32_t SEQ, uint32_t ch_n, uint32_t blocks);
void                    PutSamples(FAST_AIO_DRV *drv, uint32_t SEQ, uint32_t ch_n, int32_t *src, uint32_t blocks);


#endif /* fast_aio_drv_hpp */
