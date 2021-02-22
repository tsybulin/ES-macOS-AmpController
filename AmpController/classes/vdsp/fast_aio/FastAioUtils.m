//
//  FastAioUtils.m
//  AIOController
//
//  Created by Pavel Tsybulin on 05.10.2020.
//  Copyright © 2020 Pavel Tsybulin. All rights reserved.
//

#import "FastAioUtils.h"

#import <sys/socket.h>
#import <sys/kern_control.h>
#import <sys/sys_domain.h>
#import <sys/ioctl.h>
#import <pthread.h>
#import <sys/mman.h>

#define FAST_AIO_DRV_NAME_ROOT "com_es_fastaio_"
#define FAST_AIO_DRV_NAME_ROOT_DOT "com.es.fastaio."
#define FAST_AIO_DRV_NAME_LEN (15)

#define FAST_AIO_DRV_NAME "IPAPro"

@implementation FastAioUtils

+ (BOOL)openDriver:(FAST_AIO_DRV *)drv {
    CFDictionaryRef        matchingDict = NULL;
    io_iterator_t        iter = 0;
    kern_return_t        kr;
    io_name_t name2;
    drv->service=0;
    drv->driverConnection=0;
    drv->ctl_sock=-1;
    drv->driverMem=0;
    drv->client_output=0;
    drv->ClientN=-1;
    strcpy(name2, FAST_AIO_DRV_NAME_ROOT);
    strcat(name2, FAST_AIO_DRV_NAME);
    matchingDict = IOServiceMatching(name2);
    // Create an iterator for all IO Registry objects that match the dictionary
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
    
    if (kr != KERN_SUCCESS) {
        return NO ;
    }
    
    if((drv->service = IOIteratorNext(iter)) == 0) {
        // Release the iterator
        IOObjectRelease(iter);
        goto L_open_US;
    }
    
    // Release the iterator
    IOObjectRelease(iter);
    
    uint32_t type = 0;
    kr = IOServiceOpen(drv->service, mach_task_self(), type, &drv->driverConnection);
    if (kr != KERN_SUCCESS) {
        drv->driverConnection=0;
        [FastAioUtils closeDriver:drv] ;
        goto L_open_US;
    }
    
    [FastAioUtils getClientN:drv clientN:&drv->ClientN] ;

    if (drv->ClientN<0 || drv->ClientN>=MAX_CLIENTS) {
        NSLog(@"Incorrect client number!");
        [FastAioUtils closeDriver:drv] ;
        return NO ;
    }

    [FastAioUtils connectToCtl:drv name:FAST_AIO_DRV_NAME] ;

    mach_vm_address_t    theAddress;
    mach_vm_size_t        theSize;
    
    IOOptionBits memopt=kIOMapAnywhere;
    
    if ((kr=IOConnectMapMemory(drv->driverConnection, 1, mach_task_self(), &theAddress, &theSize, memopt))!=KERN_SUCCESS) {
        NSLog(@"IOConnectMapMemory fault, error = %d", kr) ;
        [FastAioUtils closeDriver:drv] ;
        return kr = KERN_SUCCESS ;
    }
    
    drv->driverMem = (AIOTRX_SHARED_MEMORY*)theAddress;
    drv->client_output = drv->driverMem->clients_output+drv->ClientN;
    
    return YES;

L_open_US:
    NSLog(@"Try to init user-space driver...");
    drv->srv_is_US=true;
    if (![FastAioUtils initSharedMemory:drv name:FAST_AIO_DRV_NAME]) {
        return NO;
    }
    if (![FastAioUtils initLocks:drv name:FAST_AIO_DRV_NAME]) {
        [FastAioUtils closeDriver:drv] ;
        return NO;
    }
    drv->client_output=drv->driverMem->clients_output+drv->ClientN;
    return YES;
}

+ (void)closeDriver:(FAST_AIO_DRV *)drv {
    if (drv->srv_is_US) {
        NSLog(@"User-space server cleanup...");
        if (drv->driverMem) {
            sem_t *sem;
            int32_t cn;
            cn=drv->ClientN;
            drv->driverMem->client_states[cn]=1;
            if (cn>=0) {
                drv->watchdog_thread_done=true;
                if (drv->watchdog_ptid) {
                    void *ret;
                    pthread_join(drv->watchdog_ptid,&ret);
                }
                sem=drv->WaitLock[0];
                drv->WaitLock[0]=0;
                if (sem)
                {
                    sem_post(sem);
                    sem_close(sem);
                }
                sem=drv->WaitLock[1];
                drv->WaitLock[1]=0;
                if (sem) {
                    sem_close(sem);
                }
                AIO_OUTPUT_BY_CLIENT *tx;
                tx=drv->client_output;
                tx->need_wait=0;
                bzero(tx->output_channels,sizeof(tx->output_channels));
                atomic_store(drv->driverMem->client_states+cn,0);
            }
            NSLog(@"Locks released!");
            void *addr;
            addr=drv->driverMem;
            drv->driverMem=NULL;
            munmap(addr, sizeof(AIOTRX_SHARED_MEMORY));
            NSLog(@"Shared memory released!");
        }

        return ;
    }
    
    if (drv->driverMem) {
        mach_vm_address_t    theAddress;
        theAddress=(mach_vm_address_t)drv->driverMem;
        IOConnectUnmapMemory(drv->driverConnection, 1, mach_task_self(), theAddress);
        drv->driverMem=0;
    }

    [FastAioUtils disconnectFromCtl:drv] ;

    if (drv->driverConnection) {
        IOServiceClose(drv->driverConnection);
        drv->driverConnection=0;
    }
    
    if (drv->service) {
        IOObjectRelease(drv->service);
        drv->service=0;
    }
}

+ (void)disconnectFromCtl:(FAST_AIO_DRV *)drv {
    if (drv->ctl_sock >= 0) {
        close(drv->ctl_sock) ;
    }
    drv->ctl_sock = -1 ;
}

+ (void)connectToCtl:(FAST_AIO_DRV *)drv name:(const io_name_t) name {
    struct ctl_info ctl_info;
    struct sockaddr_ctl sc;
    
    drv->ctl_sock = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (drv->ctl_sock < 0) {
        NSLog(@"Can't create PF_SYSTEM socket!");
        return ;
    }
    
    bzero(&ctl_info, sizeof(struct ctl_info));
    strcpy(ctl_info.ctl_name, FAST_AIO_DRV_NAME_ROOT_DOT);
    strcat(ctl_info.ctl_name, name);
    
    if (ioctl(drv->ctl_sock, CTLIOCGINFO, &ctl_info) == -1) {
        NSLog(@"Can't do ioctl(ctl_sock)!");
        close(drv->ctl_sock);
        drv->ctl_sock=-1;
        return;
    }
    
    bzero(&sc, sizeof(struct sockaddr_ctl));
    sc.sc_len = sizeof(struct sockaddr_ctl);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = SYSPROTO_CONTROL;
    sc.sc_id = ctl_info.ctl_id;
    sc.sc_unit = 0;
    
    if (connect(drv->ctl_sock, (struct sockaddr *)&sc, sizeof(struct sockaddr_ctl))) {
        NSLog(@"Can't connect ro ctl_sock!");
        close(drv->ctl_sock);
        drv->ctl_sock=1;
    }
}

+ (BOOL)getClientN:(FAST_AIO_DRV *)drv clientN:(int32_t *)clientN {
    uint64_t scalarOut[1];
    uint32_t scalarOutCount = 1 ;
    // Initialize to the size of scalarOut array
    kern_return_t result = IOConnectCallScalarMethod(drv->driverConnection, kAIOdriver_get_client_n, NULL, 0, scalarOut, &scalarOutCount);
    *clientN = (uint32_t)scalarOut[0];
    return result == 0 ;
}

//MARK: - UserSpace

+ (BOOL)initSharedMemory:(FAST_AIO_DRV *)drv name:(const io_name_t)name {
    io_name_t name2;
    int fd;
    NSLog(@"Init shared memory...");
    strcpy(name2,FAST_AIO_DRV_NAME_ROOT_US);
    strcat(name2,name);
    fd=shm_open(name2, O_RDWR, 0666);
    if (fd==-1) {
        NSLog(@"Can't open shm with error %d!", errno);
        return NO;
    }
    void *addr;
    addr=mmap(NULL, sizeof(AIOTRX_SHARED_MEMORY), PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (addr==MAP_FAILED) {
        NSLog(@"Can't mmap with error %d!",errno);
        return NO;
    }
    drv->driverMem=(AIOTRX_SHARED_MEMORY*)addr;
    NSLog(@"Shared memory inited!");
    return YES;
}

static void *watchdog_thread(void *arg) {
    FAST_AIO_DRV *drv=(FAST_AIO_DRV*)arg;
    do {
        usleep(50000);
        atomic_store(drv->driverMem->client_watchdog+drv->ClientN,100);
        long wdt;
        wdt=atomic_load(&drv->watchdog_timer);
        if (wdt>0)
        {
            if (wdt<2) NSLog(@"wdt==%ld",wdt);
            atomic_compare_exchange_strong(&drv->watchdog_timer, &wdt, wdt-1);
        }
        else if (wdt==0)
        {
            NSLog(@"wdt==0");
            if (atomic_compare_exchange_strong(&drv->watchdog_timer, &wdt, wdt-1))
            {
                sem_post(drv->WaitLock[0]);
            }
        }
    } while(!drv->watchdog_thread_done);
    return 0;
}

+ (BOOL)initLocks:(FAST_AIO_DRV *)drv name:(const io_name_t)name {
    for(int i=0; i<MAX_CLIENTS; i++) {
        unsigned int ev;
        ev=0;
        if (atomic_compare_exchange_strong(drv->driverMem->client_states+i,&ev,1)) {
            sem_t *sem;
            char sname[256];
            //Now we locked!
            drv->ClientN=i;
            NSLog(@"Now we are clent number %d!",i);
            snprintf(sname,256,FAST_AIO_DRV_NAME_ROOT_US "%s.c%dw0",name,i);
            sem=sem_open(sname,0 , 0666, 1);
            if (sem==SEM_FAILED) {
                NSLog(@"Can't open %s with error %d!",sname,errno);
                //return KERN_FAILURE;
                sem=NULL;
            }
            drv->WaitLock[0]=sem;
            snprintf(sname,256,FAST_AIO_DRV_NAME_ROOT_US "%s.c%dw1",name,i);
            sem=sem_open(sname,0, 0666, 1);
            if (sem==SEM_FAILED) {
                NSLog(@"Can't open %s with error %d!",sname,errno);
                //return KERN_FAILURE;
                sem=NULL;
            }
            drv->WaitLock[1]=sem;
            atomic_store(&drv->watchdog_timer,-1);
            drv->watchdog_thread_done=false;
            pthread_create(&(drv->watchdog_ptid), NULL, watchdog_thread, drv);
            return YES;
        }
    }
    NSLog(@"Can't alloc client!");
    return NO;
}

@end
