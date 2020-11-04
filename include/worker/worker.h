#ifndef _WORKER_H_
#define _WORKER_H_

#include <zephyr.h>

/* Sets queue */
void worker_init(struct k_work_q *q);

/* Submits non delayed work*/
void worker_submit(struct k_work *work);

/* Submits delayed work*/
int worker_submit_delayed(struct k_delayed_work *work, k_timeout_t delay);

#endif