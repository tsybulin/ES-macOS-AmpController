//
//  Plugin.c
//  AudioIO24
//
//  Created by Pavel Tsybulin on 2/5/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#include "Plugin.h"

#include <stdint.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <errno.h>
#include <termios.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

//#include "aiotrx_interface.h"

#include "global_defs.h"

#include "fast_aio_drv.h"

FAST_AIO_DRV *drv=NULL;

#define DBG(...) syslog(LOG_ERR,"IPAProPlugin: " __VA_ARGS__)

io_iterator_t        drv_iter = 0;
io_service_t        drv_service = 0;
io_connect_t    driverConnection=0;

//#include "asiopkt.h"
//#include "asiodevice.h"
//#include "ES_CircularBuf.h"

#define DEFAULT_SAMPLE_RATE 48000.0

//#include "ES_Event.h"
#include "doio_stat.h"

#define kIPAProDevice_UID "com.es.fastio.IPAPro"
#define kDevice_ModelUID "com.es.fastio.IPAPro"

#pragma mark - IPAProPlugin State

enum {
    kObjectID_PlugIn             = kAudioObjectPlugInObject,
    kObjectID_Device             = 2,
    kObjectID_Stream_Input       = 3,
    kObjectID_Stream_Output      = 4,
    kObjectID_Mute_Output_Master = 5,
    kObjectID_Mute_Input_Master  = 6
} ;

static UInt32 kSupportedOutputChannels = 0u ;
static UInt32 kSupportedInputChannels = 0u ;

//static UInt32 gDevice_OutDelay = DEFAULT_OUT_DELAY ;

enum {
    kChangeRequest_SampleFrequency = 1,
    kChangeRequest_NumZeroFrames = 2
} ;


static Float64 theHostClockFrequency ;

static Float64 gDevice_HostTicksPerFrame = 0.0 ;
static Float64 gDevice_SampleRate = DEFAULT_SAMPLE_RATE ;

typedef UInt32 UINT32 ;
typedef int32_t INT32 ;
typedef UInt8 UINT8 ;
typedef UInt16 UINT16 ;
typedef UInt32 DWORD;

volatile bool Done;
volatile bool AllDone = false;
volatile bool DBGDone = false;

typedef struct
{
    UINT32 base;
    volatile UINT32 count;
}DBG_COUNT;

#define CHECK_DBG_COUNTER(COUNTER) ((COUNTER).base!=(COUNTER).count)
#define RESET_DBG_COUNTER(COUNTER) ((COUNTER).base=(COUNTER).count)
#define ZERO_DBG_COUNTER(COUNTER) ((COUNTER).base=(COUNTER).count=0)

void *DBGlog_Thread(void* param);

typedef struct
{
    INT32 min;
    INT32 max;
    INT32 avg;
    UINT32 shift;
}TIME_STAT;

void FilterStat(TIME_STAT *stat, INT32 val)
{
    if (val<stat->min) stat->min = val;
    if (val>stat->max) stat->max = val;
    INT32 v = stat->avg;
    stat->avg += val;
    stat->avg -= v >> stat->shift;
}

TIME_STAT stat_delta_tx = { 0,0,0,7 };
TIME_STAT stat_delta_rx = {0,0,0,7};

void PrintStat(char *s, TIME_STAT *stat)
{
    DBG("%s%04d < %04d < %04d ", s, stat->min, stat->avg >> stat->shift, stat->max);
    stat->min = stat->max = stat->avg >> stat->shift;
}

static UInt32 gDevice_NumZeroFrames = MAX_AIO_BUFFER ;

DBG_COUNT tooearlyread_count; //Счетчик слишком долгих ожиданий входных данных (на самом деле это дроп)
DBG_COUNT toolatewrite_count; //Счетчик опустошения буфера для выходных данных

DBG_COUNT gapped_rx_count; // Счетчик разрывов в потоке RX
UInt64 expected_rx_pos;
DBG_COUNT gapped_tx_count; // Счетчик разрывов в потоке TX

DBG_COUNT rxspinlock_count;
DBG_COUNT txspinlock_count;
DBG_COUNT toomuchwait_tx_count;

//static volatile UINT32 write_size;

static bool gStream_Input_IsActive = true ;
static bool gStream_Output_IsActive = true ;
static bool gMute_Input_Master_Value = false ;
static bool gMute_Output_Master_Value = false ;
static AudioServerPlugInHostRef gPlugIn_Host = NULL ;
static UInt32 gPlugIn_RefCount = 0 ;
static pthread_mutex_t gPlugIn_StateMutex/* = PTHREAD_MUTEX_INITIALIZER */;
static volatile UInt64 gDevice_IOIsRunning = 0 ;
//static pthread_mutex_t gDevice_Output_Mutex = PTHREAD_MUTEX_INITIALIZER ;
//static ESEvent rxReadyEvent ;
static pthread_mutex_t gDevice_Input_Mutex/* = PTHREAD_MUTEX_INITIALIZER */;
static volatile UInt64 gHandler_Input_FrameCount = 0 ;
static volatile UInt64 gHandler_Output_FrameCount = 0 ;

void InitMutexes(void)
{
    int err;
    DBG("Init mutexes...\n");
    if ((err=pthread_mutex_init(&gPlugIn_StateMutex, NULL)))
    {
        DBG("gPlugIn_StateMutex init error %d\n",err);
    }
    if ((err=pthread_mutex_init(&gDevice_Input_Mutex, NULL)))
    {
        DBG("gDevice_Input_Mutex init error %d\n",err);
    }
}

#pragma mark - AudioServerPlugInDriverInterface Implementation

#pragma mark - Prototypes

//    Entry points for the COM methods
void* IPAProPlugin_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) ;
static HRESULT IPAProPlugin_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) ;
static ULONG IPAProPlugin_AddRef(void* inDriver) ;
static ULONG IPAProPlugin_Release(void* inDriver) ;
static OSStatus IPAProPlugin_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) ;
static OSStatus IPAProPlugin_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) ;
static OSStatus IPAProPlugin_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) ;
static OSStatus IPAProPlugin_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) ;
static OSStatus IPAProPlugin_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) ;
static OSStatus IPAProPlugin_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) ;
static OSStatus IPAProPlugin_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) ;
static Boolean IPAProPlugin_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) ;
static OSStatus IPAProPlugin_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) ;
static OSStatus IPAProPlugin_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) ;
static OSStatus IPAProPlugin_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) ;
static OSStatus IPAProPlugin_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData) ;
static OSStatus IPAProPlugin_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) ;
static OSStatus IPAProPlugin_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) ;
static OSStatus IPAProPlugin_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) ;
static OSStatus IPAProPlugin_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) ;
static OSStatus IPAProPlugin_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) ;
static OSStatus IPAProPlugin_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) ;
static OSStatus IPAProPlugin_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) ;

//    Implementation
static Boolean IPAProPlugin_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) ;
static OSStatus IPAProPlugin_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) ;
static OSStatus IPAProPlugin_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) ;
static OSStatus IPAProPlugin_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) ;
static OSStatus IPAProPlugin_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) ;

static Boolean IPAProPlugin_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) ;
static OSStatus IPAProPlugin_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) ;
static OSStatus IPAProPlugin_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) ;
static OSStatus IPAProPlugin_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) ;
static OSStatus IPAProPlugin_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) ;

static Boolean IPAProPlugin_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) ;
static OSStatus IPAProPlugin_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) ;
static OSStatus IPAProPlugin_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) ;
static OSStatus IPAProPlugin_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) ;
static OSStatus IPAProPlugin_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) ;

static Boolean IPAProPlugin_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) ;
static OSStatus IPAProPlugin_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) ;
static OSStatus IPAProPlugin_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) ;
static OSStatus IPAProPlugin_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) ;
static OSStatus IPAProPlugin_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) ;

//static bool IPAProPlugin_ConnectToHW(void) ;
//static void IPAProPlugin_DisconnectFromHW(void) ;
UInt32 IPAProPlugin_FindDevice(bool set_freq) ;

static AudioServerPlugInDriverInterface gAudioServerPlugInDriverInterface = {
    NULL,
    IPAProPlugin_QueryInterface,
    IPAProPlugin_AddRef,
    IPAProPlugin_Release,
    IPAProPlugin_Initialize,
    IPAProPlugin_CreateDevice,
    IPAProPlugin_DestroyDevice,
    IPAProPlugin_AddDeviceClient,
    IPAProPlugin_RemoveDeviceClient,
    IPAProPlugin_PerformDeviceConfigurationChange,
    IPAProPlugin_AbortDeviceConfigurationChange,
    IPAProPlugin_HasProperty,
    IPAProPlugin_IsPropertySettable,
    IPAProPlugin_GetPropertyDataSize,
    IPAProPlugin_GetPropertyData,
    IPAProPlugin_SetPropertyData,
    IPAProPlugin_StartIO,
    IPAProPlugin_StopIO,
    IPAProPlugin_GetZeroTimeStamp,
    IPAProPlugin_WillDoIOOperation,
    IPAProPlugin_BeginIOOperation,
    IPAProPlugin_DoIOOperation,
    IPAProPlugin_EndIOOperation
} ;

static AudioServerPlugInDriverInterface* gAudioServerPlugInDriverInterfacePtr = &gAudioServerPlugInDriverInterface ;
static AudioServerPlugInDriverRef gAudioServerPlugInDriverRef = &gAudioServerPlugInDriverInterfacePtr ;

#pragma mark - Factory

void* IPAProPlugin_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID) {
    //    This is the CFPlugIn factory function. Its job is to create the implementation for the given
    //    type provided that the type is supported. Because this driver is simple and all its
    //    initialization is handled via static iniitalization when the bundle is loaded, all that
    //    needs to be done is to return the AudioServerPlugInDriverRef that points to the driver's
    //    interface. A more complicated driver would create any base line objects it needs to satisfy
    //    the IUnknown methods that are used to discover that actual interface to talk to the driver.
    //    The majority of the driver's initilization should be handled in the Initialize() method of
    //    the driver's AudioServerPlugInDriverInterface.
    
#pragma unused(inAllocator)
    DBG("Create ---") ;
    drv=malloc(sizeof(FAST_AIO_DRV));
    memset(drv,0,sizeof(FAST_AIO_DRV));
    InitMutexes();
    void* theAnswer = NULL;
    if (OpenDriver(drv, "IPAPro"))
    {
        DBG("Can't open IPAPro, abort now!\n");
        return theAnswer;
    }
    DBG("Configuration name: %s",drv->driverMem->ConfigName);
    if(CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        theAnswer = gAudioServerPlugInDriverRef ;
    }
    
    return theAnswer ;
}

#pragma mark Inheritance

static HRESULT IPAProPlugin_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    //    This function is called by the HAL to get the interface to talk to the plug-in through.
    //    AudioServerPlugIns are required to support the IUnknown interface and the
    //    AudioServerPlugInDriverInterface. As it happens, all interfaces must also provide the
    //    IUnknown interface, so we can always just return the single interface we made with
    //    gAudioServerPlugInDriverInterfacePtr regardless of which one is asked for.
    
    //    declare the local variables
    DBG("QueryInterface ---") ;
    HRESULT theAnswer = 0 ;
    CFUUIDRef theRequestedUUID = NULL ;
    
    //    validate the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("QueryInterface: bad driver reference") ;
        return theAnswer ;
    }
    
    if (outInterface == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("QueryInterface: no place to store the returned interface") ;
        return theAnswer ;
    }
    
    //    make a CFUUIDRef from inUUID
    theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID) ;
    if (theRequestedUUID == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("QueryInterface: failed to create the CFUUIDRef") ;
        return theAnswer ;
    }
    
    //    AudioServerPlugIns only support two interfaces, IUnknown (which has to be supported by all
    //    CFPlugIns and AudioServerPlugInDriverInterface (which is the actual interface the HAL will
    //    use).
    if(CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gPlugIn_StateMutex) ;
        ++gPlugIn_RefCount ;
        pthread_mutex_unlock(&gPlugIn_StateMutex) ;
        *outInterface = gAudioServerPlugInDriverRef ;
    } else {
        theAnswer = E_NOINTERFACE ;
    }
    
    //    make sure to release the UUID we created
    CFRelease(theRequestedUUID) ;
    
    return theAnswer ;
}

static ULONG IPAProPlugin_AddRef(void* inDriver) {
    //    This call returns the resulting reference count after the increment.
    
    //    declare the local variables
    DBG("AddRef ---") ;
    ULONG theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("AddRef: bad driver reference") ;
        return theAnswer ;
    }
    
    //    increment the refcount
    pthread_mutex_lock(&gPlugIn_StateMutex) ;
    if(gPlugIn_RefCount < UINT32_MAX) {
        ++gPlugIn_RefCount ;
    }
    
    theAnswer = gPlugIn_RefCount ;
    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
    
    return theAnswer ;
}

static ULONG IPAProPlugin_Release(void* inDriver) {
    //    This call returns the resulting reference count after the decrement.
    
    //    declare the local variables
    DBG("Release ---") ;
    ULONG theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("Release: bad driver reference") ;
        return theAnswer ;
    }
    
    //    decrement the refcount
    pthread_mutex_lock(&gPlugIn_StateMutex) ;
    if(gPlugIn_RefCount > 0) {
        --gPlugIn_RefCount ;
    }
    else
    {
   
    
//        if (pluginHelper != NULL) {
//            Plugin_Helper_Free(pluginHelper) ;
//        }
        
        /*if (rxReadyEvent != NULL) {
            ESEvent_Free(rxReadyEvent) ;
        }*/
    }
    
    theAnswer = gPlugIn_RefCount ;
    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
    
    return theAnswer ;
}

#pragma mark Basic Operations

static OSStatus IPAProPlugin_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    //    The job of this method is, as the name implies, to get the driver initialized. One specific
    //    thing that needs to be done is to store the AudioServerPlugInHostRef so that it can be used
    //    later. Note that when this call returns, the HAL will scan the various lists the driver
    //    maintains (such as the device list) to get the inital set of objects the driver is
    //    publishing. So, there is no need to notifiy the HAL about any objects created as part of the
    //    execution of this method.
    
    DBG("Initialize ---") ;
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("Initialize: bad driver reference") ;
        return theAnswer ;
    }
    
    //    store the AudioServerPlugInHostRef
    gPlugIn_Host = inHost ;
    
    // Custom Properties
    //CFPropertyListRef theSettingsData = NULL ;
    
    time_t rawtime ;
    time(&rawtime) ;
    Float64 ignoreValue = rawtime ;
    CFNumberRef ignoreRef = CFNumberCreate(NULL, kCFNumberFloat64Type, &ignoreValue) ;
    gPlugIn_Host->WriteToStorage(gPlugIn_Host, CFSTR("IPAProPlugin_ignore_value"), ignoreRef) ;
    CFRelease(ignoreRef) ;
    DBGDone=false;
    pthread_t ptid_dbg ;

    pthread_create(&ptid_dbg, NULL, &DBGlog_Thread, NULL) ;

    struct sched_param sp;
    
    memset(&sp, 0, sizeof(struct sched_param));
    sp.sched_priority=sched_get_priority_min(SCHED_RR) ;
    pthread_setschedparam(ptid_dbg, SCHED_RR, &sp) ;
    
    Done = false;
    
    //    calculate the host ticks per frame
    struct mach_timebase_info theTimeBaseInfo ;
    mach_timebase_info(&theTimeBaseInfo) ;
    theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer ;
    theHostClockFrequency *= 1000000000.0 ;
    
    if (drv->driverMem->sample_frequency)
    {
        gDevice_SampleRate=drv->driverMem->sample_frequency;
    }
    
    gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate ;
#ifdef SUPERSIMPLE
    kSupportedInputChannels = 0;
    kSupportedOutputChannels = 2;
#else
    kSupportedInputChannels = drv->driverMem->num_inputs;
    kSupportedOutputChannels = drv->driverMem->num_outputs;
#endif
    
    DBG("In %dch, Out %dch",kSupportedInputChannels,kSupportedOutputChannels);

    return theAnswer ;
}

static OSStatus IPAProPlugin_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) {
    //    This method is used to tell a driver that implements the Transport Manager semantics to
    //    create an AudioEndpointDevice from a set of AudioEndpoints. Since this driver is not a
    //    Transport Manager, we just check the arguments and return
    //    kAudioHardwareUnsupportedOperationError.
    
#pragma unused(inDescription, inClientInfo, outDeviceObjectID)
    
    //    declare the local variables
    DBG("CreateDevice ---") ;
    OSStatus theAnswer = kAudioHardwareUnsupportedOperationError ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("CreateDevice: bad driver reference") ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    //    This method is used to tell a driver that implements the Transport Manager semantics to
    //    destroy an AudioEndpointDevice. Since this driver is not a Transport Manager, we just check
    //    the arguments and return kAudioHardwareUnsupportedOperationError.
    
#pragma unused(inDeviceObjectID)
    
    //    declare the local variables
    DBG("DestroyDevice ---") ;
    OSStatus theAnswer = kAudioHardwareUnsupportedOperationError ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("DestroyDevice: bad driver reference") ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    //    This method is used to inform the driver about a new client that is using the given device.
    //    This allows the device to act differently depending on who the client is. This driver does
    //    not need to track the clients using the device, so we just check the arguments and return
    //    successfully.
    
#pragma unused(inClientInfo)
    
    //    declare the local variables
    DBG("AddDeviceClient ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("AddDeviceClient: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("AddDeviceClient: bad device ID") ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    //    This method is used to inform the driver about a client that is no longer using the given
    //    device. This driver does not track clients, so we just check the arguments and return
    //    successfully.
    
#pragma unused(inClientInfo)
    
    //    declare the local variables
    DBG("RemoveDeviceClient ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("RemoveDeviceClient: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("RemoveDeviceClient: bad device ID") ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    //    This method is called to tell the device that it can perform the configuation change that it
    //    had requested via a call to the host method, RequestDeviceConfigurationChange(). The
    //    arguments, inChangeAction and inChangeInfo are the same as what was passed to
    //    RequestDeviceConfigurationChange().
    //
    //    The HAL guarantees that IO will be stopped while this method is in progress. The HAL will
    //    also handle figuring out exactly what changed for the non-control related properties. This
    //    means that the only notifications that would need to be sent here would be for either
    //    custom properties the HAL doesn't know about or for controls.
    //
    //    For the device implemented by this driver, only sample rate changes go through this process
    //    as it is the only state that can be changed for the device that isn't a control. For this
    //    change, the new sample rate is passed in the inChangeAction argument.
    
#pragma unused(inChangeInfo)
    
    //    declare the local variables
    DBG("PerformDeviceConfigurationChange ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("PerformDeviceConfigurationChange: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("PerformDeviceConfigurationChange: bad device ID") ;
        return theAnswer ;
    }
    
//    if (inChangeAction != kChangeRequest_StreamFormat) {
//        theAnswer = kAudioHardwareBadObjectError ;
//        DBG("PerformDeviceConfigurationChange: bad change request") ;
//        return theAnswer ;
//    }
    
    //    lock the state mutex
    pthread_mutex_lock(&gPlugIn_StateMutex) ;
    
    switch (inChangeAction) {
        case kChangeRequest_SampleFrequency: {

            Float64 *sampleFrequency = (Float64 *)inChangeInfo ;
            gDevice_SampleRate = *sampleFrequency ;
            DBG("kChangeRequest_SampleFrequency %f",gDevice_SampleRate);
            // recalculate the state that depends on the sample rate
            struct mach_timebase_info theTimeBaseInfo ;
            mach_timebase_info(&theTimeBaseInfo) ;
            Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer ;
            theHostClockFrequency *= 1000000000.0 ;
            gDevice_HostTicksPerFrame = theHostClockFrequency / gDevice_SampleRate ;
            
            free(sampleFrequency) ;

            CFNumberRef sampleRate = CFNumberCreate(NULL, kCFNumberFloat64Type, &gDevice_SampleRate) ;
            gPlugIn_Host->WriteToStorage(gPlugIn_Host, CFSTR("IPAProPlugin_sample_rate"), sampleRate) ;
            CFRelease(sampleRate) ;
        }
            break ;
        case kChangeRequest_NumZeroFrames:
            DBG("kChangeRequest_NumZeroFrames");
            break ;

        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    }
    
    //    unlock the state mutex
    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    //    This method is called to tell the driver that a request for a config change has been denied.
    //    This provides the driver an opportunity to clean up any state associated with the request.
    //    For this driver, an aborted config change requires no action. So we just check the arguments
    //    and return
    
#pragma unused(inChangeAction, inChangeInfo)
    
    //    declare the local variables
    DBG("AbortDeviceConfigurationChange ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("AbortDeviceConfigurationChange: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("AbortDeviceConfigurationChange: bad device ID") ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

#pragma mark - Property Operations

char cscope[5] ;
char cselector[5] ;

void enum_names(UInt32 scope, UInt32 selector) {
    memset(cscope, 0, 5) ;
    cscope[0] = ((char*) &scope)[3] ;
    cscope[1] = ((char*) &scope)[2] ;
    cscope[2] = ((char*) &scope)[1] ;
    cscope[3] = ((char*) &scope)[0] ;
    memset(cselector, 0, 5) ;
    cselector[0] = ((char*) &selector)[3] ;
    cselector[1] = ((char*) &selector)[2] ;
    cselector[2] = ((char*) &selector)[1] ;
    cselector[3] = ((char*) &selector)[0] ;
}

char* ao_name(AudioObjectID objectID) {
    switch (objectID) {
        case kObjectID_PlugIn:
            return "plugin" ;
            break;
            
        case kObjectID_Device:
            return "device" ;
            break;

        case kObjectID_Stream_Input:
            return "stream_inp" ;
            break;

        case kObjectID_Stream_Output:
            return "stream_out" ;
            break;

        case kObjectID_Mute_Output_Master:
            return "mute_out_mast" ;
            break;

        case kObjectID_Mute_Input_Master:
            return "mute_inp_mast" ;
            break;

        default:
            return "unknown" ;
            break;
    }
    
}

static Boolean IPAProPlugin_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    //    This method returns whether or not the given object has the given property.
    
    //    declare the local variables
    
    Boolean theAnswer = false ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("HasProperty: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasProperty: no address") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPropertyData() method.
    switch(inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = IPAProPlugin_HasPlugInProperty(inDriver, inObjectID, inClientProcessID, inAddress) ;
            break ;
            
        case kObjectID_Device:
            theAnswer = IPAProPlugin_HasDeviceProperty(inDriver, inObjectID, inClientProcessID, inAddress) ;
            break ;
            
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = IPAProPlugin_HasStreamProperty(inDriver, inObjectID, inClientProcessID, inAddress) ;
            break ;
            
        case kObjectID_Mute_Output_Master:
            theAnswer = IPAProPlugin_HasControlProperty(inDriver, inObjectID, inClientProcessID, inAddress) ;
            break ;
    };
    
Done:
    return theAnswer;
}

static OSStatus IPAProPlugin_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    //    This method returns whether or not the given property on the object can have its value
    //    changed.
    
    //    declare the local variables
    //    DBG("IsPropertySettable ---") ;
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("IsPropertySettable: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("IsPropertySettable: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outIsSettable == NULL) {
        DBG("IsPropertySettable: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPropertyData() method.
    switch(inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = IPAProPlugin_IsPlugInPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable) ;
            break;
            
        case kObjectID_Device:
            theAnswer = IPAProPlugin_IsDevicePropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable) ;
            break;
            
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = IPAProPlugin_IsStreamPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable) ;
            break;
            
        case kObjectID_Mute_Output_Master:
            theAnswer = IPAProPlugin_IsControlPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    //    This method returns the byte size of the property's data.
    
    //    declare the local variables
    //    DBG("GetPropertyDataSize ---") ;
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetPropertyDataSize: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetPropertyDataSize: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        DBG("GetPropertyDataSize: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPropertyData() method.
    switch(inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = IPAProPlugin_GetPlugInPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
            break;
            
        case kObjectID_Device:
            theAnswer = IPAProPlugin_GetDevicePropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
            break;
            
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = IPAProPlugin_GetStreamPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
            break;
            
        case kObjectID_Mute_Output_Master:
            theAnswer = IPAProPlugin_GetControlPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    //    declare the local variables
    //    DBG("GetPropertyData ---") ;
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outData == NULL) {
        DBG("GetPropertyData: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required.
    //
    //    Also, since most of the data that will get returned is static, there are few instances where
    //    it is necessary to lock the state mutex.
    switch(inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = IPAProPlugin_GetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
            break;
            
        case kObjectID_Device:
            theAnswer = IPAProPlugin_GetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
            break;
            
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = IPAProPlugin_GetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
            break;
            
        case kObjectID_Mute_Output_Master:
            theAnswer = IPAProPlugin_GetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
            break;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData) {
    //    declare the local variables
//    DBG("SetPropertyData oid=%u el=%u sc=%u sel=%u", inObjectID, inAddress->mElement, inAddress->mScope, inAddress->mSelector) ;
    OSStatus theAnswer = 0;
    UInt32 theNumberPropertiesChanged = 0;
    AudioObjectPropertyAddress theChangedAddresses[2];
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("SetPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("SetPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPropertyData() method.
    switch(inObjectID) {
        case kObjectID_PlugIn:
            theAnswer = IPAProPlugin_SetPlugInPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
            break;
            
        case kObjectID_Device:
            theAnswer = IPAProPlugin_SetDevicePropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
            break;
            
        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
            theAnswer = IPAProPlugin_SetStreamPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
            break;
            
        case kObjectID_Mute_Output_Master:
            theAnswer = IPAProPlugin_SetControlPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, inData, &theNumberPropertiesChanged, theChangedAddresses);
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    //    send any notifications
    if(theNumberPropertiesChanged > 0) {
        gPlugIn_Host->PropertiesChanged(gPlugIn_Host, inObjectID, theNumberPropertiesChanged, theChangedAddresses) ;
    }
    
    return theAnswer ;
}


#pragma mark - PlugIn Property Operations

static Boolean IPAProPlugin_HasPlugInProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    //    This method returns whether or not the plug-in object has the given property.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    Boolean theAnswer = false ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("HasPlugInProperty: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasPlugInProperty: no address") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasPlugInProperty: not the plug-in object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPlugInPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_IsPlugInPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    //    This method returns whether or not the given property on the plug-in object can have its
    //    value changed.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("IsPlugInPropertySettable: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("IsPlugInPropertySettable: no address") ;
        return theAnswer ;
    }
    
    if (outIsSettable == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("IsPlugInPropertySettable: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_PlugIn) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("IsPlugInPropertySettable: not the plug-in object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPlugInPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            *outIsSettable = false ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetPlugInPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    //    This method returns the byte size of the property's data.
    
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyDataSize: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyDataSize: no address") ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyDataSize: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_PlugIn) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyDataSize: not the plug-in object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPlugInPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyData: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyData: no address") ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetPlugInPropertyData: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (outData == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetPlugInPropertyData: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_PlugIn) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetPlugInPropertyData: not the plug-in object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required.
    //
    //    Also, since most of the data that will get returned is static, there are few instances where
    //    it is necessary to lock the state mutex.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            //    The base class for kAudioPlugInClassID is kAudioObjectClassID
            if (inDataSize < sizeof(AudioClassID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the plug-in") ;
                return theAnswer ;
            }
            *((AudioClassID*)outData) = kAudioObjectClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            //    The class is always kAudioPlugInClassID for regular drivers
            if (inDataSize < sizeof(AudioClassID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the plug-in") ;
                return theAnswer ;
            }
            *((AudioClassID*)outData) = kAudioPlugInClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            //    The plug-in doesn't have an owning object
            if (inDataSize < sizeof(AudioObjectID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the plug-in") ;
                return theAnswer ;
            }
            *((AudioObjectID*)outData) = kAudioObjectUnknown ;
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyManufacturer:
            //    This is the human readable name of the maker of the plug-in.
            if (inDataSize < sizeof(CFStringRef)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the plug-in") ;
                return theAnswer ;
            }
            *((CFStringRef*)outData) = CFSTR("ManufacturerName") ;
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
            //    This returns the objects directly owned by the object. In the case of the
            //    plug-in object it is the same as the device list.
        case kAudioPlugInPropertyDeviceList:
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID) ;
            
            //    Clamp that to the number of devices this driver implements (which is just 1)
            if(theNumberItemsToFetch > 1) {
                theNumberItemsToFetch = 1 ;
            }
            
            //    Write the devices' object IDs into the return value
            if(theNumberItemsToFetch > 0) {
                ((AudioObjectID*)outData)[0] = kObjectID_Device ;
            }
            
            //    Return how many bytes we wrote to
            *outDataSize = theNumberItemsToFetch * sizeof(AudioClassID) ;
            break ;
            
        case kAudioPlugInPropertyTranslateUIDToDevice:
            //    This property takes the CFString passed in the qualifier and converts that
            //    to the object ID of the device it corresponds to. For this driver, there is
            //    just the one device. Note that it is not an error if the string in the
            //    qualifier doesn't match any devices. In such case, kAudioObjectUnknown is
            //    the object ID to return.
            if (inDataSize < sizeof(AudioObjectID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyTranslateUIDToDevice") ;
                return theAnswer ;
            }
            
            if (inQualifierDataSize == sizeof(CFStringRef)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: the qualifier is the wrong size for kAudioPlugInPropertyTranslateUIDToDevice") ;
                return theAnswer ;
            }
            
            if (inQualifierData == NULL) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: no qualifier for kAudioPlugInPropertyTranslateUIDToDevice") ;
                return theAnswer ;
            }
            
            if(CFStringCompare(*((CFStringRef*)inQualifierData), CFSTR(kIPAProDevice_UID), 0) == kCFCompareEqualTo) {
                *((AudioObjectID*)outData) = kObjectID_Device ;
            } else {
                *((AudioObjectID*)outData) = kAudioObjectUnknown ;
            }
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioPlugInPropertyResourceBundle:
            //    The resource bundle is a path relative to the path of the plug-in's bundle.
            //    To specify that the plug-in bundle itself should be used, we just return the
            //    empty string.
            if (inDataSize < sizeof(AudioObjectID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetPlugInPropertyData: not enough space for the return value of kAudioPlugInPropertyResourceBundle") ;
                return theAnswer ;
            }
            *((CFStringRef*)outData) = CFSTR("") ;
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_SetPlugInPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData, inDataSize, inData)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetPlugInPropertyData: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetPlugInPropertyData: no address") ;
        return theAnswer ;
    }
    
    if (outNumberPropertiesChanged == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetPlugInPropertyData: no place to return the number of properties that changed") ;
        return theAnswer ;
    }
    
    if (outChangedAddresses == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetPlugInPropertyData: no place to return the properties that changed") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_PlugIn) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetPlugInPropertyData: not the plug-in object") ;
        return theAnswer ;
    }
    
    //    initialize the returned number of changed properties
    *outNumberPropertiesChanged = 0 ;
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetPlugInPropertyData() method.
    switch(inAddress->mSelector) {
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

#pragma mark - Device Property Operations

static Boolean IPAProPlugin_HasDeviceProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    //    This method returns whether or not the given object has the given property.

#pragma unused(inClientProcessID)
    
    //    declare the local variables
    Boolean theAnswer = false ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("HasDeviceProperty: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasDeviceProperty: no address") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Device) {
        DBG("HasDeviceProperty: not the device object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetDevicePropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            theAnswer = ((inAddress->mScope == kAudioObjectPropertyScopeGlobal) || (inAddress->mScope == kAudioObjectPropertyScopeInput && kSupportedInputChannels) || (inAddress->mScope == kAudioObjectPropertyScopeOutput && kSupportedOutputChannels)) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
            break ;
            
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            //        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            theAnswer = ((inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput)) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
            break ;
            
        case kAudioDevicePropertySafetyOffset:
            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput);
            break;
            
        case kAudioObjectPropertyElementName:
            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeInput) || (inAddress->mScope == kAudioObjectPropertyScopeOutput) ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_IsDevicePropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    //    This method returns whether or not the given property on the object can have its value
    //    changed.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("IsDevicePropertySettable: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("IsDevicePropertySettable: no address") ;
        return theAnswer ;
    }
    
    if (outIsSettable == NULL) {
        DBG("IsDevicePropertySettable: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Device) {
        DBG("IsDevicePropertySettable: not the device object") ;
        return theAnswer ;
    }
    
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetDevicePropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
        case kAudioObjectPropertyElementName:
            *outIsSettable = false ;
            break ;
            
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyZeroTimeStampPeriod:
            *outIsSettable = true ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetDevicePropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    //    This method returns the byte size of the property's data.
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetDevicePropertyDataSize: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetDevicePropertyDataSize: no address") ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetDevicePropertyDataSize: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetDevicePropertyDataSize: not the device object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetDevicePropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyName:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
            switch(inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    *outDataSize = 3 * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyScopeInput:
                    *outDataSize = 1 * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyScopeOutput:
                    *outDataSize = 2 * sizeof(AudioObjectID) ;
                    break ;
            } ;
            break ;
            
        case kAudioDevicePropertyDeviceUID:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioDevicePropertyTransportType:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioDevicePropertyClockDomain:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyDeviceIsAlive:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioDevicePropertyDeviceIsRunning:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyLatency:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyStreams:
            switch(inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    *outDataSize = ((kSupportedOutputChannels?1:0)+(kSupportedInputChannels?1:0)) * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyScopeInput:
                    *outDataSize = (kSupportedInputChannels?1:0) * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyScopeOutput:
                    *outDataSize = (kSupportedOutputChannels?1:0) * sizeof(AudioObjectID) ;
                    break ;
            } ;
            break ;
            
        case kAudioObjectPropertyControlList:
            *outDataSize = 1 * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioDevicePropertySafetyOffset:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64) ;
            break ;
            
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = 5 * sizeof(AudioValueRange) ;
            break;
            
        case kAudioDevicePropertyIsHidden:
            *outDataSize = sizeof(UInt32);
            break;
            
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            break;
            
        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (kSupportedOutputChannels * sizeof(AudioChannelDescription)) ;
            break;
            
        case kAudioDevicePropertyZeroTimeStampPeriod:
            *outDataSize = sizeof(UInt32) ;
            break;
            
        case kAudioObjectPropertyElementName:
            *outDataSize = sizeof(CFStringRef) ;
            break;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {

//    pthread_mutex_lock(&gPlugIn_StateMutex) ;
//    enum_names(inAddress->mScope, inAddress->mSelector) ;
//    DBG("GetDevicePropertyData oid=%s el=%u sc=%s sel=%s", ao_name(inObjectID), inAddress->mElement, cscope, cselector) ;
//    pthread_mutex_unlock(&gPlugIn_StateMutex) ;

#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    UInt32 theNumberItemsToFetch ;
    UInt32 theItemIndex ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetDevicePropertyData: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetDevicePropertyData: no address") ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetDevicePropertyData: no place to put the return value size") ;
        return theAnswer ;
    }
    
    if (outData == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("GetDevicePropertyData: no place to put the return value") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("GetDevicePropertyData: not the device object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required.
    //
    //    Also, since most of the data that will get returned is static, there are few instances where
    //    it is necessary to lock the state mutex.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            //    The base class for kAudioDeviceClassID is kAudioObjectClassID
            if (inDataSize < sizeof(AudioClassID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the device") ;
                return theAnswer ;
            }
            *((AudioClassID*)outData) = kAudioObjectClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            //    The class is always kAudioDeviceClassID for devices created by drivers
            if (inDataSize < sizeof(AudioClassID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyClass for the device") ;
                return theAnswer ;
            }
            *((AudioClassID*)outData) = kAudioDeviceClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            //    The device's owner is the plug-in object
            if (inDataSize < sizeof(AudioObjectID)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the device") ;
                return theAnswer ;
            }
            *((AudioObjectID*)outData) = kObjectID_PlugIn ;
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyName: {
            //    This is the human readable name of the device.
                if (inDataSize < sizeof(CFStringRef)) {
                    theAnswer = kAudioHardwareBadPropertySizeError ;
                    DBG("GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyName for the device") ;
                    return theAnswer ;
                }
            
                *((CFStringRef*)outData) = CFSTR("IPAPro Amp") ; // Localizable

            }

            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioObjectPropertyManufacturer:
            //    This is the human readable name of the maker of the plug-in.
            if (inDataSize < sizeof(CFStringRef)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioObjectPropertyManufacturer for the device") ;
                return theAnswer ;
            }
            *((CFStringRef*)outData) = CFSTR("Ethernet Sound") ; // Localizable
            *outDataSize = sizeof(CFStringRef) ;
            break;
            
        case kAudioObjectPropertyOwnedObjects:
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID) ;
            
            //    The device owns its streams and controls. Note that what is returned here
            //    depends on the scope requested.
            switch(inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    //    global scope means return all objects
                    if(theNumberItemsToFetch > 3) {
                        theNumberItemsToFetch = 3 ;
                    }
                    
                    //    fill out the list with as many objects as requested, which is everything
                    for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex ;
                    }
                    break ;
                    
                case kAudioObjectPropertyScopeInput:
                    //    input scope means just the objects on the input side
                    if(theNumberItemsToFetch > 1) {
                        theNumberItemsToFetch = 1 ;
                    }
                    
                    //    fill out the list with the right objects
                    for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Input + theItemIndex ;
                    }
                    break ;
                    
                case kAudioObjectPropertyScopeOutput:
                    //    output scope means just the objects on the output side
                    if(theNumberItemsToFetch > 2) {
                        theNumberItemsToFetch = 2 ;
                    }
                    
                    //    fill out the list with the right objects
                    for(theItemIndex = 0; theItemIndex < theNumberItemsToFetch; ++theItemIndex) {
                        ((AudioObjectID*)outData)[theItemIndex] = kObjectID_Stream_Output + theItemIndex ;
                    }
                    break ;
            } ;
            
            //    report how much we wrote
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioDevicePropertyDeviceUID:
            //    This is a CFString that is a persistent token that can identify the same
            //    audio device across boot sessions. Note that two instances of the same
            //    device must have different values for this property.
            if (inDataSize < sizeof(CFStringRef)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceUID for the device") ;
                return theAnswer ;
            }
            *((CFStringRef*)outData) = CFSTR(kIPAProDevice_UID) ;
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioDevicePropertyModelUID:
            //    This is a CFString that is a persistent token that can identify audio
            //    devices that are the same kind of device. Note that two instances of the
            //    save device must have the same value for this property.
            if (inDataSize < sizeof(CFStringRef)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyModelUID for the device") ;
                return theAnswer ;
            }
            *((CFStringRef*)outData) = CFSTR(kDevice_ModelUID) ;
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        case kAudioDevicePropertyTransportType:
            //    This value represents how the device is attached to the system. This can be
            //    any 32 bit integer, but common values for this property are defined in
            //    <CoreAudio/AudioHardwareBase.h>
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyTransportType for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = kAudioDeviceTransportTypeVirtual ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyRelatedDevices:
            //    The related devices property identifys device objects that are very closely
            //    related. Generally, this is for relating devices that are packaged together
            //    in the hardware such as when the input side and the output side of a piece
            //    of hardware can be clocked separately and therefore need to be represented
            //    as separate AudioDevice objects. In such case, both devices would report
            //    that they are related to each other. Note that at minimum, a device is
            //    related to itself, so this list will always be at least one item long.
            
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID) ;
            
            //    we only have the one device...
            if(theNumberItemsToFetch > 1) {
                theNumberItemsToFetch = 1 ;
            }
            
            //    Write the devices' object IDs into the return value
            if(theNumberItemsToFetch > 0) {
                ((AudioObjectID*)outData)[0] = kObjectID_Device ;
            }
            
            //    report how much we wrote
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioDevicePropertyClockDomain:
            //    This property allows the device to declare what other devices it is
            //    synchronized with in hardware. The way it works is that if two devices have
            //    the same value for this property and the value is not zero, then the two
            //    devices are synchronized in hardware. Note that a device that either can't
            //    be synchronized with others or doesn't know should return 0 for this
            //    property.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyClockDomain for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 0 ;
            *outDataSize = sizeof(UInt32) ;
            break;
            
        case kAudioDevicePropertyDeviceIsAlive:
            //    This property returns whether or not the device is alive. Note that it is
            //    note uncommon for a device to be dead but still momentarily availble in the
            //    device list. In the case of this device, it will always be alive.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsAlive for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 1 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyDeviceIsRunning:
            //    This property returns whether or not IO is running for the device. Note that
            //    we need to take both the state lock to check this value for thread safety.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceIsRunning for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = ((gDevice_IOIsRunning > 0) > 0) ? 1 : 0 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            //    This property returns whether or not the device wants to be able to be the
            //    default device for content. This is the device that iTunes and QuickTime
            //    will use to play their content on and FaceTime will use as it's microhphone.
            //    Nearly all devices should allow for this.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultDevice for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 1 ;
            *outDataSize = sizeof(UInt32) ;
            break;
            
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            //    This property returns whether or not the device wants to be the system
            //    default device. This is the device that is used to play interface sounds and
            //    other incidental or UI-related sounds on. Most devices should allow this
            //    although devices with lots of latency may not want to.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyDeviceCanBeDefaultSystemDevice for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 1 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyLatency:
            //    This property returns the presentation latency of the device. For this,
            //    device, the value is 0 due to the fact that it always vends silence.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyLatency for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 0 ;
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
                *((UInt32*)outData) = 0 ;
            }
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyStreams:
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID) ;
            UInt32 wp=0;
            
            //    Note that what is returned here depends on the scope requested.
            switch(inAddress->mScope) {
                case kAudioObjectPropertyScopeGlobal:
                    //    global scope means return all streams
                    if (kSupportedInputChannels && wp<theNumberItemsToFetch)
                    {
                        ((AudioObjectID*)outData)[wp] = kObjectID_Stream_Input ;
                        wp++;
                    }

                    if (kSupportedOutputChannels && wp<theNumberItemsToFetch)
                    {
                        ((AudioObjectID*)outData)[wp] = kObjectID_Stream_Output ;
                        wp++;
                    }

                    break ;
                    
                case kAudioObjectPropertyScopeInput:
                    //    input scope means just the objects on the input side
                    if (kSupportedInputChannels && wp<theNumberItemsToFetch)
                    {
                        ((AudioObjectID*)outData)[wp] = kObjectID_Stream_Input ;
                        wp++;
                    }
                    break ;
                    
                case kAudioObjectPropertyScopeOutput:
                    //    output scope means just the objects on the output side
                    if (kSupportedOutputChannels && wp<theNumberItemsToFetch)
                    {
                        ((AudioObjectID*)outData)[wp] = kObjectID_Stream_Output ;
                        wp++;
                    }
                    break ;
            } ;
            
            //    report how much we wrote
            *outDataSize = wp * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyControlList:
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioObjectID) ;
            if(theNumberItemsToFetch > 1) {
                theNumberItemsToFetch = 1 ;
            }
            
            // we only have a single control (output mute)
            *(AudioObjectID*)outData = kObjectID_Mute_Output_Master ;
            
            //    report how much we wrote
            *outDataSize = theNumberItemsToFetch * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioDevicePropertySafetyOffset:
            //    This property returns the how close to now the HAL can read and write. For
            //    this, device, the value is 0 due to the fact that it always vends silence.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertySafetyOffset for the device") ;
                return theAnswer ;
            }

            *((UInt32*)outData) = 0 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyNominalSampleRate:
            //    This property returns the nominal sample rate of the device. Note that we
            //    only need to take the state lock to get this value.
            if (inDataSize < sizeof(Float64)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyNominalSampleRate for the device") ;
                return theAnswer ;
            }
            //DBG("kAudioDevicePropertyNominalSampleRate=%f",gDevice_SampleRate);
            pthread_mutex_lock(&gPlugIn_StateMutex);
            *((Float64*)outData) = gDevice_SampleRate ;
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            *outDataSize = sizeof(Float64) ;
            break ;
            
        case kAudioDevicePropertyAvailableNominalSampleRates:
            //    This returns all nominal sample rates the device supports as an array of
            //    AudioValueRangeStructs. Note that for discrete sample rates, the range
            //    will have the minimum value equal to the maximum value.
            
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioValueRange) ;
            
            //    clamp it to the number of items we have
            if(theNumberItemsToFetch > 5) {
                theNumberItemsToFetch = 5 ;
            }
            
            //    fill out the return array
            if(theNumberItemsToFetch > 0) {
                ((AudioValueRange*)outData)[0].mMinimum = 44100.0 ;
                ((AudioValueRange*)outData)[0].mMaximum = 44100.0 ;
            }
            if(theNumberItemsToFetch > 1) {
                ((AudioValueRange*)outData)[1].mMinimum = 48000.0 ;
                ((AudioValueRange*)outData)[1].mMaximum = 48000.0 ;
            }
            if(theNumberItemsToFetch > 2) {
                ((AudioValueRange*)outData)[2].mMinimum = 88200.0 ;
                ((AudioValueRange*)outData)[2].mMaximum = 88200.0 ;
            }
            if(theNumberItemsToFetch > 3) {
                ((AudioValueRange*)outData)[3].mMinimum = 96000.0 ;
                ((AudioValueRange*)outData)[3].mMaximum = 96000.0 ;
            }
            if(theNumberItemsToFetch > 4) {
                ((AudioValueRange*)outData)[4].mMinimum = 192000.0 ;
                ((AudioValueRange*)outData)[4].mMaximum = 192000.0 ;
            }

            //    report how much we wrote
            *outDataSize = theNumberItemsToFetch * sizeof(AudioValueRange) ;
            break ;
            
        case kAudioDevicePropertyIsHidden:
            //    This returns whether or not the device is visible to clients.
            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyIsHidden for the device") ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 0 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyPreferredChannelsForStereo:
            //    This property returns which two channels to use as left/right for stereo
            //    data by default. Note that the channel numbers are 1-based.
            if (inDataSize < (2 * sizeof(UInt32))) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelsForStereo for the device") ;
                return theAnswer ;
            }

            ((UInt32*)outData)[0] = 1 ;
            ((UInt32*)outData)[1] = 2 ;
            *outDataSize = 2 * sizeof(UInt32) ;
            break ;
            
        case kAudioDevicePropertyPreferredChannelLayout: {
            //    This property returns the default AudioChannelLayout to use for the device by default.
            //    Does matter for outputs only, never calls for inputs
            
            UInt32 theACLSize = offsetof(AudioChannelLayout, mChannelDescriptions) + (kSupportedOutputChannels * sizeof(AudioChannelDescription)) ;
            if (inDataSize < theACLSize) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyPreferredChannelLayout for the device") ;
                return theAnswer ;
            }
            
            ((AudioChannelLayout*)outData)->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions ;
            ((AudioChannelLayout*)outData)->mChannelBitmap = 0 ;
            ((AudioChannelLayout*)outData)->mNumberChannelDescriptions = kSupportedOutputChannels ;
            for(theItemIndex = 0; theItemIndex < kSupportedOutputChannels; ++theItemIndex) {
                ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mChannelFlags = 0 ;
                ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[0] = 0 ;
                ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[1] = 0 ;
                ((AudioChannelLayout*)outData)->mChannelDescriptions[theItemIndex].mCoordinates[2] = 0 ;
            }
            if (kSupportedOutputChannels>=2)
            {
                ((AudioChannelLayout*)outData)->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left ;
                ((AudioChannelLayout*)outData)->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right ;
            }
            //((AudioChannelLayout*)outData)->mChannelDescriptions[2].mChannelLabel = kAudioChannelLabel_LeftSurround ;
            //((AudioChannelLayout*)outData)->mChannelDescriptions[3].mChannelLabel = kAudioChannelLabel_RightSurround ;

            *outDataSize = theACLSize ;
        }
            break ;
            
        case kAudioDevicePropertyZeroTimeStampPeriod:
            //    This property returns how many frames the HAL should expect to see between
            //    successive sample times in the zero time stamps this device √provides.

//            pthread_mutex_lock(&gPlugIn_StateMutex) ;
//            enum_names(inAddress->mScope, inAddress->mSelector) ;
//            DBG("GetDevicePropertyData oid=%s el=%u sc=%s sel=%s", ao_name(inObjectID), inAddress->mElement, cscope, cselector) ;
//            pthread_mutex_unlock(&gPlugIn_StateMutex) ;

            if (inDataSize < sizeof(UInt32)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("GetDevicePropertyData: not enough space for the return value of kAudioDevicePropertyZeroTimeStampPeriod for the device") ;
                return theAnswer ;
            }
            // as our ring-buffer only support sizes of page_length, we need to adjust the expected zero time stamp period
            *((UInt32*)outData) = gDevice_NumZeroFrames ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioObjectPropertyElementName: {
            bool found = false ;
            SInt32 idx = inAddress->mElement - 1 ;
            if (inAddress->mScope == kAudioObjectPropertyScopeInput)
            {
#ifdef SUPERSIMPLE
#else
                if (idx<drv->driverMem->num_inputs)
                {
                    *((CFStringRef*)outData) = CFStringCreateWithCString(NULL, (const char*)(drv->driverMem->input_names+idx), kCFStringEncodingUTF8) ;
                    found = true ;
                    
                }
#endif
            } else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
            {
#ifdef SUPERSIMPLE
                if (idx<2)
#else
                if (idx<drv->driverMem->num_outputs)
#endif
                {
                    *((CFStringRef*)outData) = CFStringCreateWithCString(NULL, (const char*)(drv->driverMem->output_names+idx), kCFStringEncodingUTF8) ;
                    found = true ;
                }
            }
            if (!found) {
                *((CFStringRef*)outData) = CFSTR("n/a") ;
            }
        }
            *outDataSize = sizeof(CFStringRef) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_SetDevicePropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetDevicePropertyData: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("SetDevicePropertyData: no address") ;
        return theAnswer ;
    }
    
    if (outNumberPropertiesChanged == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("SetDevicePropertyData: no place to return the number of properties that changed") ;
        return theAnswer ;
    }
    
    if (outChangedAddresses == NULL) {
        theAnswer = kAudioHardwareIllegalOperationError ;
        DBG("SetDevicePropertyData: no place to return the properties that changed") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Device) {
        theAnswer = kAudioHardwareBadObjectError ;
        DBG("SetDevicePropertyData: not the device object") ;
        return theAnswer ;
    }
    
    //    initialize the returned number of changed properties
    *outNumberPropertiesChanged = 0 ;
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetDevicePropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioDevicePropertyNominalSampleRate: {
            //    Changing the sample rate needs to be handled via the
            //    RequestConfigChange/PerformConfigChange machinery.
            
            //    check the arguments
            if (inDataSize != sizeof(Float64)) {
                theAnswer = kAudioHardwareBadPropertySizeError ;
                DBG("SetDevicePropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate") ;
                return theAnswer ;
            }
            
            if (
                (*((const Float64*)inData) != 44100.0) &&
                (*((const Float64*)inData) != 48000.0) &&
                (*((const Float64*)inData) != 88200.0) &&
                (*((const Float64*)inData) != 96000.0) &&
                (*((const Float64*)inData) != 192000.0)
            ) {
                theAnswer = kAudioHardwareIllegalOperationError ;
                DBG("SetDevicePropertyData: unsupported value for kAudioDevicePropertyNominalSampleRate") ;
                return theAnswer ;
            }
            
            Float64 theOldSampleRate ;
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            theOldSampleRate = gDevice_SampleRate ;
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            
            if (theOldSampleRate != *((const Float64*)inData)) {
                Float64 *newSampleRate = (Float64*)malloc(sizeof(Float64)) ;
                *newSampleRate = *((const Float64*)inData) ;
                DBG("newSampleRate=%f",*newSampleRate);
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, kChangeRequest_SampleFrequency, newSampleRate) ;
                    
                    // Post notification to controller
                    CFNotificationCenterRef center = CFNotificationCenterGetDarwinNotifyCenter() ;

                    if (*newSampleRate == 44100.0) {
                        CFNotificationCenterPostNotification(center, CFSTR("kIPAudioProDidChangeSampleFrequency44100"), NULL, NULL, true) ;
                    } else if (*newSampleRate == 48000.0) {
                        CFNotificationCenterPostNotification(center, CFSTR("kIPAudioProDidChangeSampleFrequency48000"), NULL, NULL, true) ;
                    } else if (*newSampleRate == 88200.0) {
                        CFNotificationCenterPostNotification(center, CFSTR("kIPAudioProDidChangeSampleFrequency88200"), NULL, NULL, true) ;
                    } else if (*newSampleRate == 96000.0) {
                        CFNotificationCenterPostNotification(center, CFSTR("kIPAudioProDidChangeSampleFrequency96000"), NULL, NULL, true) ;
                    } else if (*newSampleRate == 192000.0) {
                        CFNotificationCenterPostNotification(center, CFSTR("kIPAudioProDidChangeSampleFrequency192000"), NULL, NULL, true) ;
                    }
                    
                }) ;
            }
            break ;
        }
            
        case kAudioDevicePropertyZeroTimeStampPeriod: {
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            enum_names(inAddress->mScope, inAddress->mSelector) ;
            DBG("SetDevicePropertyData oid=%s el=%u sc=%s sel=%s", ao_name(inObjectID), inAddress->mElement, cscope, cselector) ;
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            
                UInt32 newPeriod = *((const UInt32*)inData) ;
                if (newPeriod != gDevice_NumZeroFrames) {
                    *outNumberPropertiesChanged += 1 ;
                    gDevice_NumZeroFrames = newPeriod ;
                }
            }
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

#pragma mark - Stream Property Operations

static Boolean IPAProPlugin_HasStreamProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    //    This method returns whether or not the given object has the given property.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    Boolean theAnswer = false ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("HasStreamProperty: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasStreamProperty: no address") ;
        return theAnswer ;
    }
    
    if (inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output) {
        DBG("HasStreamProperty: not a stream object") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetStreamPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
//            theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
            theAnswer = true ;
            break ;
    } ;
    
//    DBG("HasStreamProperty oid=%u el=%u sc=%u sel=%u a=%hhu", inObjectID, inAddress->mElement, inAddress->mScope, inAddress->mSelector, theAnswer) ;
    return theAnswer ;
}

static OSStatus IPAProPlugin_IsStreamPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    //    This method returns whether or not the given property on the object can have its value
    //    changed.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("IsStreamPropertySettable: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("IsStreamPropertySettable: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outIsSettable == NULL) {
        DBG("IsStreamPropertySettable: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
        DBG("IsStreamPropertySettable: not a stream object") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetStreamPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outIsSettable = false ;
            break ;
            
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = true ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetStreamPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    //    This method returns the byte size of the property's data.
    
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetStreamPropertyDataSize: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetStreamPropertyDataSize: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        DBG("GetStreamPropertyDataSize: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
        DBG("GetStreamPropertyDataSize: not a stream object") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetStreamPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0 * sizeof(AudioObjectID) ;
            break ;
            
        case kAudioStreamPropertyIsActive:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyDirection:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyTerminalType:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyStartingChannel:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyLatency:
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription) ;
            break ;
            
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = 5 * sizeof(AudioStreamRangedDescription) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    UInt32 theNumberItemsToFetch;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetStreamPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetStreamPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        DBG("GetStreamPropertyData: no place to put the return value size") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outData == NULL) {
        DBG("GetStreamPropertyData: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
        DBG("GetStreamPropertyData: not a stream object") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required.
    //
    //    Also, since most of the data that will get returned is static, there are few instances where
    //    it is necessary to lock the state mutex.
    switch(inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            //    The base class for kAudioStreamClassID is kAudioObjectClassID
            if (inDataSize < sizeof(AudioClassID)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((AudioClassID*)outData) = kAudioObjectClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            //    The class is always kAudioStreamClassID for streams created by drivers
            *((AudioClassID*)outData) = kAudioStreamClassID ;
            *outDataSize = sizeof(AudioClassID) ;
            break ;
            
        case kAudioObjectPropertyOwner:
            //    The stream's owner is the device object
            if (inDataSize < sizeof(AudioObjectID)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((AudioObjectID*)outData) = kObjectID_Device ;
            *outDataSize = sizeof(AudioObjectID) ;
            break ;
            
        case kAudioObjectPropertyOwnedObjects:
            //    Streams do not own any objects
            *outDataSize = 0 * sizeof(AudioObjectID);
            break;
            
        case kAudioStreamPropertyIsActive:
            //    This property tells the device whether or not the given stream is going to
            //    be used for IO. Note that we need to take the state lock to examine this
            //    value.
            if (inDataSize < sizeof(UInt32)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyIsActive for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            *((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ?  gStream_Input_IsActive : gStream_Output_IsActive ;
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyDirection:
            //    This returns whether the stream is an input stream or an output stream.
            if (inDataSize < sizeof(UInt32)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyDirection for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? 1 : 0 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyTerminalType:
            //    This returns a value that indicates what is at the other end of the stream
            //    such as a speaker or headphones, or a microphone. Values for this property
            //    are defined in <CoreAudio/AudioHardwareBase.h>
            if (inDataSize < sizeof(UInt32)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyTerminalType for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((UInt32*)outData) = (inObjectID == kObjectID_Stream_Input) ? kAudioStreamTerminalTypeDigitalAudioInterface : kAudioStreamTerminalTypeDigitalAudioInterface ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyStartingChannel:
            //    This property returns the absolute channel number for the first channel in
            //    the stream. For exmaple, if a device has two output streams with two
            //    channels each, then the starting channel number for the first stream is 1
            //    and the starting channel number fo the second stream is 3.
            if (inDataSize < sizeof(UInt32)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyStartingChannel for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 1 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyLatency:
            //    This property returns any additonal presentation latency the stream has.
            if (inDataSize < sizeof(UInt32)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyLatency for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            *((UInt32*)outData) = 0 ;
            *outDataSize = sizeof(UInt32) ;
            break ;
            
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            //    This returns the current format of the stream in an
            //    AudioStreamBasicDescription. Note that we need to hold the state lock to get
            //    this value.
            //    Note that for devices that don't override the mix operation, the virtual
            //    format has to be the same as the physical format.
            if (inDataSize < sizeof(AudioStreamBasicDescription)) {
                DBG("GetStreamPropertyData: not enough space for the return value of kAudioStreamPropertyVirtualFormat for the stream") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            ((AudioStreamBasicDescription*)outData)->mSampleRate = gDevice_SampleRate ;
            ((AudioStreamBasicDescription*)outData)->mFormatID = kAudioFormatLinearPCM ;
            ((AudioStreamBasicDescription*)outData)->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
            ((AudioStreamBasicDescription*)outData)->mBytesPerPacket = sizeof(SInt32) * ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels)  ;
            ((AudioStreamBasicDescription*)outData)->mFramesPerPacket = 1 ;
            ((AudioStreamBasicDescription*)outData)->mBytesPerFrame = sizeof(SInt32) * ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
            ((AudioStreamBasicDescription*)outData)->mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
            ((AudioStreamBasicDescription*)outData)->mBitsPerChannel = sizeof(SInt32) * 8 ; // 32 = 8bit * 4bytes
            
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            *outDataSize = sizeof(AudioStreamBasicDescription) ;
            break ;
            
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            //    This returns an array of AudioStreamRangedDescriptions that describe what
            //    formats are supported.
            
            //    Calculate the number of items that have been requested. Note that this
            //    number is allowed to be smaller than the actual size of the list. In such
            //    case, only that number of items will be returned
            theNumberItemsToFetch = inDataSize / sizeof(AudioStreamRangedDescription);
            
            //    clamp it to the number of items we have
            if (theNumberItemsToFetch > 5) {
                theNumberItemsToFetch = 5 ;
            }
            
            if (theNumberItemsToFetch > 0) {
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mSampleRate = 44100.0;
                ((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMinimum = 44100.0;
                ((AudioStreamRangedDescription*)outData)[0].mSampleRateRange.mMaximum = 44100.0;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerPacket = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32);
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mBytesPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32) ;
                ((AudioStreamRangedDescription*)outData)[0].mFormat.mBitsPerChannel = sizeof(SInt32) * 8 ;
            }
            
            if (theNumberItemsToFetch > 1) {
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mSampleRate = 48000.0;
                ((AudioStreamRangedDescription*)outData)[1].mSampleRateRange.mMinimum = 48000.0;
                ((AudioStreamRangedDescription*)outData)[1].mSampleRateRange.mMaximum = 48000.0;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mBytesPerPacket = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32);
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mBytesPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32) ;
                ((AudioStreamRangedDescription*)outData)[1].mFormat.mBitsPerChannel = sizeof(SInt32) * 8 ;
            }

            if (theNumberItemsToFetch > 2) {
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mSampleRate = 88200.0;
                ((AudioStreamRangedDescription*)outData)[2].mSampleRateRange.mMinimum = 88200.0;
                ((AudioStreamRangedDescription*)outData)[2].mSampleRateRange.mMaximum = 88200.0;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mBytesPerPacket = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32);
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mBytesPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32) ;
                ((AudioStreamRangedDescription*)outData)[2].mFormat.mBitsPerChannel = sizeof(SInt32) * 8 ;
            }

            if (theNumberItemsToFetch > 3) {
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mSampleRate = 96000.0 ;
                ((AudioStreamRangedDescription*)outData)[3].mSampleRateRange.mMinimum = 96000.0 ;
                ((AudioStreamRangedDescription*)outData)[3].mSampleRateRange.mMaximum = 96000.0 ;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mBytesPerPacket = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32);
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mBytesPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32) ;
                ((AudioStreamRangedDescription*)outData)[3].mFormat.mBitsPerChannel = sizeof(SInt32) * 8 ;
            }

            if (theNumberItemsToFetch > 4) {
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mSampleRate = 192000.0 ;
                ((AudioStreamRangedDescription*)outData)[4].mSampleRateRange.mMinimum = 192000.0 ;
                ((AudioStreamRangedDescription*)outData)[4].mSampleRateRange.mMaximum = 192000.0 ;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mFormatID = kAudioFormatLinearPCM;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked ;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mFramesPerPacket = 1;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mChannelsPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) ;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mBytesPerPacket = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32);
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mBytesPerFrame = ((inObjectID == kObjectID_Stream_Input) ? kSupportedInputChannels : kSupportedOutputChannels) * sizeof(SInt32) ;
                ((AudioStreamRangedDescription*)outData)[4].mFormat.mBitsPerChannel = sizeof(SInt32) * 8 ;
            }

            //    report how much we wrote
            *outDataSize = theNumberItemsToFetch * sizeof(AudioStreamRangedDescription) ;
            break ;
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_SetStreamPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("SetStreamPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("SetStreamPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outNumberPropertiesChanged == NULL) {
        DBG("SetStreamPropertyData: no place to return the number of properties that changed") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outChangedAddresses == NULL) {
        DBG("SetStreamPropertyData: no place to return the properties that changed") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if ((inObjectID != kObjectID_Stream_Input) && (inObjectID != kObjectID_Stream_Output)) {
        DBG("SetStreamPropertyData: not a stream object") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    initialize the returned number of changed properties
    *outNumberPropertiesChanged = 0 ;
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetStreamPropertyData() method.
    switch(inAddress->mSelector) {
        case kAudioStreamPropertyIsActive:
            //    Changing the active state of a stream doesn't affect IO or change the structure
            //    so we can just save the state and send the notification.
            if (inDataSize != sizeof(UInt32)) {
                DBG("SetStreamPropertyData: wrong size for the data for kAudioDevicePropertyNominalSampleRate") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            if(inObjectID == kObjectID_Stream_Input) {
                if(gStream_Input_IsActive != (*((const UInt32*)inData) != 0)) {
                    gStream_Input_IsActive = *((const UInt32*)inData) != 0 ;
                    *outNumberPropertiesChanged = 1;
                    outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive ;
                    outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal ;
                    outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster ;
                }
            } else {
                if(gStream_Output_IsActive != (*((const UInt32*)inData) != 0)) {
                    gStream_Output_IsActive = *((const UInt32*)inData) != 0 ;
                    *outNumberPropertiesChanged = 1;
                    outChangedAddresses[0].mSelector = kAudioStreamPropertyIsActive ;
                    outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal ;
                    outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster ;
                }
            }
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;
            break ;
            
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            //    Changing the stream format needs to be handled via the
            //    RequestConfigChange/PerformConfigChange machinery.
            const AudioStreamBasicDescription *newAudioFormat = (const AudioStreamBasicDescription*)inData ;
            //unsigned i ;
            
            if (inDataSize != sizeof(AudioStreamBasicDescription)) {
                DBG("SetStreamPropertyData: wrong size for the data for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioHardwareBadPropertySizeError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mFormatID != kAudioFormatLinearPCM) {
                DBG("SetStreamPropertyData: unsupported format ID for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mFormatFlags != (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked )/*kAudioFormatFlagsNativeFloatPacked*/) {
                DBG("SetStreamPropertyData: unsupported format flags %d for kAudioStreamPropertyPhysicalFormat", newAudioFormat->mFormatFlags) ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mBitsPerChannel != sizeof(SInt32) * 8) {
                DBG("SetStreamPropertyData: unsupported bits per channel for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mBytesPerPacket != sizeof(SInt32) * newAudioFormat->mChannelsPerFrame) {
                DBG("SetStreamPropertyData: unsupported bytes per packet for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mFramesPerPacket != 1) {
                DBG("SetStreamPropertyData: unsupported frames per packet for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mBytesPerFrame != newAudioFormat->mFramesPerPacket * newAudioFormat->mBytesPerPacket) {
                DBG("SetStreamPropertyData: unsupported bytes per frame for kAudioStreamPropertyPhysicalFormat") ;
                theAnswer = kAudioDeviceUnsupportedFormatError ;
                return theAnswer ;
            }
            
            if (newAudioFormat->mChannelsPerFrame != kSupportedOutputChannels) {
                DBG("SetStreamPropertyData: unsupported number of channels %d for kAudioStreamPropertyPhysicalFormat",newAudioFormat->mChannelsPerFrame) ;
                theAnswer = kAudioHardwareIllegalOperationError ;
                return theAnswer ;
            }
            
            Float64 newSampleRate = newAudioFormat->mSampleRate ;
            if (
                newSampleRate !=  44100.0 &&
                newSampleRate !=  48000.0 &&
                newSampleRate !=  88200.0 &&
                newSampleRate !=  96000.0 &&
                newSampleRate != 192000.0
            ) {
                theAnswer = kAudioHardwareIllegalOperationError ;
                DBG("SetStreamPropertyData: unsupported Sample Rate for kAudioStreamPropertyPhysicalFormat") ;
                return theAnswer ;
            }

            Float64 theOldSampleRate ;
            pthread_mutex_lock(&gPlugIn_StateMutex) ;
            theOldSampleRate = gDevice_SampleRate ;
            pthread_mutex_unlock(&gPlugIn_StateMutex) ;

            if (theOldSampleRate != *((const Float64*)inData)) {
                Float64 *newSampleRate = (Float64*)malloc(sizeof(Float64)) ;
                *newSampleRate = *((const Float64*)inData) ;
                dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                        gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, kChangeRequest_SampleFrequency, newSampleRate) ;
                    }
               ) ;
            }

            break ;
        }
            
        default:
            theAnswer = kAudioHardwareUnknownPropertyError ;
            break ;
    } ;
    
    return theAnswer ;
}


#pragma mark - Control Property Operations

static Boolean IPAProPlugin_HasControlProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress) {
    //    This method returns whether or not the given object has the given property.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    Boolean theAnswer = false ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("HasControlProperty: bad driver reference") ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("HasControlProperty: no address") ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetControlPropertyData() method.
    switch(inObjectID) {
        case kObjectID_Mute_Output_Master:
            switch(inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                case kAudioBooleanControlPropertyValue:
                    theAnswer = (inAddress->mScope == kAudioObjectPropertyScopeGlobal) && (inAddress->mElement == kAudioObjectPropertyElementMaster) ;
                    break ;
            } ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_IsControlPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) {
    //    This method returns whether or not the given property on the object can have its value
    //    changed.
    
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("IsControlPropertySettable: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("IsControlPropertySettable: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outIsSettable == NULL) {
        DBG("IsControlPropertySettable: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetControlPropertyData() method.
    switch(inObjectID) {
        case kObjectID_Mute_Output_Master:
            switch(inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioControlPropertyScope:
                case kAudioControlPropertyElement:
                    *outIsSettable = false ;
                    break ;
                    
                case kAudioBooleanControlPropertyValue:
                    *outIsSettable = true ;
                    break ;
                    
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError ;
                    break ;
            } ;
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetControlPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize) {
    //    This method returns the byte size of the property's data.
    
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetControlPropertyDataSize: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetControlPropertyDataSize: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        DBG("GetControlPropertyDataSize: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetControlPropertyData() method.
    switch(inObjectID) {
        case kObjectID_Mute_Output_Master:
            switch(inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    *outDataSize = sizeof(AudioClassID) ;
                    break ;
                    
                case kAudioObjectPropertyClass:
                    *outDataSize = sizeof(AudioClassID) ;
                    break ;
                    
                case kAudioObjectPropertyOwner:
                    *outDataSize = sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0 * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioControlPropertyScope:
                    *outDataSize = sizeof(AudioObjectPropertyScope) ;
                    break ;
                    
                case kAudioControlPropertyElement:
                    *outDataSize = sizeof(AudioObjectPropertyElement) ;
                    break ;
                    
                case kAudioBooleanControlPropertyValue:
                    *outDataSize = sizeof(UInt32) ;
                    break ;
                    
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError ;
                    break ;
            } ;
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_GetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
#pragma unused(inClientProcessID)
    
    //    declare the local variables
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetControlPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("GetControlPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outDataSize == NULL) {
        DBG("GetControlPropertyData: no place to put the return value size") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outData == NULL) {
        DBG("GetControlPropertyData: no place to put the return value") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required.
    //
    //    Also, since most of the data that will get returned is static, there are few instances where
    //    it is necessary to lock the state mutex.
    switch(inObjectID) {
        case kObjectID_Mute_Output_Master:
            switch(inAddress->mSelector) {
                case kAudioObjectPropertyBaseClass:
                    //    The base class for kAudioMuteControlClassID is kAudioBooleanControlClassID
                    if (inDataSize < sizeof(AudioClassID)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyBaseClass for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    *((AudioClassID*)outData) = kAudioBooleanControlClassID ;
                    *outDataSize = sizeof(AudioClassID) ;
                    break ;
                    
                case kAudioObjectPropertyClass:
                    //    Mute controls are of the class, kAudioMuteControlClassID
                    if (inDataSize < sizeof(AudioClassID)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyClass for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    *((AudioClassID*)outData) = kAudioMuteControlClassID ;
                    *outDataSize = sizeof(AudioClassID) ;
                    break ;
                    
                case kAudioObjectPropertyOwner:
                    //    The control's owner is the device object
                    if (inDataSize < sizeof(AudioObjectID)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioObjectPropertyOwner for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    *((AudioObjectID*)outData) = kObjectID_Device ;
                    *outDataSize = sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioObjectPropertyOwnedObjects:
                    //    Controls do not own any objects
                    *outDataSize = 0 * sizeof(AudioObjectID) ;
                    break ;
                    
                case kAudioControlPropertyScope:
                    //    This property returns the scope that the control is attached to.
                    if (inDataSize < sizeof(AudioObjectPropertyScope)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioControlPropertyScope for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    *((AudioObjectPropertyScope*)outData) = kAudioObjectPropertyScopeOutput ;
                    *outDataSize = sizeof(AudioObjectPropertyScope) ;
                    break ;
                    
                case kAudioControlPropertyElement:
                    //    This property returns the element that the control is attached to.
                    if (inDataSize < sizeof(AudioObjectPropertyElement)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioControlPropertyElement for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    *((AudioObjectPropertyElement*)outData) = kAudioObjectPropertyElementMaster ;
                    *outDataSize = sizeof(AudioObjectPropertyElement) ;
                    break ;
                    
                case kAudioBooleanControlPropertyValue:
                    //    This returns the value of the mute control where 0 means that mute is off
                    //    and audio can be heard and 1 means that mute is on and audio cannot be heard.
                    //    Note that we need to take the state lock to examine this value.
                    if (inDataSize < sizeof(UInt32)) {
                        DBG("GetControlPropertyData: not enough space for the return value of kAudioBooleanControlPropertyValue for the mute control") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    pthread_mutex_lock(&gPlugIn_StateMutex) ;
                    *((UInt32*)outData) = (inObjectID == kObjectID_Mute_Input_Master) ? (gMute_Input_Master_Value ? 1 : 0) : (gMute_Output_Master_Value ? 1 : 0) ;
                    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
                    *outDataSize = sizeof(UInt32) ;
                    break ;
                    
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError ;
                    break ;
            } ;
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_SetControlPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData, UInt32* outNumberPropertiesChanged, AudioObjectPropertyAddress outChangedAddresses[2]) {
#pragma unused(inClientProcessID, inQualifierDataSize, inQualifierData)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("SetControlPropertyData: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inAddress == NULL) {
        DBG("SetControlPropertyData: no address") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outNumberPropertiesChanged == NULL) {
        DBG("SetControlPropertyData: no place to return the number of properties that changed") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    if (outChangedAddresses == NULL) {
        DBG("SetControlPropertyData: no place to return the properties that changed") ;
        theAnswer = kAudioHardwareIllegalOperationError ;
        return theAnswer ;
    }
    
    //    initialize the returned number of changed properties
    *outNumberPropertiesChanged = 0 ;
    
    //    Note that for each object, this driver implements all the required properties plus a few
    //    extras that are useful but not required. There is more detailed commentary about each
    //    property in the LoopbackAudio_GetControlPropertyData() method.
    switch(inObjectID) {
        case kObjectID_Mute_Output_Master:
            switch(inAddress->mSelector) {
                case kAudioBooleanControlPropertyValue:
                    if (inDataSize != sizeof(UInt32)) {
                        DBG("SetControlPropertyData: wrong size for the data for kAudioBooleanControlPropertyValue") ;
                        theAnswer = kAudioHardwareBadPropertySizeError ;
                        return theAnswer ;
                    }
                    pthread_mutex_lock(&gPlugIn_StateMutex) ;
                    if(gMute_Output_Master_Value != (*((const UInt32*)inData) != 0)) {
                        gMute_Output_Master_Value = *((const UInt32*)inData) != 0 ;
                        *outNumberPropertiesChanged = 1 ;
                        outChangedAddresses[0].mSelector = kAudioBooleanControlPropertyValue ;
                        outChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal ;
                        outChangedAddresses[0].mElement = kAudioObjectPropertyElementMaster ;
                    }
                    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
                    break ;
                    
                default:
                    theAnswer = kAudioHardwareUnknownPropertyError ;
                    break ;
            } ;
            break ;
            
        default:
            theAnswer = kAudioHardwareBadObjectError ;
            break ;
    } ;
    
    return theAnswer ;
}

#pragma mark - IO Operations

static OSStatus IPAProPlugin_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    //    This call tells the device that IO is starting for the given client. When this routine
    //    returns, the device's clock is running and it is ready to have data read/written. It is
    //    important to note that multiple clients can have IO running on the device at the same time.
    //    So, work only needs to be done when the first client starts. All subsequent starts simply
    //    increment the counter.
    
    DBG("StartIO ---") ;

#pragma unused(inClientID)
    
    //    declare the local variables
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("StartIO: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("StartIO: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    we need to hold the state lock
    pthread_mutex_lock(&gPlugIn_StateMutex) ;
    
    //    figure out what we need to do
    if(gDevice_IOIsRunning == UINT32_MAX) {
        //    overflowing is an error
        theAnswer = kAudioHardwareIllegalOperationError ;
    } else if (gDevice_IOIsRunning == 0) {
        //    We need to start the hardware, which in this case is just anchoring the time line.
        //pthread_mutex_lock(&gDevice_Input_Mutex) ;
        ZERO_DBG_COUNTER(tooearlyread_count);
        ZERO_DBG_COUNTER(toolatewrite_count);
        ZERO_DBG_COUNTER(toomuchwait_tx_count);
        ZERO_DBG_COUNTER(gapped_rx_count);
        ZERO_DBG_COUNTER(gapped_tx_count);
        ZERO_DBG_COUNTER(rxspinlock_count);
        //drv->driverMem->gDevice_ZeroHostTime=mach_absolute_time();
        //drv->driverMem->gDevice_ZeroSampleTime=0;
        DBG("Call StartTRX");
        StartTRX(drv);
        //DBG("Return from StartTRX");
        gDevice_IOIsRunning = 1 ;

        gHandler_Input_FrameCount = 0 ;
        gHandler_Output_FrameCount = 0 ;
        //pthread_mutex_unlock(&gDevice_Input_Mutex) ;
    } else {
        //    IO is already running, so just bump the counter
        DBG("Already running!");
        ++gDevice_IOIsRunning ;
    }
    
    //    unlock the state lock
    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
    //DBG("StartIO ok...") ;
    return theAnswer ;
}

static OSStatus IPAProPlugin_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) {
    //    This call tells the device that the client has stopped IO. The driver can stop the hardware
    //    once all clients have stopped.
    
#pragma unused(inClientID)
    
    //    declare the local variables
    DBG("StopIO ---") ;
    OSStatus theAnswer = 0;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("StopIO: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("StopIO: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    we need to hold the state lock
    pthread_mutex_lock(&gPlugIn_StateMutex) ;
    
    //    figure out what we need to do
    if(gDevice_IOIsRunning == 0) {
        //    underflowing is an error
        theAnswer = kAudioHardwareIllegalOperationError ;
    } else if(gDevice_IOIsRunning == 1) {
        //    We need to stop the hardware.
        //pthread_mutex_lock(&gDevice_Input_Mutex) ;
        gDevice_IOIsRunning = 0 ;
        StopTRX(drv);
        //pthread_mutex_unlock(&gDevice_Input_Mutex) ;

    } else {
        //    IO is still running, so just bump the counter
        --gDevice_IOIsRunning ;
    }
    
    //    unlock the state lock
    DBG("StopIO ok...") ;
    pthread_mutex_unlock(&gPlugIn_StateMutex) ;
    
    return theAnswer ;
}

volatile UINT32 gzts_count;

static OSStatus IPAProPlugin_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed) {
    //    This method returns the current zero time stamp for the device. The HAL models the timing of
    //    a device as a series of time stamps that relate the sample time to a host time. The zero
    //    time stamps are spaced such that the sample times are the value of
    //    kAudioDevicePropertyZeroTimeStampPeriod apart. This is often modeled using a ring buffer
    //    where the zero time stamp is updated when wrapping around the ring buffer.
    //
    //    For this device, the zero time stamps' sample time increments every kDevice_RingBufferSize
    //    frames and the host time increments by kDevice_RingBufferSize * gDevice_HostTicksPerFrame.
    
#pragma unused(inClientID)
    
    //    declare the local variables
    OSStatus theAnswer = 0;

    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("GetZeroTimeStamp: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("GetZeroTimeStamp: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }

    static uint64_t old_zst,old_zht;
    uint64_t zst,zht;
    
    //uint32_t ok=0;
    
    //pthread_mutex_lock(&gDevice_Input_Mutex);
    zst=atomic_load(&drv->driverMem->gDevice_ZeroSampleTime);
    zht=atomic_load(&drv->driverMem->gDevice_ZeroHostTime);
    old_zst=zst;
    old_zht=zht;
    *outSampleTime=zst;
    *outHostTime=zht;
    //pthread_mutex_unlock(&gDevice_Input_Mutex) ;
    gzts_count++;
    //DBG("ok=%d gDevice_ZeroSampleTime=%llu gDevice_ZeroHostTime=%llu",ok,zst,zht);

    *outSeed = 1 ;

    return theAnswer ;
}

static OSStatus IPAProPlugin_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) {
    //    This method returns whether or not the device will do a given IO operation. For this device,
    //    we only support reading input data and writing output data.
    
#pragma unused(inClientID)
    
    //    declare the local variables
    //    DBG("WillDoIOOperation ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("WillDoIOOperation: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("WillDoIOOperation: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    //    figure out if we support the operation
    bool willDo = false ;
    bool willDoInPlace = true ;
    switch(inOperationID) {
        case kAudioServerPlugInIOOperationReadInput:
            willDo = true ;
            willDoInPlace = true ;
            break ;
            
        case kAudioServerPlugInIOOperationWriteMix:
            willDo = true ;
            willDoInPlace = true ;
            break ;
            
    } ;
    
    //    fill out the return values
    if(outWillDo != NULL) {
        *outWillDo = willDo ;
    }
    if(outWillDoInPlace != NULL) {
        *outWillDoInPlace = willDoInPlace ;
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    //    This is called at the beginning of an IO operation.
    
#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
    
    //    declare the local variables
    //    DBG("BeginIOOperation ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("WillDoIOOperation: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("WillDoIOOperation: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

volatile bool EnableRxWait;
volatile uint32_t EnableWaitFuseCounter;

//volatile UINT32 dbg_samples[10];

static OSStatus IPAProPlugin_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
    

#pragma unused(inClientID, inIOCycleInfo, ioSecondaryBuffer)
    
    //    declare the local variables
    
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("DoIOOperation: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("DoIOOperation: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if ((inStreamObjectID != kObjectID_Stream_Input) && (inStreamObjectID != kObjectID_Stream_Output)) {
        DBG("DoIOOperation: bad stream ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    INT32 *p=ioMainBuffer;
    
    if (inOperationID == kAudioServerPlugInIOOperationReadInput)
    {
        AIOTRX_SHARED_MEMORY *dm=drv->driverMem;
        //pthread_mutex_lock(&gDevice_Input_Mutex);
        UInt64 ipos = inIOCycleInfo->mInputTime.mSampleTime ;
        if (ipos>16) ipos-=16; //Preroll
        size_t i=inIOBufferFrameSize;
        SInt64 delta;
        delta=ipos+i-atomic_load(&dm->CurrentRecvSample);
        if (delta>0)
        {
            tooearlyread_count.count++;
        }
        FilterStat(&stat_delta_rx, (INT32)delta);
        size_t pos=ipos;
        while(i)
        {
            pos%=MAX_AIO_BUFFER;
            size_t blk;
            size_t smp;
            blk=pos / AIO_BLOCK_SZ;
            smp=pos % AIO_BLOCK_SZ;
#ifdef SUPERSIMPLE
#else
            for(size_t ch=0; ch<dm->num_inputs; ch++)
            {
                *p++=dm->input_channels[ch].blocks[blk].samples[smp];
            }
#endif
            pos++;
            i--;
        }
        //pthread_mutex_unlock(&gDevice_Input_Mutex);
    }
    else if (inOperationID == kAudioServerPlugInIOOperationWriteMix)
    {
        AIOTRX_SHARED_MEMORY *dm=drv->driverMem;
        AIO_OUTPUT_BY_CLIENT *clo=dm->clients_output+drv->ClientN;
        //pthread_mutex_lock(&gDevice_Input_Mutex);
        UInt64 ipos = inIOCycleInfo->mOutputTime.mSampleTime ;
        //memcpy((void*)dbg_samples,p,4*10);
        //LastTxPos=ipos;
        size_t i=inIOBufferFrameSize;
        size_t pos=ipos;
        while(i)
        {
            pos%=MAX_AIO_BUFFER;
            size_t blk;
            size_t smp;
            blk=pos / AIO_BLOCK_SZ;
            smp=pos % AIO_BLOCK_SZ;
#ifdef SUPERSIMPLE
            for(size_t ch=0; ch<2; ch++)
            {
                clo->output_channels[ch+2].blocks[blk].samples[smp]=*p++;
            }
#else
            for(size_t ch=0; ch<dm->num_outputs; ch++)
            {
                clo->output_channels[ch].blocks[blk].samples[smp]=*p++;
            }
#endif
            pos++;
            i--;
        }
        SInt64 delta;
        delta=ipos - atomic_load(&dm->CurrentTxSendSample);
        if (delta<0)
        {
            toolatewrite_count.count++;
        }
        FilterStat(&stat_delta_tx,(INT32)delta);
        //pthread_mutex_unlock(&gDevice_Input_Mutex);
    }
    
    return theAnswer ;
}

static OSStatus IPAProPlugin_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) {
    //    This is called at the end of an IO operation. This device doesn't do anything, so just check
    //    the arguments and return.
    
#pragma unused(inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo)
    
    //    declare the local variables
    //    DBG("EndIOOperation ---") ;
    OSStatus theAnswer = 0 ;
    
    //    check the arguments
    if (inDriver != gAudioServerPlugInDriverRef) {
        DBG("EndIOOperation: bad driver reference") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    if (inDeviceObjectID != kObjectID_Device) {
        DBG("EndIOOperation: bad device ID") ;
        theAnswer = kAudioHardwareBadObjectError ;
        return theAnswer ;
    }
    
    return theAnswer ;
}

#pragma mark - Sockets



static volatile bool DBGrunned=false;

void *DBGlog_Thread(void* param)
{
    if (!DBGrunned)
    {
        DBG("DBG Log thread started!\n");
        DBGrunned=true;
    }
    else
    {
        DBG("DBG Log thread already started!!!!\n");
        return 0;
    }
    UINT32 timer5s = 10;
    do
    {
        
        struct timespec ts ;
        ts.tv_sec = 0 ;
        ts.tv_nsec = (100 % 1000) * 1000000 ;
        nanosleep(&ts, NULL) ;
        
        
        if (!(--timer5s))
        {
            if (gDevice_IOIsRunning)
            {
                /*DBG("Zero=%llu gzts_count=%d", atomic_load(&drv->driverMem->gDevice_ZeroSampleTime), gzts_count);
                //DBG("write_size=%d",write_size);
                PrintStat("delta tx: ",&stat_delta_tx);
                PrintStat("delta rx: ",&stat_delta_rx);*/
            //if (gDevice_IOIsRunning) DBG("EnableWaitFuseCounter=%d",EnableWaitFuseCounter);
                /*char s[1024];
                char *wp=s;
                bzero(s,sizeof(s));
                for(int i=0; i<10; i++)
                {
                    wp+=sprintf(wp,"%08X ",dbg_samples[i]);
                }
                DBG("%s",s);*/
            }
            timer5s = 10;
        }
        
        static bool old_EnableRxWait;
        if (EnableRxWait!=old_EnableRxWait)
        {
            if ((old_EnableRxWait=EnableRxWait))
            {
                DBG("EnableRxWait=true!");
            }
            else
            {
                DBG("EnableRxWait=false!");
            }
        }
        
        if (CHECK_DBG_COUNTER(tooearlyread_count))
        {
            DBG("tooearlyread_count=%d\n", RESET_DBG_COUNTER(tooearlyread_count)) ;
        }
        if (CHECK_DBG_COUNTER(toomuchwait_tx_count))
        {
            DBG("toomuchwait_tx_count=%d\n", RESET_DBG_COUNTER(toomuchwait_tx_count)) ;
        }
        if (CHECK_DBG_COUNTER(toolatewrite_count))
        {
            DBG("toolatewrite_count=%d\n", RESET_DBG_COUNTER(toolatewrite_count)) ;
        }
        if (CHECK_DBG_COUNTER(gapped_rx_count))
        {
            DBG("gapped_rx_count=%d\n", RESET_DBG_COUNTER(gapped_rx_count)) ;
        }
        if (CHECK_DBG_COUNTER(gapped_tx_count))
        {
            DBG("gapped_tx_count=%d\n", RESET_DBG_COUNTER(gapped_tx_count)) ;
        }
        if (CHECK_DBG_COUNTER(rxspinlock_count))
        {
            DBG("rxspinlock_count=%d\n", RESET_DBG_COUNTER(rxspinlock_count)) ;
        }
        if (CHECK_DBG_COUNTER(txspinlock_count))
        {
            DBG("txspinlock_count=%d\n", RESET_DBG_COUNTER(txspinlock_count)) ;
        }
        
        if (drv != NULL && drv->driverMem != NULL) {
            if (atomic_load(&drv->driverMem->ConfigurationChanged[drv->ClientN]) == 1) {
                atomic_store(&drv->driverMem->ConfigurationChanged[drv->ClientN], 0) ;
                
                 dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                         Float64 *newSampleRate = (Float64*)malloc(sizeof(Float64)) ;
                         *newSampleRate = drv->driverMem->sample_frequency ;
                         gPlugIn_Host->RequestDeviceConfigurationChange(gPlugIn_Host, kObjectID_Device, kChangeRequest_SampleFrequency, newSampleRate) ;
                     }
                ) ;
            }
        }
        
        
    } while (!DBGDone);
    DBG("DBG Log thread stopped!\n");
    return 0;
}
