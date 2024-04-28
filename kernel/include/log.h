#ifndef __CAFFEINIX_KERNEL_FS_LOG_H
#define __CAFFEINIX_KERNEL_FS_LOG_H

#include <bio.h>

#define LOGSIZE                         30
#define LOGOP                           10 

// #define LOG_TEST

void log_init(uint32 dev, uint32 sz, uint32 start);
void log_write(bio_t b);
void log_begin(void);
void log_end(void);

void log_test(void);

#endif