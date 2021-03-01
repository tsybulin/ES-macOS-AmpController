//
//  aiotrx_interface.h
//  aiotrx
//
//  Created by Dmytro Oparin on 7/26/19.
//  Copyright © 2019 IPaudio. All rights reserved.
//

#ifndef aiotrx_interface_h
#define aiotrx_interface_h

enum
{
    kAIOdriver_start,
    kAIOdriver_stop,
    kAIOdriver_get_client_n,
    kAIOdriver_wait_rx_seq,
    kAIOdriver_UserClientMethodCount
};

#include <stdatomic.h>


#define MAX_CLIENTS (4)
#define MAX_DEVICES (8)
#define MAX_CHANNELS_FROM_DEVICE (16)
#define AIO_BLOCK_SZ (8)

#define TOTAL_BUSES (8)

#define TOTAL_CHANNELS (MAX_DEVICES*MAX_CHANNELS_FROM_DEVICE)
#define MAX_AIO_BUFFER (2048)
#define MAX_AIO_BLOCKS (MAX_AIO_BUFFER/AIO_BLOCK_SZ)

typedef struct
{
    uint32_t stream_addr;
    uint32_t hw_inputs;
    uint32_t hw_outputs;
    char name[128];
} TRX_DEVICE_CONFIG;

typedef struct
{
    TRX_DEVICE_CONFIG devcfg[MAX_DEVICES];
    uint32_t TailBufferSize;
    uint32_t RxPrerollSamples;
} TRX_DEVICES_CONFIGURATION;

#define IPADDR(A0,A1,A2,A3) ((((uint32_t)(A0))<<0)+(((uint32_t)(A1))<<8)+(((uint32_t)(A2))<<16)+(((uint32_t)(A3))<<24))

typedef struct
{
	int32_t samples[AIO_BLOCK_SZ];
}CHANNEL_SAMPLES_BLOCK;

typedef struct
{
    CHANNEL_SAMPLES_BLOCK blocks[MAX_AIO_BLOCKS];
}AIO_CHANNEL;

typedef struct
{
	volatile atomic_uint need_wait;	//Set only for ASIO client, for CoreAudio we don't need wait
	AIO_CHANNEL output_channels[TOTAL_CHANNELS];
	volatile atomic_uint readySEQ;	//Max ready tx data SEQ
}AIO_OUTPUT_BY_CLIENT;

typedef struct
{
    char name[128];
}CHANNEL_NAME;

typedef struct
{
    TRX_DEVICES_CONFIGURATION dev_config;
    volatile atomic_uint ConfigurationChanged[MAX_CLIENTS];
    uint32_t num_inputs;
    uint32_t num_outputs;
    CHANNEL_NAME input_names[TOTAL_CHANNELS];
    CHANNEL_NAME output_names[TOTAL_CHANNELS];
    uint32_t sample_frequency;
    uint32_t input_latency;
    uint32_t output_latency;
    AIO_CHANNEL input_channels[TOTAL_CHANNELS];
    volatile atomic_uint readySEQ; //Max ready rx data SEQ
    volatile atomic_uint waitSEQ[MAX_CLIENTS]; //Area to pass SEQ number to function WaitRXseq via kernctl
    AIO_OUTPUT_BY_CLIENT clients_output[MAX_CLIENTS];
    volatile atomic_uint_least64_t gDevice_ZeroSampleTime;
    volatile atomic_uint_least64_t gDevice_ZeroHostTime;
    volatile atomic_uint_least64_t CurrentTxSendSample;
    volatile atomic_uint_least64_t CurrentRecvSample;
    char ConfigName[128];
    int32_t base_bus_input;
    int32_t base_bus_output;
    volatile atomic_uint client_states[MAX_CLIENTS];
    volatile atomic_uint client_watchdog[MAX_CLIENTS];
    volatile atomic_uint ReloadConfigRequest;
    volatile atomic_uint curtx_SEQ;
}AIOTRX_SHARED_MEMORY;

#define FAST_WAIT_RX_SEQ (1)

#endif /* aiotrx_interface_h */
