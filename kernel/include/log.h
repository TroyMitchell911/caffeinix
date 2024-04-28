#ifndef __CAFFEINIX_KERNEL_LOG_H
#define __CAFFEINIX_KERNEL_LOG_H

#include <bio.h>

#define LOGSIZE                         30
#define LOGOP                           10 

// #define LOG_TEST

void log_init(uint16 dev);
void log_write(bio_t b);
void log_begin(void);
void log_end(void);

void log_test(void);

#endif