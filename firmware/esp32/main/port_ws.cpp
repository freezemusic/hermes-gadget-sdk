// WebSocket transport over esp_websocket_client. Receiving runs on the client's
// own task and sending on hg-ws-tx; complete messages are posted to the app
// task as events.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

namespace hgp {
namespace {

const char* TAG = "hg.ws";
constexpr int kBufferSize = 4096;
constexpr size_t kMaxMessage = 512 * 1024;  // images arrive in 4 KB chunks; this only bounds JSON

using hg::ws::TxItem;

// The client calls hg::ws::process() makes, each with a time limit.
struct EspOps {
  static esp_websocket_client_handle_t h(void* c) { return static_cast<esp_websocket_client_handle_t>(c); }
  bool is_connected(void* c) { return esp_websocket_client_is_connected(h(c)); }
  int send(void* c, uint8_t opcode, const uint8_t* data, size_t len, uint32_t timeout_ms) {
    const auto* p = reinterpret_cast<const char*>(data);
    return opcode == hg::ws::kBinary ? esp_websocket_client_send_bin(h(c), p, static_cast<int>(len), pdMS_TO_TICKS(timeout_ms))
                                     : esp_websocket_client_send_text(h(c), p, static_cast<int>(len), pdMS_TO_TICKS(timeout_ms));
  }
  // Not esp_websocket_client_close(): it ignores its timeout for the close frame.
  void send_close(void* c, uint32_t timeout_ms) {
    esp_websocket_client_send_with_opcode(h(c), WS_TRANSPORT_OPCODES_CLOSE, nullptr, 0, pdMS_TO_TICKS(timeout_ms));
  }
  void destroy(void* c) { esp_websocket_client_destroy(h(c)); }
};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void clear_dns(void*) { dns_clear_cache(); }

uint32_t generation_of(const esp_websocket_event_data_t* d) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d->user_context));
}

}  // namespace

void WsTransport::retire_current() {
  gens_.leave();  // anything still queued from the old connection is now stale
  if (!client_) return;
  // The send task closes and destroys the client after whatever it is sending.
  // On a dead link that can take as long as the handshake timeout, and the app
  // task must not wait. The next client is not created until this destroy ends
  // (ClientSlot): two handshakes at once kept a stuck TLS context alive.
  slot_.push();
  TxItem item{client_, 0, hg::ws::kClose, nullptr, 0};
  client_ = nullptr;
  if (!tx_ || xQueueSend(tx_, &item, pdMS_TO_TICKS(hg::ws::kHandoffTimeoutMs)) != pdTRUE) {
    ESP_LOGE(TAG, "the send task took no close for %u ms", static_cast<unsigned>(hg::ws::kHandoffTimeoutMs));
    slot_.unpush();
    handoff_failed_ = true;
  }
}

void WsTransport::open_client() {
  if (!tx_) {
    tx_ = xQueueCreate(hg::ws::kTxMessages + hg::ws::kTxReserved, sizeof(TxItem));
    if (!tx_ || xTaskCreate(&WsTransport::tx_task, "hg-ws-tx", 6144, this, 5, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "can't start the send task");
      if (tx_) vQueueDelete(tx_);
      tx_ = nullptr;
      events::post(EventType::WsClosed, "client init failed", 18, gens_.current());
      return;
    }
  }
  // lwIP caches the last answer. A restarted gateway (a Tailscale Funnel name
  // included) can be a new address while this cache still names the old one,
  // and every new client would connect to it until something rebooted.
  if (tcpip_callback_wait(clear_dns, nullptr) != ERR_OK) ESP_LOGW(TAG, "couldn't clear the DNS cache");
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  cfg.subprotocol = subprotocol_.c_str();
  cfg.buffer_size = kBufferSize;
  cfg.task_stack = 6144;
  cfg.disable_auto_reconnect = true;  // hg::App owns retry and backoff
  cfg.network_timeout_ms = hg::ws::kNetworkTimeoutMs;
  cfg.ping_interval_sec = 0;          // the protocol has its own heartbeat
  // Every event names the connection it belongs to: a client still being torn
  // down on the send task must not be mistaken for the current one.
  cfg.user_context = reinterpret_cast<void*>(static_cast<uintptr_t>(gens_.current()));
  if (url_.rfind("wss://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  client_ = esp_websocket_client_init(&cfg);
  if (!client_) {
    events::post(EventType::WsClosed, "client init failed", 18, gens_.current());
    return;
  }
  esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &WsTransport::on_event, this);
  ESP_LOGI(TAG, "new websocket client");
  if (esp_websocket_client_start(client_) != ESP_OK) {
    events::post(EventType::WsClosed, "client start failed", 19, gens_.current());
  }
}

void WsTransport::connect(const std::string& url, const std::string& subprotocol) {
  url_ = url;
  subprotocol_ = subprotocol;
  slot_.want();
  retire_current();
  if (slot_.ready()) {
    slot_.cancel();
    open_client();
  }
}

void WsTransport::arm() {
  // The send task finished destroying the previous client. Called on the app task.
  if (!slot_.ready()) return;
  slot_.cancel();
  open_client();
}

void WsTransport::close() {
  slot_.cancel();
  retire_current();
}

bool WsTransport::stuck() const { return handoff_failed_ || watch_.stuck(now_ms()); }

bool WsTransport::enqueue(uint8_t opcode, const void* data, size_t len) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
  if (!hg::ws::may_queue_message(uxQueueSpacesAvailable(tx_), opcode)) return false;  // the link isn't keeping up
  auto* copy = static_cast<uint8_t*>(heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!copy) copy = static_cast<uint8_t*>(malloc(len ? len : 1));
  if (!copy) return false;
  if (len) std::memcpy(copy, data, len);
  TxItem item{client_, gens_.current(), opcode, copy, len};
  if (xQueueSend(tx_, &item, 0) != pdTRUE) {
    free(copy);
    return false;
  }
  return true;
}

bool WsTransport::send_text(std::string_view text) { return enqueue(hg::ws::kText, text.data(), text.size()); }

bool WsTransport::send_binary(const uint8_t* data, size_t len) { return enqueue(hg::ws::kBinary, data, len); }

void WsTransport::tx_task(void* arg) {
  auto* self = static_cast<WsTransport*>(arg);
  EspOps ops;
  TxItem item;
  for (;;) {
    if (xQueueReceive(self->tx_, &item, portMAX_DELAY) != pdTRUE) continue;
    self->watch_.begin(now_ms());
    const hg::ws::Outcome outcome = hg::ws::process(item, self->gens_.current(), ops);
    self->watch_.end();
    if (outcome == hg::ws::Outcome::Failed) ESP_LOGW(TAG, "send failed");
    // The app task opens the next client only after this destroy, and only if
    // a connect is still wanted. Posting from here keeps that open off this task.
    if (item.opcode == hg::ws::kClose && self->slot_.destroyed()) events::post(EventType::WsArm);
    free(const_cast<uint8_t*>(item.data));
  }
}

void WsTransport::on_event(void* arg, const char*, int32_t id, void* event_data) {
  auto* self = static_cast<WsTransport*>(arg);
  auto* d = static_cast<esp_websocket_event_data_t*>(event_data);
  const uint32_t gen = generation_of(d);
  if (!self->gens_.is_current(gen)) return;  // a client being torn down
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      events::post(EventType::WsOpen, nullptr, 0, gen);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      self->gens_.lost(gen);
      events::post(EventType::WsClosed, "disconnected", 12, gen);
      break;
    case WEBSOCKET_EVENT_ERROR:
      self->gens_.lost(gen);
      events::post(EventType::WsClosed, "connection error", 16, gen);
      break;
    case WEBSOCKET_EVENT_DATA: {
      uint8_t op = d->op_code;
      if (op == 0x8 || op == 0x9 || op == 0xA) break;  // close/ping/pong are handled by the client
      if (op != 0x0) self->rx_opcode_ = op;          // first frame of a message
      if (d->payload_offset == 0 && op != 0x0) self->rx_.clear();
      if (self->rx_.size() + static_cast<size_t>(d->data_len) > kMaxMessage) {
        ESP_LOGW(TAG, "dropping oversized message");
        self->rx_.clear();
        break;
      }
      self->rx_.append(d->data_ptr, static_cast<size_t>(d->data_len));
      bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
      if (frame_done && d->fin) {
        EventType t = self->rx_opcode_ == 0x2 ? EventType::WsBinary : EventType::WsText;
        events::post(t, self->rx_.data(), self->rx_.size(), gen);
        self->rx_.clear();
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace hgp
