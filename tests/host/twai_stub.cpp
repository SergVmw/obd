// A virtual CAN bus for the host tests.
//
// The driver lifecycle is a no-op that reports success. The interesting half is
// the queue: frames the test queues here are what the client will read on its
// next loop, and every frame the client writes, flow control included, is kept
// so a test can assert on the bytes that went out. Requests that nobody asserts
// on are how a request format goes wrong unnoticed for a whole release.
#include "driver/twai.h"

#include <deque>
#include <vector>

namespace h2g_twai_stub {

namespace {
std::deque<twai_message_t> g_rx;
std::vector<twai_message_t> g_tx;
twai_state_t g_state = TWAI_STATE_STOPPED;
// The driver's own counters, which the real controller accumulates and this stub
// has to be told. Without them every read returns zero and a test cannot tell a
// working path from one that copies zeros faithfully.
twai_status_info_t g_status{};
// Alerts raised by the test, consumed by the next twai_read_alerts. Before this
// the stub ignored the call entirely, which left handleAlerts - RX_QUEUE_FULL,
// TX_FAILED, BUS_OFF and the recovery that follows - unreachable from any test.
uint32_t g_alerts = 0;
}  // namespace

void reset() {
  g_rx.clear();
  g_tx.clear();
  g_state = TWAI_STATE_STOPPED;
  g_status = twai_status_info_t{};
  g_alerts = 0;
}

void setState(twai_state_t state) { g_state = state; }

void raiseAlert(uint32_t alert) { g_alerts |= alert; }

void setStatus(const twai_status_info_t& status) {
  g_status = status;
  g_status.state = g_state;
}

// Hand the next frames to twai_receive(), in order.
void queueRx(const twai_message_t& message) { g_rx.push_back(message); }

const std::vector<twai_message_t>& transmitted() { return g_tx; }
void clearTransmitted() { g_tx.clear(); }
void recordTx(const twai_message_t& message) { g_tx.push_back(message); }

namespace {
// Reached from the C entry points below, which cannot see the anonymous
// namespace directly.
bool takeNext(twai_message_t* out) {
  if (g_rx.empty()) return false;
  *out = g_rx.front();
  g_rx.pop_front();
  return true;
}
twai_state_t currentState() { return g_state; }
const twai_status_info_t& status() { return g_status; }
uint32_t takeAlerts() {
  const uint32_t raised = g_alerts;
  g_alerts = 0;
  return raised;
}
}  // namespace

}  // namespace h2g_twai_stub

esp_err_t twai_driver_install(const twai_general_config_t*, const twai_timing_config_t*,
                              const twai_filter_config_t*) {
  return ESP_OK;
}

esp_err_t twai_driver_uninstall(void) { return ESP_OK; }

esp_err_t twai_start(void) {
  h2g_twai_stub::setState(TWAI_STATE_RUNNING);
  return ESP_OK;
}

esp_err_t twai_stop(void) {
  h2g_twai_stub::setState(TWAI_STATE_STOPPED);
  return ESP_OK;
}

esp_err_t twai_transmit(const twai_message_t* message, TickType_t) {
  if (message == nullptr) return ESP_ERR_INVALID_ARG;
  h2g_twai_stub::recordTx(*message);
  return ESP_OK;
}

esp_err_t twai_receive(twai_message_t* message, TickType_t) {
  if (message == nullptr) return ESP_ERR_INVALID_ARG;
  // An empty queue reports a timeout, which is what an empty RX queue looks
  // like on real hardware and is what ends the client's drain loop.
  if (!h2g_twai_stub::takeNext(message)) return ESP_ERR_TIMEOUT;
  return ESP_OK;
}

esp_err_t twai_read_alerts(uint32_t* alerts, uint32_t*) {
  if (alerts == nullptr) return ESP_ERR_INVALID_ARG;
  *alerts = h2g_twai_stub::takeAlerts();
  return ESP_OK;
}

esp_err_t twai_get_status_info(twai_status_info_t* status_info) {
  if (status_info == nullptr) return ESP_ERR_INVALID_ARG;
  *status_info = h2g_twai_stub::status();
  status_info->state = h2g_twai_stub::currentState();
  return ESP_OK;
}

esp_err_t twai_initiate_recovery(void) { return ESP_OK; }
