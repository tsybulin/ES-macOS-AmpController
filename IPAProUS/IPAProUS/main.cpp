//
//  main.cpp
//  IPAdrvUS
//
//  Created by Dmytro Oparin on 11/20/20.
//  Copyright © 2020 IPaudio. All rights reserved.
//
#include <CoreFoundation/CoreFoundation.h>
#include <termios.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <mach/thread_act.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <semaphore.h>

#include "global_defs.h"

#include "hacks.h"
#include "drvnames.h"
#include "aiotrx_interface.h"

#include <unistd.h>

#include <sys/mman.h>
#include <fcntl.h>

#include <signal.h>

#define AIO_PORT (0xAD10)

#define MULTICAST_ADDR (0x415049EF)

typedef uint32_t UINT32 ;
typedef int32_t INT32 ;
typedef uint8_t UINT8 ;
typedef uint16_t UINT16 ;
typedef uint32_t DWORD;

#pragma pack(push)
//#pragma pack(1)

#define AIO_QBLOCKS (32)

#define F_OOO (1UL<<1)
#define F_RTX (1UL<<2)
#define F_DATA (1UL<<3)
#define F_SYNC (1UL<<4)

typedef struct
{
    UINT8 chans; //Количество каналов
    UINT8 flags; //Флаги
    UINT16 spare;
    UINT32 spare2;
    UINT32 SEQ; //Номер фрейма
    UINT32 ACK; //Номер ответа
    CHANNEL_SAMPLES_BLOCK block[MAX_CHANNELS_FROM_DEVICE]; //Макс. количество каналов
}AIO_PAYLOAD;

typedef struct
{
    UINT32 connected;
    UINT32 sync;
    UINT32 sample_freq;
    UINT32 frame_number;
    UINT32 txfrt_count;
    UINT32 rxfrt_count;
    UINT32 min_q;
    UINT32 max_q;
    UINT32 avg_q;
    UINT32 underruns;
    UINT32 spare;
}UDP_STATS_AIO;



typedef struct
{
    UINT32 base;
    volatile UINT32 count;
}DBG_COUNT;

typedef struct
{
    bool used;
    unsigned int devn;
    char addr[128];
    unsigned int hwin;
    unsigned int hwout;
    UINT8 state;
    volatile UINT8 visible;
    volatile UINT8 visible_now;
    UINT32 stream_addr;
    UINT32 SEQ;
    UINT32 ACK;
    UINT32 SEQs[AIO_QBLOCKS];
    //Bases for first device channel in buffers
    UINT32 basein;
    UINT32 baseout;
    //Отладочные флаги
    UINT8 dbg_connected;
    UINT8 dbg_outofrange;
    UINT8 dbg_wdtfault;
    UINT8 dbg_connectfault;
    UINT8 dbg_connectfault_reported;
    DBG_COUNT rxfrtreq_count; //Всего запрошено перепосылок нами
    DBG_COUNT rxfrt_count; //Всего принято перепосылок
    DBG_COUNT txfrtreq_count; //Всего запрошено перепосылок от нас
    DBG_COUNT txfrt_count; //Всего отправлено перепосылок
    DBG_COUNT txbusy_count; //Счетчик занятых блоков
    DBG_COUNT txbusy32_count; //Счетчик занятых блоков для быстрого ответа
    //
    UDP_STATS_AIO current_stat;
    UDP_STATS_AIO base_stat;
} S_IO;

S_IO sio[MAX_DEVICES];

S_IO *FindSIObyIP(UINT32 ip)
{
    S_IO *s=sio;
    for(size_t i=0; i<MAX_DEVICES; i++, s++)
    {
        if (s->used && s->stream_addr==ip)
        {
            return s;
        }
    }
    return NULL;
}

volatile UINT32 MaxSEQ; //Вообще максимальный SEQ, который щас может быть
volatile UINT32 MinReceivedSEQ; //Минимально принятый SEQ

UINT32 TailBlocks = 6;
UINT32 WDTblocks = 16;

static UINT32 zeroframe_SEQ;
static UINT32 curtx_SEQ;
static UINT32 currx_SEQ;

#pragma pack(pop)

//States and requests
volatile atomic_uint aio_runned;
volatile atomic_uint aio_wdt;
volatile bool something_rxed=false;
volatile atomic_uint clients_run_requests;
volatile bool ReloadConfigRequest;

//====================================================================
// Locks
//====================================================================

AIOTRX_SHARED_MEMORY *mem=NULL;

pthread_mutex_t aio_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stat_lock=PTHREAD_MUTEX_INITIALIZER;


//sem_t *client_locks[MAX_CLIENTS];
sem_t *wait_locks[MAX_CLIENTS][2];

int InitLocks(void)
{
    char sname[256];
    DBG("Init locks...");
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        sem_t *sem;
        /*snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%d",i);
        sem_unlink(sname);
        sem=sem_open(sname,O_CREAT | O_EXCL, 0666, 1);
        if (sem==SEM_FAILED)
        {
            DBG("Can't create %s with error %d!",sname,errno);
            return -1;
        }
        client_locks[i]=sem;*/
        snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%dw0",i);
        sem_unlink(sname);
        sem=sem_open(sname,O_CREAT | O_EXCL, 0666, 1);
        if (sem==SEM_FAILED)
        {
            DBG("Can't create %s with error %d!",sname,errno);
            return -1;
        }
        wait_locks[i][0]=sem;
        snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%dw1",i);
        sem_unlink(sname);
        sem=sem_open(sname,O_CREAT | O_EXCL, 0666, 0);
        if (sem==SEM_FAILED)
        {
            DBG("Can't create %s with error %d!",sname,errno);
            return -1;
        }
        wait_locks[i][1]=sem;
        mem->client_watchdog[i]=10;
    }
    DBG("Locks inited...");
    return 0;
}

void DeinitLocks(void)
{
    char sname[256];
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        //snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%d",i);
        //sem_unlink(sname);
        snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%dw0",i);
        sem_unlink(sname);
        snprintf(sname,256,DRIVER_NAME_US DRIVER_NAME ".c%dw1",i);
        sem_unlink(sname);
    }
}

//====================================================================
// Shared memory
//====================================================================

int InitSharedMemory(void)
{
    int fd;
    DBG("Init shared memory...");
    //mem=(AIOTRX_SHARED_MEMORY*)malloc(sizeof(AIOTRX_SHARED_MEMORY));
    shm_unlink(DRIVER_NAME_US DRIVER_NAME);
    fd=shm_open(DRIVER_NAME_US DRIVER_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd==-1)
    {
        DBG("Can't create shm with error %d!",errno);
        return -1;
    }
    if (ftruncate(fd, sizeof(AIOTRX_SHARED_MEMORY))==-1)
    {
        DBG("Can't ftruncate with error %d!",errno);
        return -1;
    }
    void *addr;
    addr=mmap(NULL, sizeof(AIOTRX_SHARED_MEMORY), PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (addr==MAP_FAILED)
    {
        DBG("Can't mmap with error %d!",errno);
        return -1;
    }
    mem=(AIOTRX_SHARED_MEMORY*)addr;
    bzero(mem,sizeof(AIOTRX_SHARED_MEMORY));
    DBG("Shared memory inited!");
    return 0;
}

void DeinitSharedMemory(void)
{
    void *addr;
    addr=mem;
    mem=NULL;
    munmap(addr, sizeof(AIOTRX_SHARED_MEMORY));
    shm_unlink(DRIVER_NAME_US DRIVER_NAME);
}

volatile bool AllDone;

void stop_signal_handler(int sig, siginfo_t *si, void *uc)
{
    AllDone=true;
}

//===============================================================
// Route and ifaces
//===============================================================
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

int stat_sock;


typedef struct
{
    UINT32 ip;
    struct sockaddr_dl hwadr;
    char name[8];
}WORK_IF;

#define MAX_IFACES (32)

WORK_IF work_ips[MAX_IFACES];
size_t work_ips_hash;
int work_ips_number=0;

static void sdl2a(char *s, size_t maxlen, struct sockaddr_dl *addr)
{
    size_t i=0;
    bzero(s, maxlen);
    char *sp=s;
    char *p=addr->sdl_data;
    i=addr->sdl_nlen;
    if (i>maxlen) i=maxlen;
    while(i){*sp++=*p++;i--;}
    i=addr->sdl_alen;
    while(i)
    {
        sp+=snprintf(sp,s-sp+maxlen,":%02X",(unsigned char)(*p++));
        i--;
    }
}

#include <sys/socket.h>
#include <net/if.h>
#include <net/if_media.h>

static int gmt_socket=-1;

struct ifmediareq IFMREQ;

static int get_media_type(const char *name)
{
    if (gmt_socket<0)
    {
        gmt_socket=socket(AF_INET, SOCK_DGRAM, 0);
        if (gmt_socket<0) return 0;
    }
    bzero(&IFMREQ,sizeof(IFMREQ));
    strcpy(IFMREQ.ifm_name,name);
    if (ioctl(gmt_socket,SIOCGIFMEDIA,(caddr_t)&IFMREQ)<0)
    {
        return 0;
    }
    return IFMREQ.ifm_current;
}


int EnumWorkIP(WORK_IF *ip_list, size_t *out_hash, bool verbose)
{
    //errno_t err;
    int sz=0;
    if (verbose) DBG("Search all work interfaces..");
    //Search all work ifaces
    
    struct ifaddrs *ifaces=NULL;
    u_int32_t ifaces_count=0;
    
    if (getifaddrs(&ifaces))
    {
        /*if (verbose) */DBG("ifnet_list_get error %d",errno);
        return -1;
    }
    if (!ifaces)
    {
        DBG("ifnet_list_get return empty list!");
        return -1;
    }
    size_t hash;
    hash=0;
    ifaddrs *iface;
    iface=ifaces;
    ip_list[sz].name[0]=0;
    bool fip,fhw;
    fip=false;
    fhw=false;
    
    for(;;)
    {
        if (!iface || strcmp(ip_list->name,iface->ifa_name))
        {
            if (fip && fhw)
            {
                //We can add interface to list
                char s[256];
                sdl2a(s,sizeof(s),&ip_list->hwadr);
                int mt;
                mt=get_media_type(ip_list->name);
                if (verbose)
                {
                    DBG(" Name: %s, type %08X",ip_list->name,mt);
                    UINT32 ip;
                    ip=ip_list->ip;
                    if (mt==0x80)
                    {
                        DBG(" skipped as wireless...");
                        goto L_skip;
                    }
                    DBG(" added to list as %d.%d.%d.%d",(ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
                    DBG(" as interface %s",s);
                }
                if (mt==0x80) goto L_skip;
                char *p;
                char c;
                p=s;
                while((c=*p++)!=0) hash=hash*37+c;
                ip_list++;
                sz++;
                if (sz>=MAX_IFACES)
                {
                    if (verbose)
                    {
                        DBG("Too many ifaces!");
                    }
                    break;
                }
            }
            else
            {
                if (fip || fhw)
                {
                    if (verbose) DBG(" skipped (no ip & hw)...");
                }
            }
        L_skip:
            if (!iface) break;
            //Set new
            strcpy(ip_list->name,iface->ifa_name);
            fip=false;
            fhw=false;
        }
        if (verbose) DBG("%d: %s",ifaces_count,iface->ifa_name);
        if (strncmp(iface->ifa_name,"en",2))
        {
            if (verbose) DBG(" skipped (not ethernet)...");
        }
        /*else if (iface->ifa_name[2]=='1')
        {
            if (verbose) DBG(" skipped (wireless)...");
        }*/
        else
        {
            struct sockaddr_dl addr;
            memcpy(&addr,iface->ifa_addr,sizeof(addr));
            UINT32 ip;
            ip=((sockaddr_in*)&addr)->sin_addr.s_addr;
            if (addr.sdl_family==AF_LINK)
            {
                memcpy(&ip_list->hwadr,&addr,sizeof(ip_list->hwadr));
                if (verbose)
                {
                    char s[256];
                    sdl2a(s,sizeof(s),&ip_list->hwadr);
                    DBG(" HW addr %s",s);
                }
                fhw=true;
            }
            if (addr.sdl_family==AF_INET)
            {
                hash=hash*37+ip;
                ip_list->ip=ip;
                if (verbose)
                {
                    DBG(" IP addr %d.%d.%d.%d",(ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
                }
                fip=true;
            }
        }
        iface=iface->ifa_next;
        ifaces_count++;
    }
    freeifaddrs(ifaces);
    if (sz>=0) *out_hash=hash;
    return sz;
}

void MulticastAdd(WORK_IF *ip_list, unsigned int sz)
{
    //errno_t err;
    while(sz)
    {
        UINT32 ip;
        ip=ip_list->ip;
        DBG("Add multicast on addr=%d.%d.%d.%d",(ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        ip_mreq mreq;
        mreq.imr_multiaddr.s_addr=MULTICAST_ADDR;
        mreq.imr_interface.s_addr=ip;
        if (setsockopt(stat_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)))
        {
            DBG("sock_setsockpopt(IP_ADD_MEMBERSHIP) error %d",errno);
        }
        ip_list++;
        sz--;
    }
}

void MulticastDrop(WORK_IF *ip_list, unsigned int sz)
{
    //errno_t err;
    while(sz)
    {
        UINT32 ip;
        ip=ip_list->ip;
        DBG("Drop multicast on addr=%d.%d.%d.%d",(ip>>0)&0xFF,(ip>>8)&0xFF,(ip>>16)&0xFF,(ip>>24)&0xFF);
        ip_mreq mreq;
        mreq.imr_multiaddr.s_addr=MULTICAST_ADDR;
        mreq.imr_interface.s_addr=ip;
        if (setsockopt(stat_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)))
        {
            DBG("sock_setsockpopt(IP_DROP_MEMBERSHIP) error %d",errno);
        }
        ip_list++;
        sz--;
    }
}

pthread_t iface_thread;
volatile bool iface_thread_done=false;

#include <net/route.h>

struct {
    struct    rt_msghdr m_rtm;
    char    m_space[512];
} m_rtmsg;

#define ROUNDUP(a) \
((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))
#define ADVANCE(x, n) (x += ROUNDUP((n)->sa_len))

void DoRoute(struct sockaddr_dl *ifa, uint cmd)
{
    static uint32_t seq;
#define rtm m_rtmsg.m_rtm
    size_t l;
    char *cp=m_rtmsg.m_space;
    
    struct sockaddr_in a;
    
    sdl2a(cp,256,ifa);
    DBG("Route cmd=%d gateway=%s",cmd,cp);
    
    bzero(&a,sizeof(a));
    a.sin_family=AF_INET;
    bzero((char *)&m_rtmsg, sizeof(m_rtmsg));
    
#define STOREADDR(u) do{ l = ROUNDUP(((struct sockaddr *)(u))->sa_len); bcopy((char *)(u), cp, l); cp += l; }while(0)
    
    rtm.rtm_type=cmd;//RTM_ADD;
    rtm.rtm_flags=RTF_UP|RTF_STATIC;
    rtm.rtm_version=RTM_VERSION;
    rtm.rtm_seq=++seq;
    rtm.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;
    bzero(&rtm.rtm_rmx,sizeof(rtm.rtm_rmx));
    rtm.rtm_inits=0;
    rtm.rtm_index=0;
    
    a.sin_len=sizeof(a);
    a.sin_addr.s_addr=IPADDR(44, 16, 0, 0); //DST
    STOREADDR(&a);
    STOREADDR(ifa); //GATEWAY
    a.sin_len=sizeof(a)-2;
    a.sin_addr.s_addr=IPADDR(255, 255, 0, 0); //MASK
    STOREADDR(&a);
    rtm.rtm_msglen = l = cp - (char *)&m_rtmsg;
    //Write to router socket
    
    //errno_t err;
    
    int router_sock;
    
    router_sock=socket(PF_ROUTE, SOCK_RAW, 0);
    
    if (router_sock<0)
    {
        DBG("socket(router_sock) error %d!",errno);
    }
    else
    {
        if (send(router_sock,(char*)&m_rtmsg,l,0)<0)
        {
            DBG("sock_send(router_sock) error %d",errno);
        }
        else
            DBG("sock_send(router_sock) ok");
        close(router_sock);
    }
    DBG("Route cmd done!");
}

volatile bool RouteDone;
volatile bool DoRoute_req;
struct sockaddr_dl sdl_RouteTo;

volatile bool iface_wdt;

void *IfaceThread(void *arg)
{
    DBG("IfaceThread started...");
L_rescan:
    int n;
    n=EnumWorkIP(work_ips, &work_ips_hash,true);
    if (n>=0)
    {
        DBG("Found %d work interfaces!",n);
    }
    else
    {
        usleep(500000);
        if (iface_thread_done) goto L_exit;
        goto L_rescan;
    }
    work_ips_number=n;
    MulticastAdd(work_ips, work_ips_number);
    //Test
    //DoRoute(&work_ips[0].hwadr,RTM_ADD);
    do
    {
        WORK_IF new_ips[MAX_IFACES];
        int new_ips_num;
        size_t new_hash;
        iface_wdt=false;
        usleep(100000);
        if (!iface_wdt)
        {
            //DBG("?");
            new_ips_num=EnumWorkIP(new_ips, &new_hash,false);
            if (new_ips_num>=0)
            {
                if (new_ips_num!=work_ips_number || new_hash!=work_ips_hash)
                {
                    DBG("Something in interfaces changed, rescan...");
                    MulticastDrop(work_ips, work_ips_number);
                    if (RouteDone)
                    {
                        DoRoute(&sdl_RouteTo,RTM_DELETE);
                        RouteDone=false;
                    }
                    //iface_threads_number=0;
                    goto L_rescan;
                }
            }
        }
        if (DoRoute_req)
        {
            DoRoute_req=false;
            DoRoute(&sdl_RouteTo,RTM_ADD);
        }
        S_IO *s;
        s=sio;
        for(size_t i=0; i<MAX_DEVICES; i++, s++)
        {
            if (s->visible_now)
            {
                s->visible_now=0;
                DBG("Device %s now visible.",s->addr);
            }
            if (s->visible)
            {
                if (!(--s->visible))
                {
                    if (s->used) DBG("Device %s now invisible!",s->addr);
                }
            }
        }
    }
    while(!iface_thread_done);
L_exit:
    DBG("IfaceThread exit!");
    if (RouteDone)
    {
        DoRoute(&sdl_RouteTo,RTM_DELETE);
    }
    //DoRoute(&work_ips[0].hwadr,RTM_DELETE);
    iface_thread_done=false;
    return 0;
}

//==============================================================================================
// Load config
//==============================================================================================

void UseDevice(UINT32 devn, UINT32 ip, UINT32 hwin, UINT32 hwout)
{
    if (devn >= MAX_DEVICES) return;
    S_IO* s = sio + devn;
    s->stream_addr=ip;
    if (!ip)
    {
        s->addr[0]=0;
        s->used=false;
        return;
    }
    
    snprintf(s->addr,sizeof(s->addr),"%d.%d.%d.%d",
             (ip >> 0) & 0xFF,
             (ip >> 8) & 0xFF,
             (ip >> 16) & 0xFF,
             (ip >> 24) & 0xFF
             );
    
    s->hwin = hwin;
    s->hwout = hwout;
    s->used = true;
}

#define CHANNEL_IDX(DEVICE,CHAN) (((DEVICE)*MAX_CHANNELS_FROM_DEVICE+(CHAN)))

void ComputeBases(void)
{
    UINT32 basein,baseout;
    basein=0;
    baseout=0;
    for(unsigned int i=0; i<MAX_DEVICES; i++)
    {
        S_IO* s = sio + i;
        s->basein=basein;
        s->baseout=baseout;
        if (s->used)
        {
            for(unsigned int ch=0; ch<s->hwin; ch++)
            {
                snprintf(mem->input_names[basein+ch].name, sizeof(mem->input_names[0]), "%s in %d",mem->dev_config.devcfg[i].name,ch+1);
            }
            for(unsigned int ch=0; ch<s->hwout; ch++)
            {
                snprintf(mem->output_names[baseout+ch].name, sizeof(mem->output_names[0]), "%s out %d",mem->dev_config.devcfg[i].name,ch+1);
            }
            basein+=s->hwin;
            baseout+=s->hwout;
        }
    }
    mem->base_bus_input=-1;
    mem->base_bus_output=-1;
    if (basein<=(TOTAL_CHANNELS+TOTAL_BUSES) && baseout<=(TOTAL_CHANNELS+TOTAL_BUSES))
    {
        mem->base_bus_input=basein;
        mem->base_bus_output=baseout;
        for(unsigned int ch=0; ch<TOTAL_BUSES; ch++)
        {
            snprintf(mem->input_names[basein+ch].name, sizeof(mem->input_names[0]), "Bus in %d",ch+1);
            snprintf(mem->output_names[baseout+ch].name, sizeof(mem->output_names[0]), "Bus out %d",ch+1);
        }
        basein+=TOTAL_BUSES;
        baseout+=TOTAL_BUSES;
    }
    mem->num_inputs=basein;
    mem->num_outputs=baseout;
}

size_t strcspn(const char *s1, const char *s2)
{
    /*register */const char *p, *spanp;
    /*register */char c, sc;
    
    /*
     * Stop as soon as we find any character from s2.  Note that there
     * must be a NUL in s2; it suffices to stop when we find that, too.
     */
    for (p = s1;;) {
        c = *p++;
        spanp = s2;
        do {
            if ((sc = *spanp++) == c)
                return (p - 1 - s1);
        } while (sc != 0);
    }
    /* NOTREACHED */
}

char * strsep (char **stringp, const char *delim)
{
    char *begin, *end;
    begin = *stringp;
    if (begin == NULL)
        return NULL;
    /* Find the end of the token.  */
    end = begin + strcspn (begin, delim);
    if (*end)
    {
        /* Terminate the token and set *STRINGP past NUL character.  */
        *end++ = '\0';
        *stringp = end;
    }
    else
    /* No more delimiters; this is the last token.  */
        *stringp = NULL;
    return begin;
}

void LoadConfig(void)
{
    //Clear
    for(uint idx=0; idx<MAX_DEVICES; idx++)
    {
        mem->dev_config.devcfg[idx].hw_inputs=0;
        mem->dev_config.devcfg[idx].hw_outputs=0;
        mem->dev_config.devcfg[idx].stream_addr=0;
        bzero(mem->dev_config.devcfg[idx].name,sizeof(mem->dev_config.devcfg[0].name));
    }
    //Default values
    mem->dev_config.RxPrerollSamples=32;
    mem->dev_config.TailBufferSize=48;
#ifdef SUPERSIMPLE
    //Hardcoded version
    mem->dev_config.devcfg[0].hw_inputs=0;
    mem->dev_config.devcfg[0].hw_outputs=2;
    mem->dev_config.devcfg[0].stream_addr=IPADDR(44, 24, 0, 1);
    strcpy(mem->dev_config.devcfg[0].name,"AudioIOusbAmp");
    strcpy(mem->ConfigName,"UsbAmp");
    mem->dev_config.TailBufferSize=240;
#else
    char *config;
    config=(char*)malloc(65536);
    if (!config)
    {
        DBG("IOMalloc() error!");
        goto L_abort;
    }
    DBG("Try to load config file...");
    FILE *f;
    f=fopen("/Users/Shared/IPAudioPro/IPAudioPro.cfg","rb");
    if (!f)
    {
        DBG("Config file not found!");
        goto L_abort;
    }
    int len;
    len=0;
    len=(int)fread(config,1,65536,f);
    fclose(f);
    DBG("%d bytes read",len);
    if (len<0 || len>65535)
    {
        DBG("Incorrect length!");
        goto L_abort;
    }
    config[len]=0;
    char *configp;
    char *current_line;
    configp=config;
    while((current_line=strsep(&configp,"\n"))!=NULL)
    {
        char *name;
        char *value=current_line;
        name=strsep(&value,"=");
        if (name && value)
        {
            //DBG("%s=%s\n",name,value);
            if (!strcasecmp(name, "ConfigName"))
            {
                strncpy(mem->ConfigName,value,127);
                DBG("ConfigName=%s",mem->ConfigName);
            }
            if (!strcasecmp(name, "TailBufferSize"))
            {
                uint32_t val;
                val=(uint32_t)strtoul(value, NULL, 10);
                DBG("TailBufferSize=%d",val);
                val&=~7;
                if (val<8) val=8;
                if (val>248) val=248;
                mem->dev_config.TailBufferSize=val;
            }
            for (size_t dev = 0; dev < MAX_DEVICES; dev++)
            {
                char stest[128];
                strncpy(stest, "Name0", 128);
                stest[4] = '1' + dev;
                if (!strcasecmp(name, stest))
                {
                    strncpy(mem->dev_config.devcfg[dev].name,value,127);
                    DBG("CONFIG NAME=%s",mem->dev_config.devcfg[dev].name);
                }
                strncpy(stest, "Addr0", 128);
                stest[4] = '1' + dev;
                if (!strcasecmp(name, stest))
                {
                    UINT32 ip=0;
                    char *s1;
                    while((s1=strsep(&value,"."))!=NULL)
                    {
                        ip=(ip<<8)|(UINT32)strtoul(s1,NULL,10);
                    }
                    ip=ntohl(ip);
                    mem->dev_config.devcfg[dev].stream_addr=ip;
                    if (ip)
                    {
                        DBG("CONFIG ADDR%ld = %d.%d.%d.%d", dev + 1,
                            (ip >> 0) & 0xFF,
                            (ip >> 8) & 0xFF,
                            (ip >> 16) & 0xFF,
                            (ip >> 24) & 0xFF
                            );
                    }
                    else
                    {
                        DBG("CONFIG ADDR%ld = not used", dev + 1);
                    }
                }
                strncpy(stest, "In0", 128);
                stest[2] = '1' + dev;
                if (!strcasecmp(name, stest)) { uint32_t v; v = (uint32_t)strtoul(value,NULL,10); if (v < 0) v = 0; if (v > 16) v = 16; mem->dev_config.devcfg[dev].hw_inputs=v; DBG("CONFIG IN%ld = %d", dev + 1, v); }
                strncpy(stest, "Out0", 128);
                stest[3] = '1' + dev;
                if (!strcasecmp(name, stest)) { uint32_t v; v = (uint32_t)strtoul(value,NULL,10); if (v < 0) v = 0; if (v > 16) v = 16; mem->dev_config.devcfg[dev].hw_outputs=v; DBG("CONFIG OUT%ld = %d", dev + 1, v); }
            }
        }
    }
    DBG("File parse complete!");
L_abort:
    if (config) free(config);
#endif
}

void PingTxThreads(void)
{
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        if (atomic_load(mem->client_states+i))
        {
            sem_post(wait_locks[i][1]);
        }
    }
}

void InitAllAIO(void)
{
    for (unsigned int devn = 0; devn < MAX_DEVICES; devn++)
    {
        sio[devn].devn = devn;
    }
    for (unsigned int devn = 0; devn < MAX_DEVICES; devn++)
    {
        sio[devn].dbg_connected = 0;
        sio[devn].dbg_connectfault = 0;
        sio[devn].dbg_connectfault_reported = 0;
        sio[devn].dbg_outofrange = 0;
        sio[devn].dbg_wdtfault = 0;
        sio[devn].used = false;
    }
    LoadConfig();
    for (unsigned int devn=0; devn<MAX_DEVICES; devn++)
    {
        UseDevice(devn,
                  mem->dev_config.devcfg[devn].stream_addr,
                  mem->dev_config.devcfg[devn].hw_inputs,
                  mem->dev_config.devcfg[devn].hw_outputs
                  );
        if (sio[devn].used) DBG("%s (%s) in use",mem->dev_config.devcfg[devn].name,sio[devn].addr);
    }
    TailBlocks=mem->dev_config.TailBufferSize/AIO_BLOCK_SZ;
    mem->input_latency=5;
    mem->output_latency=6+TailBlocks*AIO_BLOCK_SZ;
    ComputeBases();
}

//==============================================================================================
// AIO socket
//==============================================================================================
pthread_t aio_sock_tid;

int aio_sock;

void ResetEnableWait(void)
{
    for(unsigned int i=0; i<MAX_CLIENTS; i++)
    {
        atomic_store(&mem->clients_output[i].need_wait,0);
    }
}

/*#include <x86intrin.h>
 
 static inline __m128i selectb(__m128i s, __m128i a, __m128i b) {
 #if 1   // SSE4.1 supported
 return _mm_blendv_epi8(b, a, s);
 #else
 return _mm_or_si128(
 _mm_and_si128(s, a),
 _mm_andnot_si128(s, b));
 #endif
 }
 
 static inline __m128i add_saturated(__m128i a, __m128i b) {
 __m128i sum = _mm_add_epi32(a, b);                  // a + b
 __m128i axb = _mm_xor_si128(a, b);                  // check if a and b have different sign
 __m128i axs = _mm_xor_si128(a, sum);                // check if a and sum have different sign
 __m128i overf1 = _mm_andnot_si128(axb, axs);            // check if sum has wrong sign
 __m128i overf2 = _mm_srai_epi32(overf1, 31);            // -1 if overflow
 __m128i asign = _mm_srli_epi32(a, 31);                 // 1  if a < 0
 __m128i sat1 = _mm_srli_epi32(overf2, 1);             // 7FFFFFFF if overflow
 __m128i sat2 = _mm_add_epi32(sat1, asign);            // 7FFFFFFF if positive overflow 80000000 if negative overflow
 return  selectb(overf2, sat2, sum);                      // sum if not overflow, else sat2
 }*/

static inline int32_t addsat(int32_t a, int32_t b)
{
    int64_t r=a;
    r+=b;
    if (r>0x7FFFFFFFLL) r=0x7FFFFFFFLL;
    if (r<-0x80000000LL) r=-0x80000000LL;
    return (int32_t) r;
}

void SendBlock(S_IO* s, UINT32 SEQ, UINT32 ACK, UINT8 flags)
{
    AIO_PAYLOAD pkt;
    
    pkt.SEQ = SEQ;
    pkt.ACK = ACK;
    pkt.flags = flags;
    int len;
    if (flags & F_DATA)
    {
#if MAX_CLIENTS != 4
#error Please write new mixer code for MAX_CLIENTS!=4
#endif
        pkt.chans = s->hwout;
        int32_t *s0,*s1,*s2,*s3;
        size_t chidx,disp;
        chidx=s->baseout;
        disp=(SEQ % MAX_AIO_BLOCKS);
        s0=mem->clients_output[0].output_channels[chidx].blocks[disp].samples;
        s1=mem->clients_output[1].output_channels[chidx].blocks[disp].samples;
        s2=mem->clients_output[2].output_channels[chidx].blocks[disp].samples;
        s3=mem->clients_output[3].output_channels[chidx].blocks[disp].samples;
        //Заполним данными блок для передачи
        //size_t disp = TotalTxChannels;
        //INT32* source = mem->AudioTxBuffer + s->txsamples + (((SEQ - BaseSEQ) * AIO_BLOCK_SZ) % MAX_ASIO_BUFFER) * disp;
        for(UINT32 i=0; i<s->hwout; i++)
        {
            INT32* dest = pkt.block[i].samples;
            dest[0] = addsat( addsat( s0[0],s1[0]) , addsat(s2[0],s3[0]) );
            dest[1] = addsat( addsat( s0[1],s1[1]) , addsat(s2[1],s3[1]) );
            dest[2] = addsat( addsat( s0[2],s1[2]) , addsat(s2[2],s3[2]) );
            dest[3] = addsat( addsat( s0[3],s1[3]) , addsat(s2[3],s3[3]) );
            dest[4] = addsat( addsat( s0[4],s1[4]) , addsat(s2[4],s3[4]) );
            dest[5] = addsat( addsat( s0[5],s1[5]) , addsat(s2[5],s3[5]) );
            dest[6] = addsat( addsat( s0[6],s1[6]) , addsat(s2[6],s3[6]) );
            dest[7] = addsat( addsat( s0[7],s1[7]) , addsat(s2[7],s3[7]) );
            /*dest[0]=s1[0];
             dest[1]=s1[1];
             dest[2]=s1[2];
             dest[3]=s1[3];
             dest[4]=s1[4];
             dest[5]=s1[5];
             dest[6]=s1[6];
             dest[7]=s1[7];*/
            
            s0+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
            s1+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
            s2+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
            s3+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
        }
        len = offsetof(AIO_PAYLOAD, block) + sizeof(CHANNEL_SAMPLES_BLOCK) * s->hwout;
    }
    else
    {
        if (flags & F_SYNC)
        {
            pkt.chans = /*current_config.TailBufferSize/AIO_BLOCK_SZ;//*/TailBlocks;
        }
        else
            pkt.chans = 0;
        len = offsetof(AIO_PAYLOAD, block);
    }
    if (s->visible)
    {
        struct sockaddr_in addr;
        bzero(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(AIO_PORT);
        addr.sin_addr.s_addr = s->stream_addr;
        addr.sin_len=sizeof(addr);
        struct iovec iov;
        iov.iov_base=&pkt;
        iov.iov_len=len;
        struct msghdr outmsg;
        outmsg.msg_iov=&iov;
        outmsg.msg_iovlen=1;
        outmsg.msg_control=NULL;
        outmsg.msg_controllen=0;
        outmsg.msg_name=&addr;
        outmsg.msg_namelen=sizeof(addr);
        //mbuf_t mb;
        ssize_t sentlen=len;
        //DBG("mbuf %p aio_sock %p\n",mb,aio_sock);
        sentlen=sendmsg(aio_sock,&outmsg,0);
        if (sentlen<0)
        {
            //if (err!=EAGAIN)
            {
                DBG("sock_sendmbuf error %d",errno);
            }
        }
    }
    //DBG("sentlen %zu\n",sentlen);
}

void StartIO(S_IO *s, UINT32 start_seq)
{
    if (!s->used) return;
    s->state = 1; //Приготовление к соединению
    s->SEQ = s->ACK = start_seq;
    SendBlock(s, start_seq, start_seq, F_SYNC);
}

UINT32 MaxBusSEQ;

int AIOtx(void)
{
    UINT32 min_ready_seq = MaxSEQ + 1;
    UINT32 max_tx_seq = MaxSEQ + 1; //Максимально разрешенный для передачи SEQ
    INT32 delta;
    //Ищем минимальный SEQ готовых к передаче данных
    for(UINT32 i=0; i<MAX_CLIENTS; i++)
    {
        UINT32 rs;
        rs=atomic_load(&mem->clients_output[i].readySEQ);
        if (atomic_load(&mem->client_states[i])==2 && atomic_load(&mem->clients_output[i].need_wait))
        {
            delta = rs - min_ready_seq;
            if (delta<0) min_ready_seq=rs;
        }
    }
    delta=min_ready_seq-max_tx_seq;
    if (delta<0) max_tx_seq=min_ready_seq; //Если минимально готовый меньше, то можно передать не более, чем он
    bool something_txed;
    do
    {
        something_txed = false; //Хоть что-то передавали
        for (unsigned int devn = 0; devn < MAX_DEVICES; devn++)
        {
            S_IO* s = sio + devn;
            if (!s->used) continue;
            if (!s->state) continue; //Обрабатываем всех, кто не оффлайн
            UINT32 seq = s->SEQ;
            delta = seq - max_tx_seq;
            if (delta >= 0) continue; //Если наш seq добежал или перебежал max_tx_seq - ничего не передаем
            s->SEQ = seq + 1;
            SendBlock(s, seq, s->ACK, F_DATA);
            something_txed = true;
        }
    }
    while (something_txed);
    //Process buses
    UINT32 seq;
    seq = MaxBusSEQ;
    if (mem->base_bus_input<0 || mem->base_bus_output<0)
    {
        seq=MaxSEQ+1;
    }
    else
    {
        for(;;)
        {
            delta = seq - max_tx_seq;
            if (delta >= 0) break;
#if MAX_CLIENTS != 4
#error Please write new mixer code for MAX_CLIENTS!=4
#endif
            int32_t *s0,*s1,*s2,*s3;
            size_t chidx,disp;
            chidx=mem->base_bus_output;
            disp=(seq % MAX_AIO_BLOCKS);
            s0=mem->clients_output[0].output_channels[chidx].blocks[disp].samples;
            s1=mem->clients_output[1].output_channels[chidx].blocks[disp].samples;
            s2=mem->clients_output[2].output_channels[chidx].blocks[disp].samples;
            s3=mem->clients_output[3].output_channels[chidx].blocks[disp].samples;
            //Заполним данными блок для передачи
            //size_t disp = TotalTxChannels;
            //INT32* source = mem->AudioTxBuffer + s->txsamples + (((SEQ - BaseSEQ) * AIO_BLOCK_SZ) % MAX_ASIO_BUFFER) * disp;
            int32_t *dest=mem->input_channels[mem->base_bus_input].blocks[disp].samples;
            for(UINT32 i=0; i<TOTAL_BUSES; i++)
            {
                dest[0] = addsat( addsat( s0[0],s1[0]) , addsat(s2[0],s3[0]) );
                dest[1] = addsat( addsat( s0[1],s1[1]) , addsat(s2[1],s3[1]) );
                dest[2] = addsat( addsat( s0[2],s1[2]) , addsat(s2[2],s3[2]) );
                dest[3] = addsat( addsat( s0[3],s1[3]) , addsat(s2[3],s3[3]) );
                dest[4] = addsat( addsat( s0[4],s1[4]) , addsat(s2[4],s3[4]) );
                dest[5] = addsat( addsat( s0[5],s1[5]) , addsat(s2[5],s3[5]) );
                dest[6] = addsat( addsat( s0[6],s1[6]) , addsat(s2[6],s3[6]) );
                dest[7] = addsat( addsat( s0[7],s1[7]) , addsat(s2[7],s3[7]) );
                /*dest[0]=s1[0];
                 dest[1]=s1[1];
                 dest[2]=s1[2];
                 dest[3]=s1[3];
                 dest[4]=s1[4];
                 dest[5]=s1[5];
                 dest[6]=s1[6];
                 dest[7]=s1[7];*/
                
                s0+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
                s1+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
                s2+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
                s3+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
                
                dest+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
            }
            seq++;
            something_txed=true;
        }
    }
    MaxBusSEQ = seq;
    if (something_txed)
        return 1;
    else
        return 0;
}

void InitAIOconnect(UINT32 frame_number)
{
    atomic_store(&mem->CurrentTxSendSample,0);
    ResetEnableWait();
    for (unsigned int devn = 0; devn < MAX_DEVICES; devn++)
    {
        S_IO *s = sio + devn;
        s->state = 0;
    }
    //if (!TimeoutReported) DEBUG_PRINT("*** Try to link ***\n");
    MaxSEQ = (frame_number + 100);
    MinReceivedSEQ = MaxSEQ;
    zeroframe_SEQ = MaxSEQ;
    //pthread_mutex_lock(&gDevice_Input_Mutex);
    atomic_store(&mem->gDevice_ZeroSampleTime,0);
    atomic_store(&mem->gDevice_ZeroHostTime, mach_absolute_time());
    curtx_SEQ = MaxSEQ;
    currx_SEQ = MaxSEQ;
    atomic_store(&mem->readySEQ,MaxSEQ);
    atomic_store(&mem->curtx_SEQ,MaxSEQ);
    MaxBusSEQ = MaxSEQ;
    
    INT32 zfrm;
    
    zfrm=zeroframe_SEQ & ~(MAX_AIO_BUFFER/AIO_BLOCK_SZ-1);
    zfrm=zeroframe_SEQ-zfrm;
    
    int64_t smp;
    smp=AIO_BLOCK_SZ*zfrm;
    atomic_store(&mem->CurrentTxSendSample,smp);
    atomic_store(&mem->CurrentRecvSample,smp);
    //pthread_mutex_unlock(&gDevice_Input_Mutex) ;
    for (unsigned int devn = 0; devn < MAX_DEVICES; devn++)
    {
        S_IO *s = sio + devn;
        if (s->used) StartIO(s, MaxSEQ);
    }
    atomic_store(&aio_runned,1);
    PingTxThreads();
}

int UpdateZeroFrameCount(void)
{
    int f=0;
    while(zeroframe_SEQ != MinReceivedSEQ)
    {
        f=1;
        zeroframe_SEQ++;
        if ((((zeroframe_SEQ)*AIO_BLOCK_SZ) % MAX_AIO_BUFFER)==0)
        {
            //pthread_mutex_lock(&gDevice_Input_Mutex);
            atomic_fetch_add(&mem->gDevice_ZeroSampleTime, MAX_AIO_BUFFER);
            //mem->gDevice_ZeroSampleTime += MAX_AIO_BUFFER;
            atomic_store(&mem->gDevice_ZeroHostTime, mach_absolute_time());
            //pthread_mutex_unlock(&gDevice_Input_Mutex) ;
        }
    }
    return f;
}

int UpdateCurrentTxSendSample(void)
{
    int f=0;
    while(curtx_SEQ!=MaxSEQ)
    {
        f=1;
        curtx_SEQ++;
        atomic_store(&mem->curtx_SEQ,curtx_SEQ);
        atomic_fetch_add(&mem->CurrentTxSendSample,AIO_BLOCK_SZ);
    }
    return f;
}

int UpdateCurrentRecvSample(void)
{
    int f=0;
    while(currx_SEQ!=MinReceivedSEQ)
    {
        f=1;
        currx_SEQ++;
        atomic_fetch_add(&mem->CurrentRecvSample,AIO_BLOCK_SZ);
    }
    return f;
}

int MoveRxWp(void)
{
    int f=0;
    //Перемещаем на новую позицию wp и генерируем event, если надо
    UINT32 seq = MaxSEQ;
    for (unsigned int i = 0; i < MAX_DEVICES; i++)
    {
        S_IO *s = sio + i;
        if (!s->used) continue;
        if (s->state != 2) continue; //Пока не подключился - нефиг нам там что-то подбирать
        if ((INT32)(s->ACK - seq) < 0) seq = s->ACK; //Кто-то еще не добрался до топа
    }
    if ((INT32)(MaxBusSEQ - seq) < 0) seq = MaxBusSEQ; //Кто-то из шин не добрался до топа
    if (MinReceivedSEQ!=seq)
    {
        f=1;
        MinReceivedSEQ=seq;
    };
    atomic_store(&mem->readySEQ,seq);
    for(unsigned int i=0; i<MAX_CLIENTS; i++)
    {
        if (atomic_load(&mem->client_states[i])==2)
        {
            if ((INT32)(atomic_load(&mem->waitSEQ[i])-seq)<=0)
            {
                if (sem_trywait(wait_locks[i][0]))
                {
                    if (errno==EAGAIN)
                    {
                        //Его там уже ждут
                        atomic_store(&mem->waitSEQ[i],seq+0x10000000);
                        sem_post(wait_locks[i][0]); //Отпустим тот поток
                    }
                    else
                    {
                        DBG("sem_wait client %d failed, error %d",i,errno);
                    }
                }
                else
                {
                    //Мы его сами сдуру заняли, отпускаем
                    sem_post(wait_locks[i][0]);
                }
            }
        }
    }
    return f;
}


void AIOrx(S_IO* s, AIO_PAYLOAD* p)
{
    UINT32 seq;
    UINT32 ack;
    UINT8 flags;
    seq = p->SEQ;
    ack = s->ACK;
    INT32 delta = seq - ack;
    if (delta >= AIO_QBLOCKS)
    {
        //Переполнилась очередь в передатчике, так что можно рвать связь
        s->state = 0;
        s->dbg_outofrange = 1;
        return;
    }
    if (s->state != 2)
    {
        //ESEvent_Set(rxRedy);
        s->state = 2;
        s->dbg_connected = 1;
        if (!something_rxed)
        {
            mem->gDevice_ZeroHostTime = mach_absolute_time();
            something_rxed=true;
        }
    }
    flags = 0;
    if (p->flags & F_DATA && delta >= 0)
    {
        iface_wdt=true;
        if (p->flags & F_RTX)
        {
            s->rxfrt_count.count++;
        }
        //Place rx'ed data to circular buffers
        s->SEQs[seq % AIO_QBLOCKS] = seq;
        size_t chidx,disp;
        chidx=s->basein;
        disp=(seq % MAX_AIO_BLOCKS);
        int32_t *dest=mem->input_channels[chidx].blocks[disp].samples;
        for (UINT32 i = 0; i < s->hwin; i++)
        {
            INT32* src = p->block[i].samples;
            dest[0]=src[0];
            dest[1]=src[1];
            dest[2]=src[2];
            dest[3]=src[3];
            dest[4]=src[4];
            dest[5]=src[5];
            dest[6]=src[6];
            dest[7]=src[7];
            dest+=MAX_AIO_BLOCKS*AIO_BLOCK_SZ;
        }
        seq++;
        if ((INT32)(seq - MaxSEQ) > 0 && !(p->flags & F_RTX))
        {
            MaxSEQ = seq; //Максимальный SEQ с полезной нагрузкой
        }
        //Move ACK pointer
        while (s->SEQs[ack % AIO_QBLOCKS] == ack)
        {
            ack++;
            if (seq == ack) break;
        }
        if (seq != ack)
        {
            flags |= F_OOO;
            s->rxfrtreq_count.count++;
        }
    }
    s->ACK = ack;
    seq = p->ACK;
    delta = s->SEQ - seq; //Насколько старый блок от нас хотят получить?
    if (p->flags & F_OOO && delta < AIO_QBLOCKS)
    {
        s->txfrtreq_count.count++;
        flags |= F_RTX | F_DATA;
        s->txfrt_count.count++;
    }
    if (flags)
    {
        SendBlock(s, seq, ack, flags);
    }
}


void DispatchReceived(AIO_PAYLOAD *p, ssize_t len, struct sockaddr_in *addr)
{
    UINT32 ip = addr->sin_addr.s_addr;
    if (len < offsetof(AIO_PAYLOAD, block))
    {
        DBG("%d.%d.%d.%d: RIO Error too short packet received from  len=%ld!",
            (ip >> 0) & 0xFF,
            (ip >> 8) & 0xFF,
            (ip >> 16) & 0xFF,
            (ip >> 24) & 0xFF,
            len);
    }
    for (int i = 0; i < MAX_DEVICES; i++)
    {
        S_IO *s = sio + i;
        if (s->used && s->stream_addr == ip)
        {
            AIOrx(s, p);
            atomic_store(&aio_wdt,2);
            //TimeoutReported = false;
            break;
        }
    }
}

/*kern_return_t WaitRXseq(uint32_t client, UINT32 seq)
{
    if (client>=MAX_CLIENTS) return KERN_FAILURE;
    //Раз клиент сюда пришел, то он приготовил какие-то данные для отправки (в том числе bus'ы)
    lck_mtx_lock(aio_lock);
    if (aio_runned)
    {
        AIOtx(); //Посылаем все данные, какие можно
        MoveRxWp(); //После этого проверяем, не надо ли обновить флаги
        UpdateZeroFrameCount();
        UpdateCurrentTxSendSample();
        UpdateCurrentRecvSample();
    }
    lck_mtx_unlock(aio_lock);
    lck_mtx_t *mtx=clients_mutex[client];
    if ((INT32)(curtx_SEQ-seq)>MAX_AIO_BLOCKS) return KERN_FAILURE; //Reset if need wait already tx'ed data
    if ((INT32)(seq-MinReceivedSEQ)<=0) return KERN_SUCCESS;
    lck_mtx_lock(mtx);
    WRxSEQs[client]=seq;
    uint64_t deadline;
    //deadline=mach_absolute_time()+compute_timeout(50); //50 ms enouth
    clock_interval_to_deadline(50,kMillisecondScale,&deadline);
    wait_result_t result;
    result=lck_mtx_sleep_deadline(mtx, LCK_SLEEP_UNLOCK, WRxSEQs+client, THREAD_ABORTSAFE,deadline); //Don't reclaim lock after sleep
    if (result==THREAD_AWAKENED)
        return KERN_SUCCESS;
    else
        return KERN_FAILURE;
}*/


volatile atomic_uint tx_loop_counter;

void TXandUpdateAll(void)
{
    unsigned int lc;
    lc=0;
    int not_done;
    do
    {
        not_done=AIOtx(); //Посылаем все данные, какие можно
        not_done|=MoveRxWp(); //После этого проверяем, не надо ли обновить флаги
        not_done|=UpdateZeroFrameCount();
        not_done|=UpdateCurrentTxSendSample();
        not_done|=UpdateCurrentRecvSample();
        if (not_done) lc++;
    }
    while(not_done);
    if (lc>1)
    {
        atomic_store(&tx_loop_counter,lc);
    }
}

void *aio_sock_thread(void *arg)
{
    struct mach_timebase_info theTimeBaseInfo ;
    mach_timebase_info(&theTimeBaseInfo) ;
    Float64 theHostClockFrequency = (Float64) theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer ;
    theHostClockFrequency *= 1000000000.0 ;
    struct thread_time_constraint_policy ttcp ;
    ttcp.period = theHostClockFrequency / 1000 ;
    ttcp.computation = theHostClockFrequency / 2000 ;
    ttcp.constraint = theHostClockFrequency / 1500 ;
    ttcp.preemptible = 0 ;
    kern_return_t err;
    if ((err=thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&ttcp, THREAD_TIME_CONSTRAINT_POLICY_COUNT)) != 0) {
        DBG("aio_sock_thread set_realtime failed err %d",err) ;
    }

    for(;;)
    {
        //errno_t err;
        AIO_PAYLOAD pkt;
        ssize_t len=sizeof(pkt);
        //mbuf_t data;
        struct msghdr inmsg;
        struct sockaddr_in rxaddr;
        struct iovec iov;
        iov.iov_base=&pkt;
        iov.iov_len=sizeof(pkt);
        rxaddr.sin_len=sizeof(rxaddr);
        inmsg.msg_iov=&iov;
        inmsg.msg_iovlen=1;
        inmsg.msg_control=NULL;
        inmsg.msg_controllen=0;
        inmsg.msg_name=&rxaddr;
        inmsg.msg_namelen=sizeof(rxaddr);
        //data=NULL;
        
        len=recvmsg(aio_sock, &inmsg, 0);
        if (len<0)
        {
            DBG("aio_sock closed?");
            break;
        }
        pthread_mutex_lock(&aio_lock);
        if (atomic_load(&aio_runned))
        {
            //mbuf_copydata(data, 0, len, &pkt);
            DispatchReceived(&pkt, len, &rxaddr);
            //Проверяем сторожевые таймеры и пересоединения к устройствам
            for (unsigned int i = 0; i < MAX_DEVICES; i++)
            {
                S_IO *s = sio + i;
                if (s->used)
                {
                    if (s->state == 2)
                    {
                        //Мы подключены, проверяем сторожевой таймер
                        INT32 delta = MaxSEQ - s->ACK;
                        if (delta > (INT32)WDTblocks)
                        {
                            s->state = 0;
                            s->dbg_wdtfault = 1;
                        }
                    }
                    if (s->state == 1)
                    {
                        INT32 delta = MaxSEQ - s->ACK;
                        if (delta > 10000)
                        {
                            //Не смог подключиться
                            s->state = 0;
                            s->dbg_connectfault = 1;
                        }
                    }
                    if (s->state == 0)
                    {
                        //У нас состояние отключения
                        StartIO(s, MaxSEQ + 10); //Быстрое возобновление
                    }
                }
            }
            TXandUpdateAll();
        }
        pthread_mutex_unlock(&aio_lock);
    }
    DBG("aio_sock thread done!");
    return 0;
}

volatile bool txwakeup_threads_done;

pthread_t txwakeup_ptids[MAX_CLIENTS];

void *txwakeup_thread(void *arg)
{
    int Client=(int)(size_t)arg;
    DBG("txwakeup%d start",Client);
    struct mach_timebase_info theTimeBaseInfo ;
    mach_timebase_info(&theTimeBaseInfo) ;
    Float64 theHostClockFrequency = (Float64) theTimeBaseInfo.denom / (Float64)theTimeBaseInfo.numer ;
    theHostClockFrequency *= 1000000000.0 ;
    struct thread_time_constraint_policy ttcp ;
    ttcp.period = theHostClockFrequency / 1000 ;
    ttcp.computation = theHostClockFrequency / 2000 ;
    ttcp.constraint = theHostClockFrequency / 1500 ;
    ttcp.preemptible = 0 ;
    if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&ttcp, THREAD_TIME_CONSTRAINT_POLICY_COUNT) != 0) {
        DBG("txwakeup%d set_realtime failed",Client) ;
    }

    do
    {
        sem_wait(wait_locks[Client][1]);
        pthread_mutex_lock(&aio_lock);
        if (atomic_load(&aio_runned))
        {
            TXandUpdateAll();
        }
        pthread_mutex_unlock(&aio_lock);
    }
    while(!txwakeup_threads_done);
    DBG("txwakeup%d done",Client);
    return 0;
}


int create_aio_sock(void)
{
    DBG("try to create aio_sock...");
    atomic_store(&aio_wdt,0);
    atomic_store(&aio_runned,0);
    if ((aio_sock=socket(PF_INET, SOCK_DGRAM, 0))<0)
    {
        DBG("sock_socket error %d",errno);
        //return err;
        return -1;
    }
    int val=1;
    val=1;
    if (setsockopt(aio_sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(SO_REUSEADDR) error %d",errno);
        return -1;
    }
    val=1;
    if (setsockopt(aio_sock, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(SO_REUSEPORT) error %d",errno);
        return -1;
    }
    
    /*int sz=sizeof(val);
     val=0;
     if ((err=sock_getsockopt(aio_sock, SOL_SOCKET, SO_RCVLOWAT, &val, &sz)))
     {
     DBG("sock_getsockopt(SO_RCVLOWAT) error %d\n",err);
     return KERN_FAILURE;
     }
     DBG("SO_RCVLOWAT=%d\n",val);*/
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_len=sizeof(addr);
    addr.sin_family=PF_INET;
    addr.sin_port=ntohs(AIO_PORT);
    if (bind(aio_sock, (struct sockaddr *)&addr, sizeof(addr)))
    {
        DBG("sock_bind error %d",errno);
        return -1;
    }
    pthread_create(&aio_sock_tid, NULL, aio_sock_thread, NULL);
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        pthread_create(txwakeup_ptids+i, NULL, txwakeup_thread, (void*)(size_t)i);
    }
    DBG("aio_sock ok!");
    return 0;
}

void close_aio_sock(void)
{
    void *ret;
    if (!aio_sock) return;
    close(aio_sock);
    aio_sock=0;
    DBG("aio_sock closed!");
    pthread_join(aio_sock_tid, &ret);
    DBG("aio_sock_thread stopped!");
    txwakeup_threads_done=true;
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        sem_post(wait_locks[i][1]);
        pthread_join(txwakeup_ptids[i], &ret);
    }
    DBG("txwakeup threads stopped!");
}

//==============================================================================================
// Stat socket
//==============================================================================================
void ConfigurationChangeNotify(void)
{
    for(unsigned int i=0; i<MAX_CLIENTS; i++)
        atomic_store(&mem->ConfigurationChanged[i],1);
}

pthread_t stat_sock_tid;

typedef struct
{
    UINT32 ip;
    UDP_STATS_AIO stat;
}EXT_UDP_STATS_AIO;

void SendStatToLocalhost(int sock, UDP_STATS_AIO *stat, UINT32 ip)
{
    EXT_UDP_STATS_AIO estat[1];
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port=ntohs(0xAD63);
    addr.sin_addr.s_addr=IPADDR(127, 0, 0, 1);
    addr.sin_len=sizeof(addr);
    struct iovec iov;
    iov.iov_base=estat;
    iov.iov_len=sizeof(EXT_UDP_STATS_AIO);
    estat->ip=ip;
    memcpy(&estat->stat,stat,sizeof(UDP_STATS_AIO));
    struct msghdr outmsg;
    outmsg.msg_iov=&iov;
    outmsg.msg_iovlen=1;
    outmsg.msg_control=NULL;
    outmsg.msg_controllen=0;
    outmsg.msg_name=&addr;
    outmsg.msg_namelen=sizeof(addr);
    ssize_t sentlen;
    sentlen=sendmsg(aio_sock, &outmsg, 0);
    if (sentlen<0)
    {
        //if (err!=EAGAIN)
        {
            DBG("sock_send(stat_sock) error %d\n",errno);
        }
    }
}


void *stat_sock_thread(void *arg)
{
    for(;;)
    {
        UDP_STATS_AIO stat;
        ssize_t len=sizeof(stat);
        //mbuf_t mb;
        struct msghdr inmsg;
        struct sockaddr_in rxaddr;
        rxaddr.sin_len=sizeof(rxaddr);
        struct iovec iov;
        iov.iov_base=&stat;
        iov.iov_len=sizeof(stat);
        inmsg.msg_iov=&iov;
        inmsg.msg_iovlen=1;
        inmsg.msg_name=&rxaddr;
        inmsg.msg_namelen=sizeof(rxaddr);
        
        struct
        {
            cmsghdr hdr;
            uint8_t data[128];
        }rxif;
        inmsg.msg_control=&rxif;
        inmsg.msg_controllen=sizeof(rxif);
        
        
        len=recvmsg(stat_sock, &inmsg, 0);
        if (len<=0)
        {
            DBG("stat_sock closed?");
            break;
        }
        if (len!=sizeof(stat))
        {
            DBG("stat_sock incorrect rx size=%zu",len);
            continue;
        }
        pthread_mutex_lock(&stat_lock);
        //DBG("%d",inmsg.msg_controllen);
        if (inmsg.msg_controllen>=sizeof(cmsghdr))
        {
            //DBG("len=%d level=%d type=%d\n",rxif.hdr.cmsg_len,rxif.hdr.cmsg_level,rxif.hdr.cmsg_type);
            if (/*rxif.hdr.cmsg_len==sizeof(struct sockaddr_dl) && */rxif.hdr.cmsg_level==IPPROTO_IP && rxif.hdr.cmsg_type==IP_RECVIF)
            {
                //char s[256];
                //sdl2a(s,256,(sockaddr_dl*)rxif.data);
                //DBG("sdl: %s\n",s);
                if ((rxaddr.sin_addr.s_addr&0x0000FFFF)==IPADDR(44, 16, 0, 0))
                {
                    //Check for routing
                    if (!RouteDone)
                    {
                        RouteDone=true;
                        memcpy(&sdl_RouteTo,rxif.data,sizeof(sdl_RouteTo));
                        DoRoute_req=true;
                    }
                }
            }
        }
        S_IO *s;
        s=sio;
        bool at_least_one_device_visible=false;
        for(size_t i=0; i<MAX_DEVICES; i++, s++)
        {
            if (s->used && s->visible) at_least_one_device_visible=true;
        }
        if (stat.sync==1)
        {
            if (mem->sample_frequency!=stat.sample_freq)
            {
                mem->sample_frequency=stat.sample_freq;
                ConfigurationChangeNotify();
            }
            if (atomic_load(&clients_run_requests))
            {
                if (atomic_load(&aio_runned))
                {
                }
                else
                {
                    if (at_least_one_device_visible)
                    {
                        UINT32 frame;
                        frame=stat.frame_number;
                        pthread_mutex_lock(&aio_lock);
                        InitAIOconnect(frame);
                        pthread_mutex_unlock(&aio_lock);
                    }
                }
            }
            else
            {
                pthread_mutex_lock(&aio_lock);
                atomic_store(&aio_wdt,0);
                atomic_store(&aio_runned,0);
                something_rxed=false;
                ResetEnableWait();
                pthread_mutex_unlock(&aio_lock);
            }
#ifdef SUPERSIMPLE
            if (rxaddr.sin_addr.s_addr!=sio[0].stream_addr)
            {
                DBG("Change device IP!");
                UseDevice(0,rxaddr.sin_addr.s_addr,sio[0].hwin,sio[0].hwout);
            }
#endif
        }
        if ((s=FindSIObyIP(rxaddr.sin_addr.s_addr))!=NULL)
        {
            memcpy(&s->current_stat,&stat,sizeof(s->current_stat));
            if (!s->visible)
            {
                memcpy(&s->base_stat,&stat,sizeof(s->base_stat));
                s->visible_now=1;
            }
            s->visible=10;
        }
        SendStatToLocalhost(stat_sock,&stat,rxaddr.sin_addr.s_addr);
        pthread_mutex_unlock(&stat_lock);
    }
    DBG("stat_sock thread done!");
    return 0;
}

int create_stat_sock(void)
{
    DBG("try to create stat_sock...");
    //rxtrhead=0;
    atomic_store(&aio_wdt,0);
    atomic_store(&aio_runned,0);
    if ((stat_sock=socket(PF_INET, SOCK_DGRAM, 0))<=0)
    {
        DBG("sock_socket error %d",errno);
        return -1;
    }
    int val=1;
    if (setsockopt(stat_sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(SO_REUSEADDR) error %d",errno);
        return -1;
    }
    val=1;
    if (setsockopt(stat_sock, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(SO_REUSEPORT) error %d",errno);
        return -1;
    }
    val=1;
    if (setsockopt(stat_sock, IPPROTO_IP, IP_RECVIF, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(IP_RECVIF) error %d",errno);
        return -1;
    }
    val=1;
    if (setsockopt(stat_sock, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val)))
    {
        DBG("sock_setsockpopt(SO_BROADCAST) error %d",errno);
        //return KERN_FAILURE;
    }
    
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_len=sizeof(addr);
    addr.sin_family=PF_INET;
    addr.sin_port=ntohs(0xAD53);
    addr.sin_addr.s_addr=MULTICAST_ADDR;
    //addr.sin_addr.s_addr=IPADDR(44, 16, 0, 0);
    if (bind(stat_sock, (struct sockaddr *)&addr, sizeof(addr)))
    {
        DBG("sock_bind error %d",errno);
        return -1;
    }
    iface_thread_done=false;
    pthread_create(&stat_sock_tid, NULL, stat_sock_thread, NULL);
    pthread_create(&iface_thread, NULL, IfaceThread, NULL);
    DBG("stat_sock ok!");
    return 0;
}

void close_stat_sock(void)
{
    if (!stat_sock) return;
    close(stat_sock);
    stat_sock=0;
    DBG("stat_sock closed!");
    iface_thread_done=true;
    void *ret;
    pthread_join(iface_thread, &ret);
    while(iface_thread_done);
    pthread_join(stat_sock_tid, &ret);
    DBG("iface_thread stopped!");
}



//==============================================================================================

/*void *AudioProcessingThread(void *arg)
{
    //DBG("AudioProcessingThread start!\n") ;
    struct mach_timebase_info theTimeBaseInfo ;
    mach_timebase_info(&theTimeBaseInfo) ;
    Float64 theHostClockFrequency = theTimeBaseInfo.denom / theTimeBaseInfo.numer ;
    theHostClockFrequency *= 1000000000.0 ;
    struct thread_time_constraint_policy ttcp ;
    ttcp.period = theHostClockFrequency / 160 ;
    ttcp.computation = theHostClockFrequency / 3300 ;
    ttcp.constraint = theHostClockFrequency / 2200 ;
    ttcp.preemptible = 1 ;
    if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&ttcp, THREAD_TIME_CONSTRAINT_POLICY_COUNT) != 0) {
        DBG("AudioProcessingThread set_realtime failed") ;
    }
    int lnum;
    lnum=0;
    do
    {
        for(int i=0; i<MAX_CLIENTS; i++)
        {
            if (mem->client_states[i])
            {
                if (sem_wait(wait_locks[i][lnum]))
                {
                    DBG("sem_wait client %d lnum %d failed error %d!",i,lnum,errno);
                }
                if (sem_post(wait_locks[i][lnum^1]))
                {
                    DBG("sem_wait client %d lnum %d failed error %d!",i,lnum,errno);
                }
            }
        }
        usleep(1000); //Wait 1ms
        lnum^=1;
    }
    while(!AllDone);
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        sem_post(wait_locks[i][0]);
        sem_post(wait_locks[i][0]);
    }
    return 0;
}*/

//====================================================================
// Clients health checker
//====================================================================

unsigned long old_client_states[MAX_CLIENTS];

void CheckClients(void)
{
    unsigned int rcreq;
    rcreq=atomic_load(&mem->ReloadConfigRequest);
    if (atomic_compare_exchange_strong(&mem->ReloadConfigRequest, &rcreq, 0))
    {
        if (rcreq)
        {
            DBG("ReloadConfigRequest received!");
            pthread_mutex_lock(&stat_lock);
            pthread_mutex_lock(&aio_lock);
            if (atomic_load(&aio_runned))
            {
                atomic_store(&aio_wdt,0);
                atomic_store(&aio_runned,0);
                something_rxed=false;
                InitAllAIO();
                ConfigurationChangeNotify();
            }
            else
            {
                InitAllAIO();
            }
            pthread_mutex_unlock(&aio_lock);
            pthread_mutex_unlock(&stat_lock);
        }
    }
    for(int i=0; i<MAX_CLIENTS; i++)
    {
        uint32_t mask;
        unsigned long sts;
        mask=1<<i;
        sts=atomic_load(mem->client_states+i);
        if (sts!=old_client_states[i])
        {
            DBG("Client %d state transition %lu->%lu",i,old_client_states[i],sts);
            old_client_states[i]=sts;
        }
        if (sts>1)
        {
            atomic_fetch_or(&clients_run_requests, mask);
        }
        else
        {
            atomic_fetch_and(&clients_run_requests, ~mask);
        }
        if (sts)
        {
            unsigned int v;
            v=atomic_load(mem->client_watchdog+i);
            if (v)
            {
                atomic_compare_exchange_strong(mem->client_watchdog+i, &v, v-1);
            }
            else
            {
                DBG("Client %d died from state %lu!",i,sts);
                AIO_OUTPUT_BY_CLIENT *tx=mem->clients_output+i;
                tx->need_wait=0;
                bzero(tx->output_channels,sizeof(tx->output_channels));
                atomic_store(mem->client_watchdog+i,100);
                atomic_store(mem->client_states+i,0);
            }
        }
        else
        {
            atomic_store(mem->client_watchdog+i,100);
        }
    }
    //Check global aio_timeout
    //Check wdt
    unsigned int wdt;
    wdt=atomic_load(&aio_wdt);
    if (wdt)
    {
        if (atomic_compare_exchange_strong(&aio_wdt,&wdt, wdt-1))
        {
            if (wdt==1)
            {
                atomic_store(&aio_runned,0);
                DBG("Timeout in AIO detected, restart now!");
            }
        }
    }
    static unsigned int old_aio_runned;
    unsigned int ar;
    ar=atomic_load(&aio_runned);
    if (ar)
    {
        PingTxThreads();
    }
    if (ar!=old_aio_runned)
    {
        if (ar)
            DBG("*** AIO run ***");
        else
            DBG("*** AIO stop ***");
        old_aio_runned=ar;
    }
    unsigned int lc;
    lc=atomic_load(&tx_loop_counter);
    if (atomic_compare_exchange_strong(&tx_loop_counter, &lc, 0))
    {
        if (lc>3)
            DBG("tx_loop_counter=%d",lc);
    }
    //Print stat changes
    for(int i=0; i<MAX_DEVICES; i++)
    {
        S_IO *s=sio+i;
#define PRINT_STAT(name,string) do {if (s->current_stat.name!=s->base_stat.name) {DBG("%s " string " (total %d)",s->addr,s->current_stat.name-s->base_stat.name,s->base_stat.name=s->current_stat.name);}}while(0)
        PRINT_STAT(underruns,"%d underruns");
        PRINT_STAT(rxfrt_count,"%d rxfrt");
        PRINT_STAT(txfrt_count,"%d txfrt");
        
    }
}

//==============================================================================================
// Main
//==============================================================================================

pthread_t ptid_APT;

pthread_mutex_t log_lock=PTHREAD_MUTEX_INITIALIZER;


#include <termios.h>
int Get_Key (void)
{
    int key;
    struct termios oldt, newt;
    tcgetattr( STDIN_FILENO, &oldt); // 1473
    memcpy((void *)&newt, (void *)&oldt, sizeof(struct termios));
    newt.c_lflag &= ~(ICANON);  // Reset ICANON
    newt.c_lflag &= ~(ECHO);    // Echo off, after these two .c_lflag = 1217
    tcsetattr( STDIN_FILENO, TCSANOW, &newt); // 1217
    key=getchar(); // works like "getch()"
    tcsetattr( STDIN_FILENO, TCSANOW, &oldt);
    return key;
}

void DoChildTask(void)
{
    int fd;
    for(int tryes=0; tryes<3; tryes++)
    {
        usleep(1000000);
        fd=shm_open(DRIVER_NAME_US DRIVER_NAME, O_RDWR, 0666);
        if (fd==-1)
        {
            //DBG("Can't open shm with error %d!\n",errno);
            continue;
        }
        void *addr;
        addr=mmap(NULL, sizeof(AIOTRX_SHARED_MEMORY), PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (addr==MAP_FAILED)
        {
            continue;
        }
        DBG("Restart CoreAudio");
        //sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
        execl("/bin/launchctl","/bin/launchctl","kickstart","-k","system/com.apple.audio.coreaudiod",NULL);
        DBG("execl error %d",errno);
    }
}

int main(int argc, const char * argv[])
{
    DBG("---------------------------------------------------------------");
    DBG("IPAudioPro user-space driver v0.1 start!");
    DBG("---------------------------------------------------------------");
    switch(fork())
    {
    case -1:
        DBG("Can't fork, error %d",errno);
        break;
    case 0:
        DoChildTask();
        return 0;
    }
    if (InitSharedMemory()) return 10;
    if (InitLocks()) return 10;
    AllDone=false;
    struct sigaction sa = { .sa_sigaction=stop_signal_handler, .sa_flags=SA_SIGINFO };
    struct sigaction oldsa;
    sigaction(SIGINT, &sa, &oldsa);
    InitAllAIO();
    if (create_aio_sock()) goto L_abort;
    if (create_stat_sock()) goto L_abort;
    DBG("Enter mainloop...");
    //pthread_create(&ptid_APT, NULL, AudioProcessingThread, NULL	) ;
    do
    {
        CheckClients();
        usleep(50000);
        /*char c;
        c=Get_Key();
        switch(c)
        {
            case 'r':
                DBG("Run request!");
                //atomic_store(&clients_run_requests,1);
                atomic_fetch_or(&clients_run_requests,1ULL);
                break;
            case 's':
                DBG("Stop request!");
                //atomic_store(&clients_run_requests,0);
                atomic_fetch_and(&clients_run_requests, ~1ULL);
                break;
        }*/
    }
    while(!AllDone);
    DBG("AllDone rised!");
L_abort:
    close_aio_sock();
    close_stat_sock();
    //void *ret;
    //pthread_join(ptid_APT, &ret);
    DeinitLocks();
    DeinitSharedMemory();
    DBG("IPAudioPro user-space driver exit!");
    return 0;
}
