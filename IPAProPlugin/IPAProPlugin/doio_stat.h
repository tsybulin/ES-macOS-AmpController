//
//  doio_stat.h
//  ESController
//
//  Created by Pavel Tsybulin on 2/17/19.
//  Copyright © 2019 Pavel Tsybulin. All rights reserved.
//

#ifndef doio_stat_h
#define doio_stat_h

#include <MacTypes.h>

//#define ESSTAT 1

#ifdef ESSTAT

/**
 
 текущее время с точностью до мкс
 текущий st
 текущий tzst
 текущий rcvCnt
 
 **/

typedef struct doio_stat_s {
    UInt64  absoluteTime ;
    Float64 sampleTime ;
    Float64 zeroSampleTime ;
    UInt64  frameCount ;
} doio_stat_t ;

#define DOIO_MAX_SIZE 2000

static doio_stat_t doio_stat[DOIO_MAX_SIZE] ;
static UInt64 doio_stat_counter = 0 ;

typedef struct hdr_stat_s {
    UInt64  absoluteTime ;
    Float64 zeroSampleTime ;
    UInt64  frameCount ;
} hdr_stat_t ;

#define HDR_MAX_SIZE 1000

static hdr_stat_t hdr_stat[HDR_MAX_SIZE] ;
static UInt64 hdr_stat_counter = 0 ;

#endif

#endif /* doio_stat_h */
