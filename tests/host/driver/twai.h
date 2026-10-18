#pragma once
// Host stand-in for the ESP-IDF CAN (TWAI) driver, covering the surface the
// firmware actually uses: the configuration types begin() passes, the driver
// lifecycle, and transmit/receive.
//
// The functions are declared here and defined in tests/host/twai_stub.cpp, so a
// test owns a virtual bus: it queues frames the client will find and inspects
// every frame the client puts on the wire, flow control included. That is what
// makes a request-format assertion possible at all.
#include <cstdint>

#include "esp_err.h"

struct twai_message_t {
  uint32_t identifier = 0;
  uint32_t extd = 0;
  uint32_t rtr = 0;
  uint8_t data_length_code = 0;
  uint8_t data[8]{};
};

using TickType_t = uint32_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef enum {
  TWAI_STATE_STOPPED = 0,
  TWAI_STATE_RUNNING = 1,
  TWAI_STATE_BUS_OFF = 2,
  TWAI_STATE_RECOVERING = 3,
} twai_state_t;

#define TWAI_ALERT_TX_FAILED (1u << 0)
#define TWAI_ALERT_RX_QUEUE_FULL (1u << 1)
#define TWAI_ALERT_BUS_OFF (1u << 2)
#define TWAI_ALERT_BUS_RECOVERED (1u << 3)
#define TWAI_ALERT_ABORT_FAILED (1u << 4)
#define TWAI_ALERT_BUS_ERROR (1u << 6)
#define TWAI_ALERT_AND_LOG (1u << 5)

typedef enum {
  TWAI_MODE_NORMAL = 0,
  TWAI_MODE_NO_ACK = 1,
  TWAI_MODE_LISTEN_ONLY = 2,
} twai_mode_t;

typedef struct {
  int tx_gpio_num;
  int rx_gpio_num;
  twai_mode_t mode;
  uint32_t tx_queue_len;
  uint32_t rx_queue_len;
  uint32_t alerts_enabled;
  uint32_t ctrl_alerts_enabled;
  uint32_t flags;
} twai_general_config_t;

// Positional initialisers rather than designated ones: this header is compiled
// as C++14, where designated initialisers are a GNU extension.
#define TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin, mode) \
  { (tx_pin), (rx_pin), (mode), 5u, 5u, 0u, 0u, 0u }

typedef struct {
  uint32_t tseg1;
  uint32_t tseg2;
  uint32_t tprop;
  uint32_t sjw;
  uint32_t brp;
} twai_timing_config_t;

#define TWAI_TIMING_CONFIG_500KBITS() {10u, 5u, 15u, 1u, 4u}

typedef struct {
  uint32_t acceptance_code;
  uint32_t acceptance_mask;
  uint32_t rejection_code;
  uint32_t rejection_mask;
  uint32_t flags;
} twai_filter_config_t;

#define TWAI_FILTER_CONFIG_ACCEPT_ALL() {0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0u}

// Field names follow ESP-IDF's twai_status_info_t rather than this stub's own
// naming. They used to differ here and nothing read them, so the divergence was
// free; the moment firmware reads a counter, a stub with the wrong name would
// let a test pass against fields the real driver does not have.
typedef struct {
  twai_state_t state;
  uint32_t msgs_to_tx;
  uint32_t msgs_to_rx;
  uint32_t tx_error_counter;
  uint32_t rx_error_counter;
  uint32_t tx_failed_count;
  uint32_t rx_missed_count;
  uint32_t rx_overrun_count;
  uint32_t arb_lost_count;
  uint32_t bus_error_count;
} twai_status_info_t;

esp_err_t twai_driver_install(const twai_general_config_t* general_config,
                              const twai_timing_config_t* timing_config,
                              const twai_filter_config_t* filter_config);
esp_err_t twai_driver_uninstall(void);
esp_err_t twai_start(void);
esp_err_t twai_stop(void);
esp_err_t twai_transmit(const twai_message_t* message, TickType_t timeout);
esp_err_t twai_receive(twai_message_t* message, TickType_t timeout);
esp_err_t twai_read_alerts(uint32_t* alerts, uint32_t* alert_mask);
esp_err_t twai_get_status_info(twai_status_info_t* status_info);
esp_err_t twai_initiate_recovery(void);
