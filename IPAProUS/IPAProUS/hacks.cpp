//
//  hacks.c
//  IPAdriver
//
//  Created by Dmytro Oparin on 3/23/20.
//
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <mach/clock.h>
#include <mach/mach_time.h>

#include <semaphore.h>

#include "hacks.h"

#include "drvnames.h"

char log_string[1024];

void WriteToLog(void)
{
    char mstr[256];
    
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    uint32_t secs;
    uint32_t usecs;
    secs=mts.tv_sec;
    usecs=mts.tv_nsec/1000;
    {
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
        uint32_t e;
        uint32_t f;
        
        uint32_t seconds, minutes, hours, year, month, day;
        
        //Clear milliseconds
        
        //Retrieve hours, minutes and seconds
        seconds = secs % 60;
        secs /= 60;
        minutes = secs % 60;
        secs /= 60;
        hours = secs % 24;
        secs /= 24;
        
        //Convert Unix time to date
        a = (uint32_t) ((4 * secs + 102032) / 146097 + 15);
        b = (uint32_t) (secs + 2442113 + a - (a / 4));
        c = (20 * b - 2442) / 7305;
        d = b - 365 * c - (c / 4);
        e = d * 1000 / 30601;
        f = d - e * 30 - e * 601 / 1000;
        
        //January and February are counted as months 13 and 14 of the previous year
        if(e <= 13)
        {
            c -= 4716;
            e -= 1;
        }
        else
        {
            c -= 4715;
            e -= 13;
        }
        
        //Retrieve year, month and day
        year = c;
        month = e;
        day = f;
        snprintf(mstr,256,"%04d-%02d-%02d %02d:%02d:%02d.%06d: ",
                     year,
                     month,
                     day,
                     hours,
                     minutes,
                     seconds,
                     usecs);

    }
    FILE *logfile = fopen("/Users/Shared/IPAudioPro/IPAProUS.log", "a") ;
    if (logfile == NULL) {
        printf("log fopen error %x\n", errno) ;
        return ;
    }
    fprintf(logfile,"%s%s\n",mstr,log_string);
    fclose(logfile) ;
}
