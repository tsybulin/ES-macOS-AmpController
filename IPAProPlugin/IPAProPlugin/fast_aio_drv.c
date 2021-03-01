//
//  fast_aio_drv.cpp
//  UserSpaceClient
//
//  Created by Dmytro Oparin on 3/26/20.
//

#include <CoreFoundation/CoreFoundation.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/mman.h>

#include "fast_aio_drv.h"

//#define DBG printf
#include <sys/syslog.h>
#define DBG(...) syslog(LOG_ERR,"IPAProPlugin: " __VA_ARGS__)


kern_return_t    StartTRX(FAST_AIO_DRV *drv)
{
    if (drv->srv_is_US)
    {
        uint32_t inputSEQ;
        uint32_t timeout;
        timeout=10000;
        inputSEQ=atomic_load(&drv->driverMem->readySEQ);
        atomic_store(&drv->driverMem->client_states[drv->ClientN],2);
        do
        {
            if (!(--timeout))
            {
                return KERN_FAILURE;
            }
            usleep(100);
        }
        while(inputSEQ==atomic_load(&drv->driverMem->readySEQ));
        inputSEQ=atomic_load(&drv->driverMem->readySEQ);
        do
        {
            if (!(--timeout))
            {
                return KERN_FAILURE;
            }
            usleep(100);
        }
        while(inputSEQ==atomic_load(&drv->driverMem->readySEQ));
        return KERN_SUCCESS;
    }
    else
        return IOConnectCallMethod(drv->driverConnection, kAIOdriver_start, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

kern_return_t    StopTRX(FAST_AIO_DRV *drv)
{
    if (drv->srv_is_US)
    {
        bzero(drv->client_output->output_channels,sizeof(drv->client_output->output_channels));
        atomic_store(&drv->driverMem->client_states[drv->ClientN],1);
        return KERN_SUCCESS;
    }
    else
        return IOConnectCallMethod(drv->driverConnection, kAIOdriver_stop, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

static kern_return_t   GetClientN(FAST_AIO_DRV *drv, int32_t* ClientN)
{
    uint64_t scalarOut[1];
    uint32_t scalarOutCount;
    kern_return_t     result;
    // Initialize to the size of scalarOut array
    scalarOutCount = 1;
    result = IOConnectCallScalarMethod(drv->driverConnection, kAIOdriver_get_client_n,
                                       NULL, 0, scalarOut, &scalarOutCount);
    *ClientN = (uint32_t)scalarOut[0];
    return result;
}

/*kern_return_t    WaitRXseq(FAST_AIO_DRV *drv, uint32_t seq)
{
    uint32_t ClientN=drv->ClientN;
    if (drv->srv_is_US)
    {
        int32_t d;
        if (!drv->WaitLock[1]) return KERN_SUCCESS;
        sem_post(drv->WaitLock[1]); //Notify to transmitter that data is ready
        //Wait until right sequence number reached
        for(;;)
        {
            drv->watchdog_abort=false;
            d=drv->driverMem->curtx_SEQ-seq;
            if (d>MAX_AIO_BLOCKS) return KERN_FAILURE;
            d=drv->driverMem->readySEQ-seq;
            if (d>=0) return KERN_SUCCESS;
            drv->watchdog_timer=2;
            drv->driverMem->waitSEQ[ClientN]=seq;
            if (sem_wait(drv->WaitLock[0]))
            {
                if (errno==EINTR)
                {
                    DBG("EINTR\n");
                    return KERN_FAILURE;
                }
                DBG("sem_wait error %d\n",errno);
                return KERN_FAILURE;
            }
            drv->watchdog_timer=0;
            drv->wait_locked=true;
            if (drv->watchdog_abort)
            {
                return KERN_FAILURE;
            }
        }
    }
    else
    {
        if (drv->ctl_sock>=0)
        {
            //Fast path
            drv->driverMem->waitSEQ[ClientN]=seq;
            return setsockopt(drv->ctl_sock,SYSPROTO_CONTROL,FAST_WAIT_RX_SEQ+ClientN,NULL,0);
        }
        else
        {
            uint64_t        scalarIn[1];
            scalarIn[0] = seq;
            return IOConnectCallScalarMethod(drv->driverConnection, kAIOdriver_wait_rx_seq, scalarIn, 1, NULL, NULL);
        }
    }
}*/


static void ConnectToCtl(FAST_AIO_DRV *drv, const io_name_t name)
{
    struct ctl_info ctl_info;
    struct sockaddr_ctl sc;
    
    drv->ctl_sock = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (drv->ctl_sock < 0)
    {
        DBG("Can't create PF_SYSTEM socket!\n");
        return ;
    }
    
    bzero(&ctl_info, sizeof(struct ctl_info));
    strcpy(ctl_info.ctl_name, FAST_AIO_DRV_NAME_ROOT_DOT);
    strcat(ctl_info.ctl_name, name);
    
    if (ioctl(drv->ctl_sock, CTLIOCGINFO, &ctl_info) == -1)
    {
        DBG("Can't do ioctl(ctl_sock)!\n");
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
    
    if (connect(drv->ctl_sock, (struct sockaddr *)&sc, sizeof(struct sockaddr_ctl)))
    {
        DBG("Can't connect ro ctl_sock!\n");
        close(drv->ctl_sock);
        drv->ctl_sock=1;
    }
}

static void DisconnectFromCtl(FAST_AIO_DRV *drv)
{
    if (drv->ctl_sock>=0)
        close(drv->ctl_sock);
    drv->ctl_sock=-1;
    DBG("ctl_sock closed!\n");
}

//Get samples from device dev_n and channel ch_n
//Place to *dest
//Number of samples = blocks * AIO_BLOCK_SZ
void GetSamples(FAST_AIO_DRV *drv, int32_t *dest, uint32_t SEQ, uint32_t ch_n, uint32_t blocks)
{
    AIOTRX_SHARED_MEMORY *driverMem=drv->driverMem;
    size_t chidx=ch_n;
    if (chidx<driverMem->num_inputs)
    {
        AIO_CHANNEL *sch=driverMem->input_channels+chidx;
        while(blocks)
        {
            int32_t *src;
            src=sch->blocks[SEQ % MAX_AIO_BLOCKS].samples;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            SEQ++;
            blocks--;
        }
    }
    else
        bzero(dest,sizeof(int32_t)*AIO_BLOCK_SZ*blocks);
}

//Put samples to device dev_n and channel ch_n
//Take from *src
//Number of samples = blocks * AIO_BLOCK_SZ
void PutSamples(FAST_AIO_DRV *drv, uint32_t SEQ, uint32_t ch_n, int32_t *src, uint32_t blocks)
{
    AIOTRX_SHARED_MEMORY *driverMem=drv->driverMem;
    AIO_OUTPUT_BY_CLIENT *client_output=drv->client_output;
    size_t chidx=ch_n;
    if (chidx<driverMem->num_outputs)
    {
        AIO_CHANNEL *dch=client_output->output_channels+chidx;
        while(blocks)
        {
            
            int32_t *dest;
            dest=dch->blocks[SEQ % MAX_AIO_BLOCKS].samples;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            *dest++=*src++;
            SEQ++;
            blocks--;
        }
    }
}

//User-space server stuff

static kern_return_t US_InitSharedMemory(FAST_AIO_DRV *drv, const io_name_t name)
{
    io_name_t name2;
    int fd;
    DBG("Init shared memory...\n");
    strcpy(name2,FAST_AIO_DRV_NAME_ROOT_US);
    strcat(name2,name);
    fd=shm_open(name2, O_RDWR, 0666);
    if (fd==-1)
    {
        DBG("Can't open shm with error %d!\n",errno);
        return KERN_FAILURE;
    }
    void *addr;
    addr=mmap(NULL, sizeof(AIOTRX_SHARED_MEMORY), PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (addr==MAP_FAILED)
    {
        DBG("Can't mmap with error %d!\n",errno);
        return KERN_FAILURE;
    }
    drv->driverMem=(AIOTRX_SHARED_MEMORY*)addr;
    DBG("Shared memory inited!\n");
    return KERN_SUCCESS;
}


static void *watchdog_thread(void *arg)
{
    FAST_AIO_DRV *drv=(FAST_AIO_DRV*)arg;
    do
    {
        usleep(50000);
        atomic_store(drv->driverMem->client_watchdog+drv->ClientN,100);
        /*if (drv->watchdog_timer)
        {
            drv->watchdog_timer--;
        }
        else
        {
            if (drv->wait_locked && !drv->watchdog_abort)
            {
                drv->watchdog_abort=true;
                sem_post(drv->WaitLock[0]);
            }
        }*/
    }
    while(!drv->watchdog_thread_done);
    return 0;
}


static kern_return_t US_InitLocks(FAST_AIO_DRV *drv, const io_name_t name)
{
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        unsigned int ev;
        ev=0;
        if (atomic_compare_exchange_strong(drv->driverMem->client_states+i,&ev,1))
        {
            sem_t *sem;
            char sname[256];
            //Now we locked!
            drv->ClientN=i;
            DBG("Now we are clent number %d!\n",i);
            snprintf(sname,256,FAST_AIO_DRV_NAME_ROOT_US "%s.c%dw0",name,i);
            sem=sem_open(sname,0 , 0666, 1);
            if (sem==SEM_FAILED)
            {
                DBG("Can't open %s with error %d!\n",sname,errno);
                //return KERN_FAILURE;
                sem=NULL;
            }
            drv->WaitLock[0]=sem;
            snprintf(sname,256,FAST_AIO_DRV_NAME_ROOT_US "%s.c%dw1",name,i);
            sem=sem_open(sname,0, 0666, 1);
            if (sem==SEM_FAILED)
            {
                DBG("Can't open %s with error %d!\n",sname,errno);
                //return KERN_FAILURE;
                sem=NULL;
            }
            drv->WaitLock[1]=sem;
            drv->watchdog_timer=0;
            drv->wait_locked=false;
            drv->watchdog_thread_done=false;
            pthread_create(&(drv->watchdog_ptid), NULL, watchdog_thread, drv);
            return 0;
        }
    }
    DBG("Can't alloc client!\n");
    return KERN_FAILURE;
}

//Cleanup stuff

void CloseDriver(FAST_AIO_DRV *drv)
{
    if (drv->srv_is_US)
    {
        //User-space mode
        DBG("User-space server cleanup...\n");
        if (drv->driverMem)
        {
            sem_t *sem;
            int32_t cn;
            cn=drv->ClientN;
            drv->driverMem->client_states[cn]=1;
            if (cn>=0)
            {
                drv->watchdog_thread_done=true;
                if (drv->watchdog_ptid)
                {
                    void *ret;
                    pthread_join(drv->watchdog_ptid,&ret);
                }
                sem=drv->WaitLock[0];
                drv->WaitLock[0]=0;
                if (sem)
                {
                    if (drv->wait_locked) sem_post(sem);
                    sem_close(sem);
                }
                sem=drv->WaitLock[1];
                drv->WaitLock[1]=0;
                if (sem)
                {
                    sem_close(sem);
                }
                AIO_OUTPUT_BY_CLIENT *tx;
                tx=drv->client_output;
                tx->need_wait=0;
                bzero(tx->output_channels,sizeof(tx->output_channels));
                atomic_store(drv->driverMem->client_states+cn,0);
            }
            DBG("Locks released!\n");
            void *addr;
            addr=drv->driverMem;
            drv->driverMem=NULL;
            munmap(addr, sizeof(AIOTRX_SHARED_MEMORY));
            DBG("Shared memory released!\n");
        }
    }
    else
    {
        //Kernel-space mode
        if (drv->driverMem)
        {
            mach_vm_address_t    theAddress;
            theAddress=(mach_vm_address_t)drv->driverMem;
            IOConnectUnmapMemory(drv->driverConnection, 1, mach_task_self(), theAddress);
            drv->driverMem=0;
        }
        DisconnectFromCtl(drv);
        if (drv->driverConnection)
        {
            IOServiceClose(drv->driverConnection);
            drv->driverConnection=0;
        }
        if (drv->service)
        {
            IOObjectRelease(drv->service);
            drv->service=0;
        }
    }
}

kern_return_t OpenDriver(FAST_AIO_DRV *drv, const io_name_t name)
{
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
    
    drv->srv_is_US=false;
    drv->WaitLock[0]=NULL;
    drv->WaitLock[1]=NULL;
    drv->watchdog_ptid=0;
    
    strcpy(name2,FAST_AIO_DRV_NAME_ROOT);
    strcat(name2,name);
    matchingDict = IOServiceMatching(name2);
    // Create an iterator for all IO Registry objects that match the dictionary
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
    if (kr != KERN_SUCCESS)
        return KERN_FAILURE;
    if((drv->service = IOIteratorNext(iter)) == 0)
    {
        // Release the iterator
        IOObjectRelease(iter);
        goto L_open_US;
        //return KERN_FAILURE;
    }
    // Release the iterator
    IOObjectRelease(iter);
    uint32_t type;
    type = 0;
    kr = IOServiceOpen(drv->service, mach_task_self(), type, &drv->driverConnection);
    if (kr != KERN_SUCCESS)
    {
        drv->driverConnection=0;
        CloseDriver(drv);
        goto L_open_US;
        //return kr;
    }
    GetClientN(drv, &drv->ClientN);
    DBG("Connected as client %d\n",drv->ClientN);
    if (drv->ClientN<0 || drv->ClientN>=MAX_CLIENTS)
    {
        DBG("Incorrect client number!\n");
        CloseDriver(drv);
        return kr;
    }
    ConnectToCtl(drv,name);
    mach_vm_address_t    theAddress;
    mach_vm_size_t        theSize;
    
    IOOptionBits memopt;
    memopt=kIOMapAnywhere;
    
    if ((kr=IOConnectMapMemory(drv->driverConnection, 1, mach_task_self(), &theAddress, &theSize, memopt))!=KERN_SUCCESS)
    {
        DBG("IOConnectMapMemory fault, error = %d\n",kr);
        CloseDriver(drv);
        return kr;
    }
    drv->driverMem=(AIOTRX_SHARED_MEMORY*)theAddress;
    drv->client_output=drv->driverMem->clients_output+drv->ClientN;
    return KERN_SUCCESS;
L_open_US:
    DBG("Try to init user-space driver...\n");
    drv->srv_is_US=true;
    if (US_InitSharedMemory(drv, name)) return KERN_FAILURE;
    if (US_InitLocks(drv,name))
    {
        CloseDriver(drv);
        return KERN_FAILURE;
    }
    drv->client_output=drv->driverMem->clients_output+drv->ClientN;
    return KERN_SUCCESS;
}

unsigned long IsConfigurationChanged(FAST_AIO_DRV *drv)
{
    unsigned int flag;
    flag=1;
    atomic_compare_exchange_strong(drv->driverMem->ConfigurationChanged+drv->ClientN, &flag, 0);
    return flag;
}
