#include <app/app.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(app);

static void my_expiry_function(struct k_timer *timer);
K_TIMER_DEFINE(my_timer, my_expiry_function, NULL);

static void my_expiry_function(struct k_timer *timer)
{
  if (ble_is_connected())
  {
    ble_publish("pong", "");
  }

  k_timer_start(&my_timer, K_SECONDS(1), K_NO_WAIT);
}

uint32_t counter = 0;

void evt_cb(char *name, char *data)
{
  LOG_INF("%d \"%s\" \"%s\"", counter++, log_strdup(name), log_strdup(data));
}

void setup(void)
{
  // Message!
  LOG_INF("Start of Pyrinas Client example!\n");

  // Default config for central mode
  BLE_STACK_PERIPH_DEF(init);

  /* BLE initialization */
  ble_stack_init(&init);

  // Subscribe to event
  ble_subscribe("ping", evt_cb);

  // Start message timer
  k_timer_start(&my_timer, K_SECONDS(1), K_NO_WAIT);
}

void loop(void)
{
  k_msleep(100);
}