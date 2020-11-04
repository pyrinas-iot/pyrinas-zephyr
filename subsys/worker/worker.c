#include <worker/worker.h>

/* Queue */
static struct k_work_q *main_tasks_q;

void worker_init(struct k_work_q *q)
{
  __ASSERT(q != NULL, "Work must not be NULL!");
  main_tasks_q = q;
}

void worker_submit(struct k_work *work)
{
  k_work_submit_to_queue(main_tasks_q, work);
}

int worker_submit_delayed(struct k_delayed_work *work, k_timeout_t delay)
{
  return k_delayed_work_submit_to_queue(main_tasks_q, work, delay);
}
