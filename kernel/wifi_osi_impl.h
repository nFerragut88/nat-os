/* nat-os — WiFi OSI implementation, the call0 side. See wifi_osi_impl.c. */
#ifndef NATOS_WIFI_OSI_IMPL_H
#define NATOS_WIFI_OSI_IMPL_H

#include <stdint.h>

void    *osi_impl_sem_create(uint32_t max, uint32_t init);
void    *osi_impl_recursive_mutex_create(void);   /* [step 209] */
int64_t  osi_impl_time_us(void);                 /* [step 210] */
int32_t  osi_impl_event_post(uint32_t base, uint32_t id, uint32_t data); /* [211] */
uint32_t osi_impl_time_us_lo(void);
uint32_t osi_impl_time_us_hi(void);
uint32_t wpa_cb_table_fill(uint32_t sta_connect);  /* [step 219] */
void     osi_impl_sem_delete(void *h);
int32_t  osi_impl_sem_take(void *h, uint32_t ticks);
int32_t  osi_impl_sem_give(void *h);

void    *osi_impl_queue_create(uint32_t len, uint32_t item_size);
void     osi_impl_queue_delete(void *h);
int32_t  osi_impl_queue_send(void *h, void *item, uint32_t ticks, int to_front);
int32_t  osi_impl_queue_recv(void *h, void *item, uint32_t ticks);
uint32_t osi_impl_queue_waiting(void *h);

void    *osi_impl_evt_create(void);
void     osi_impl_evt_delete(void *h);
uint32_t osi_impl_evt_set(void *h, uint32_t bits);
uint32_t osi_impl_evt_clear(void *h, uint32_t bits);
uint32_t osi_impl_evt_wait(void *h, uint32_t bits, int clear, int all, uint32_t ticks);

void    *osi_impl_timer_alloc(void);
void     osi_impl_timer_setfn(void *p, void *fn, void *arg);
void     osi_impl_timer_arm(void *p, uint32_t ms, int periodic);
void     osi_impl_timer_arm_us(void *p, uint32_t us, int periodic);
void     osi_impl_timer_disarm(void *p);
void     osi_impl_timer_done(void *p);

/* Called once per tick by the kernel; runs due callbacks in the caller's
 * context, so they may block and may take locks. */
void     osi_impl_timer_service(void);

/* Creates the service task on first call; returns its id, or -1 if the task
 * table is full. Idempotent. Call once when the radio is brought up. */
int      osi_impl_service_start(void);

void    *osi_impl_malloc(uint32_t n);
void     osi_impl_free(void *p);
void    *osi_impl_calloc(uint32_t count, uint32_t size);
uint32_t osi_impl_free_heap(void);

uint32_t osi_impl_random(void);
int32_t  osi_impl_get_random(uint8_t *buf, uint32_t len);  /* [step 193] */
uint32_t osi_impl_ms_to_tick(uint32_t ms);
void     osi_impl_delay(uint32_t ticks);
int32_t  osi_impl_current_task(void);
uint32_t osi_impl_time_us_lo(void);

/* Telemetry: how many of each pool are in use, so exhaustion is visible rather
 * than presenting as a mysterious failure inside the blob. */
uint32_t osi_impl_sems_used(void);
uint32_t osi_impl_queues_used(void);
uint32_t osi_impl_timers_used(void);

#endif /* NATOS_WIFI_OSI_IMPL_H */
