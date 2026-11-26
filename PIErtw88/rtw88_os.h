/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 The Blueberry Pie OS Authors
 */
#ifndef PIE_RTW88_OS_H
#define PIE_RTW88_OS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct rtw88_os;
struct rtw88_worker;
struct rtw88_pie;
struct rtw88_board_profile;
struct pie_sdio_func;
struct pie80211_drv;
void *rtw88_os_alloc_mem(size_t size);
void rtw88_os_free_mem(void *p, size_t size);
struct rtw88_os *rtw88_os_new(void (*fn)(void *), void *arg);
void rtw88_os_free(struct rtw88_os *os);
void rtw88_os_lock(struct rtw88_os *os);
void rtw88_os_unlock(struct rtw88_os *os);
void rtw88_os_kick(struct rtw88_os *os);
void rtw88_os_stop(struct rtw88_os *os);
void rtw88_os_delay(unsigned int usec);
uint64_t rtw88_os_now(void);
void rtw88_os_error(const char *stage, uint32_t address, int error, int retry);
void rtw88_os_failure(const char *stage, uint32_t address, int error);
int rtw88_os_firmware(uint8_t **data, size_t *size);
struct rtw88_worker *rtw88_worker_new(void (*fn)(void *), void *arg);
void rtw88_worker_kick(struct rtw88_worker *worker);
void rtw88_worker_stop(struct rtw88_worker *worker);
void rtw88_worker_free(struct rtw88_worker *worker);
struct rtw88_pie *rtw88_pie_new(struct pie_sdio_func *function, const struct rtw88_board_profile *profile);
struct pie80211_drv *rtw88_pie_attach(struct rtw88_pie *pie, int *error);
void rtw88_pie_delete(struct rtw88_pie *pie);
#ifdef __cplusplus
}
#endif
#endif
