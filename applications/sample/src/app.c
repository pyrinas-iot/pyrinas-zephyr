#include <app/app_includes.h>
#include <worker/worker.h>

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <nrfx.h>

#include <pyrinas_codec.h>
#include <comms/comms.h>

#include <app/version.h>

#include <libpyrinas_codec_example.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(app);

/* Delayed application work*/
static struct k_work publish_work;

static void publish_timer_evt(struct k_timer *timer);
K_TIMER_DEFINE(publish_timer, publish_timer_evt, NULL);

static void publish_timer_evt(struct k_timer *timer)
{
  /* Start work */
  worker_submit(&publish_work);
}

static void publish_work_fn()
{
  /* Create request */
  EnvironmentData data = {
      .temperature = 1000,
      .humidity = 3000,
  };

  /* Encode the data with our sample library */
  Encoded encoded = encode_environment_data(&data);

  /* If we have valid encoded data, publish it.*/
  if (encoded.resp == Ok)
  {
    /* Request config */
    pyrinas_cloud_publish("env", encoded.data, encoded.size);
  }
}

void application_callback(const uint8_t *topic, size_t topic_len, const uint8_t *data, size_t data_len)
{
  LOG_INF("Application: Topic: %s Len: %d", topic, data_len);
}

void setup(void)
{
  /* Message! */
  LOG_INF("Start of Pyrinas Sample!");

  uint8_t ver[64];
  get_version_string(ver, sizeof(ver));
  LOG_INF("Version: %s", ver);

  /* Setup callback for certain events */
  pyrinas_cloud_subscribe("cmd", application_callback);
  pyrinas_cloud_subscribe("cfg", application_callback);

  /* Work */
  k_work_init(&publish_work, publish_work_fn);

  /* Start timer */
  k_timer_start(&publish_timer, K_MINUTES(10), K_NO_WAIT);
}