#ifndef __CAFFEINIX_KERNEL_PRINTK_H
#define __CAFFEINIX_KERNEL_PRINTK_H

#include <typedefs.h>

#define PRINTK_TEXT_MAX 192
#define PRINTK_RECORD_MAX 128

enum printk_level {
	PRINTK_EMERG = 0,
	PRINTK_ALERT,
	PRINTK_CRIT,
	PRINTK_ERR,
	PRINTK_WARN,
	PRINTK_NOTICE,
	PRINTK_INFO,
	PRINTK_DEBUG,
	PRINTK_LEVEL_COUNT,
};

struct printk_record {
	uint64 sequence;
	uint64 timestamp_ns;
	uint16 length;
	uint8 level;
	char text[PRINTK_TEXT_MAX];
};

void printk_init(void);
void printk(enum printk_level level, const char *format, ...);
void printk_enter_panic(void);
void printk_set_console_level(enum printk_level level);
enum printk_level printk_get_console_level(void);
const char *printk_level_name(enum printk_level level);
uint64 printk_first_sequence(void);
uint64 printk_next_sequence(void);
int printk_read_record(uint64 sequence, struct printk_record *record);

#define pr_emerg(format, ...) \
	printk(PRINTK_EMERG, format, ##__VA_ARGS__)
#define pr_alert(format, ...) \
	printk(PRINTK_ALERT, format, ##__VA_ARGS__)
#define pr_crit(format, ...) \
	printk(PRINTK_CRIT, format, ##__VA_ARGS__)
#define pr_err(format, ...) \
	printk(PRINTK_ERR, format, ##__VA_ARGS__)
#define pr_warn(format, ...) \
	printk(PRINTK_WARN, format, ##__VA_ARGS__)
#define pr_notice(format, ...) \
	printk(PRINTK_NOTICE, format, ##__VA_ARGS__)
#define pr_info(format, ...) \
	printk(PRINTK_INFO, format, ##__VA_ARGS__)
#define pr_debug(format, ...) \
	printk(PRINTK_DEBUG, format, ##__VA_ARGS__)

#endif
