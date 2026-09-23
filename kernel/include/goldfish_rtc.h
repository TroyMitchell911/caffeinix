/* Goldfish RTC platform-driver initialization interface. */
#ifndef __CAFFEINIX_KERNEL_GOLDFISH_RTC_H
#define __CAFFEINIX_KERNEL_GOLDFISH_RTC_H

/**
 * goldfish_rtc_init() - Register the Goldfish RTC platform driver.
 *
 * Context:
 * Boot process context; registration may probe devices.
 * Return:
 * Zero on success or a driver registration error.
 */
int goldfish_rtc_init(void);

#endif
