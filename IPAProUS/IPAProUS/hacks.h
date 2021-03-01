//
//  hacks.h
//  IPAdriver
//
//  Created by Dmytro Oparin on 3/23/20.
//

#ifndef hacks_h
#define hacks_h

extern char log_string[1024];
void WriteToLog(void);

extern pthread_mutex_t log_lock;

#define DBG(...) do {\
    pthread_mutex_lock(&log_lock);\
    snprintf(log_string,1024,__VA_ARGS__);\
    WriteToLog();\
    pthread_mutex_unlock(&log_lock);\
}while(0)

#endif /* hacks_h */
