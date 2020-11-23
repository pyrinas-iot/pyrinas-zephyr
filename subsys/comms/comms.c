
#include <zephyr.h>
#include <zephyr/types.h>
#include <sys/ring_buffer.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/uart.h>
#include <comms/comms.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(comms);

/* Parameters for memory segment */
#define UART_RINGBUF_SIZE 512
#define UART_SLAB_BUF_SIZE 128
#define UART_SLAB_BUF_COUNT 12
#define UART_SLAB_ALIGNMENT 4

BUILD_ASSERT((UART_SLAB_BUF_SIZE % UART_SLAB_ALIGNMENT) == 0);
K_MEM_SLAB_DEFINE(uart_slab, UART_SLAB_BUF_SIZE, UART_SLAB_BUF_COUNT, UART_SLAB_ALIGNMENT);

/* Semaphore for releasing comms thread */
static K_SEM_DEFINE(comms_sem, 0, 1);

/* Event callback */
comms_evt_callback_t callback;

/* Ring buff for rx/tx */
struct uart_buf
{
  struct ring_buf rb;
  uint32_t buf[UART_RINGBUF_SIZE];
};

/* Static instance of tx buf */
static struct uart_buf tx_buf, rx_buf;

/* Static instance of uart */
const static struct device *uart;

static int uart_start_tx(const struct device *uart)
{

  uint8_t *buf;
  size_t size;
  int err;

  /* Check if there's anything else to send */
  size = ring_buf_get_claim(&tx_buf.rb, &buf, UART_RINGBUF_SIZE);
  if (size)
  {
    /* Start transmitting next block of data */
    err = uart_tx(uart, buf, size, 0);
    if (err)
    {
      LOG_ERR("Unable to start tx. Err: %i", err);

      /* "Free" the memory in ringbuf */
      err = ring_buf_get_finish(&tx_buf.rb, size);
      if (err)
        LOG_ERR("Unable to free ringbuf. Error: %i", err);

      return err;
    }
  }

  return 0;
}

static int uart_start_rx(const struct device *uart)
{
  int err;
  uint8_t *buf;

  /* Init slab */
  err = k_mem_slab_alloc(&uart_slab, (void **)&buf, K_NO_WAIT);
  if (err)
  {
    LOG_ERR("Failed to alloc slab");
    return err;
  }

  /* Enable uart rx */
  err = uart_rx_enable(uart, buf, UART_SLAB_BUF_SIZE, 10);
  if (err)
  {
    LOG_ERR("Failed to enable RX");
    return err;
  }

  return 0;
}

static void uart_callback(const struct device *uart, struct uart_event *evt, void *user_data)
{
  int err;
  size_t written;

  switch (evt->type)
  {
  case UART_TX_DONE:
    LOG_INF("Tx sent %d bytes", evt->data.tx.len);

    /* "Free" the memory in ringbuf */
    err = ring_buf_get_finish(&tx_buf.rb, evt->data.tx.len);
    if (err)
      LOG_ERR("Unable to free ringbuf. Error: %i", err);

    /* Queue up next chunk of bytes if theres anything left */
    if (!ring_buf_is_empty(&tx_buf.rb))
    {
      err = uart_start_tx(uart);
      if (err)
        LOG_WRN("Tx start err: %i", err);
    }

    break;

  case UART_TX_ABORTED:
    LOG_ERR("Tx aborted");

    /* "Free" the memory in ringbuf */
    err = ring_buf_get_finish(&tx_buf.rb, evt->data.tx.len);
    if (err)
      LOG_ERR("Unable to free ringbuf. Error: %i", err);

    break;

  case UART_RX_RDY:
    LOG_INF("Received data %d bytes", evt->data.rx.len);

    /* Enqueue into ringbuf */
    written = ring_buf_put(&rx_buf.rb, evt->data.rx.buf, evt->data.rx.len);
    if (written == 0)
      LOG_ERR("Unable to rx. Buffer is full.");

    /* Allow processing in loop */
    k_sem_give(&comms_sem);

    break;

  case UART_RX_BUF_REQUEST:
  {
    uint8_t *buf;

    LOG_DBG("Buf request");

    err = k_mem_slab_alloc(&uart_slab, (void **)&buf, K_NO_WAIT);
    __ASSERT(err == 0, "Failed to allocate slab");

    err = uart_rx_buf_rsp(uart, buf, UART_SLAB_BUF_SIZE);
    __ASSERT(err == 0, "Failed to provide new buffer");
    break;
  }

  case UART_RX_BUF_RELEASED:
    LOG_DBG("Buf release");
    k_mem_slab_free(&uart_slab, (void **)&evt->data.rx_buf.buf);
    break;

  case UART_RX_DISABLED:
    LOG_INF("Rx disabled.");

    /* Restart RX if it has stopped */
    uart_start_rx(uart);
    break;

  case UART_RX_STOPPED:
    LOG_INF("Rx stopped.");
    break;
  }
}

int comms_init(comms_evt_callback_t cb)
{
  int err;

  if (cb == NULL)
  {
    return -EINVAL;
  }

  /* Copy pointer to callback */
  callback = cb;

  /* Get uart binding */
  uart = device_get_binding(DT_LABEL(DT_NODELABEL(uart2)));
  __ASSERT(uart, "Failed to get the uart device");

  /* Set UART callback */
  err = uart_callback_set(uart, uart_callback, NULL);
  __ASSERT(err == 0, "Failed to set callback");

  /* Start UART Rx*/
  err = uart_start_rx(uart);
  __ASSERT(err == 0, "Failed to allocate slab");

  /* Enable uart rx */
  return 0;
}

int comms_write(const uint8_t *buf, size_t len)
{
  int err;
  uint32_t written;

  /* Enqueue into ringbuf */
  written = ring_buf_put(&tx_buf.rb, buf, len);
  if (written == 0)
    return -ENOMEM;

  /* Start tx process*/
  err = uart_start_tx(uart);
  if (err)
    LOG_WRN("Tx start err: %i", err);

  return err;
}

void comms_rx_thread(void)
{

  LOG_DBG("comms_rx_thread");

  /* Init tx ringbuf */
  ring_buf_init(
      &tx_buf.rb,
      sizeof(tx_buf.buf),
      tx_buf.buf);

  /* Init rx ringbuf */
  ring_buf_init(
      &rx_buf.rb,
      sizeof(rx_buf.buf),
      rx_buf.buf);

  int err;
  static uint8_t message[256];
  size_t message_size = 0;
  size_t size = 0;

  /* Pointers */
  uint8_t *p_message_next;
  uint8_t *p_data_start;
  uint8_t *p_data_end;

  for (;;)
  {

    /* Wait indefinitely for data to be sent over bluetooth */
    k_sem_take(&comms_sem, K_FOREVER);

    /* Pull available data */
    size = ring_buf_get_claim(&rx_buf.rb, &p_data_start, UART_RINGBUF_SIZE);
    if (size == 0)
      continue;

    /* Get start and end of buffer */
    p_data_end = p_data_start + size;

    /* Get the message size if first byte */
    if (message_size == 0)
    {
      /* Set next to beginning of message*/
      p_message_next = message;

      /* Set message size to incoming first byte */
      message_size = *p_data_start++;

      LOG_DBG("Message size: %d", message_size);

      /* Skip ahead if we get a message size of 0 */
      if (message_size == 0)
        goto free;
    }

    size_t read_cnt = p_data_end - p_data_start;
    size_t remaining = message_size - (p_message_next - message);

    // Determine read_cnt
    if (p_data_end - p_data_start > remaining)
    {
      read_cnt = remaining;
    }

    LOG_DBG("Read count: %d. Remaining: %d", read_cnt, remaining);

    /* Copy to the next point in message */
    memcpy(p_message_next, p_data_start, read_cnt);

    /* We have a full message */
    if (remaining - read_cnt == 0)
    {
      LOG_DBG("Message from serial!");

      comms_evt_t evt = {
          comms_evt_read,
          message,
          message_size};

      /* Call the event callback */
      callback(&evt);

      /* Increment start */
      p_data_start += read_cnt;

      /* Reset this pointer */
      p_message_next = message;

      /* If there are some remaining bits */
      if (p_data_end - p_data_start)
      {

        /* Get the new message size */
        message_size = *p_data_start++;

        LOG_DBG("Message size: %d", message_size);

        /* Skip ahead if we get a message size of 0 */
        if (message_size == 0)
          goto free;

        /* Copy remaining data over */
        memcpy(p_message_next, p_data_start, p_data_end - p_data_start);

        /* Increment next */
        p_message_next += p_data_end - p_data_start;
      }
      else
      {
        /* Reset message size */
        message_size = 0;
      }
    }
    else
    {
      /* Increment next */
      p_message_next += read_cnt;
    }

  free:

    /* Release bytes used */
    err = ring_buf_get_finish(&rx_buf.rb, size);
    if (err)
    {
      LOG_ERR("Unable to release rx ring buffer. Err :%d", err);
    }
  }
}

#define STACKSIZE KB(2)
K_THREAD_DEFINE(comms_rx_thread_id, STACKSIZE, comms_rx_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);