#include "bike_tracker.h"
#include "string_utils.h"

#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "mbedtls/sha256.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace bike_tracker {

static const char *const TAG = "bike_tracker.component";

static constexpr int CONNECT_FAILURE_ERROR_THRESHOLD = 3;
static constexpr int CONNECT_FAILURE_REBOOT_THRESHOLD = 15;
static constexpr unsigned long HEARTBEAT_TIMEOUT_MS = 60000;
static constexpr int ICON_COUNT_GAP = 1;
static constexpr int GROUP_GAP = 3;

// Pixel-art icons, 7 pixels tall. '#' uses the icon's color, '+' its accent color.
static const char *const DOCK_ICON[] = {
  ".#####.",
  "##...##",
  "##.#.##",
  "##...##",
  "##.####",
  "##.####",
  ".#####.",
  nullptr,
};

static const char *const BIKE_ICON[] = {
  "..##...##..",
  "...#...#...",
  "...#####...",
  ".##.#.#.##.",
  "#..#.#.#..#",
  "#..#...#..#",
  ".##.....##.",
  nullptr,
};

static const char *const EBIKE_ICON[] = {
  "..+..##...##..",
  ".+....#...#...",
  "+++...#####...",
  ".+..##.#.#.##.",
  "+..#..#.#.#..#",
  "...#..#...#..#",
  "....##.....##.",
  nullptr,
};

static std::string compute_device_id() {
  uint8_t mac[6];
  esphome::get_mac_address_raw(mac);
  uint8_t hash[32];
  mbedtls_sha256(mac, sizeof(mac), hash, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  }
  return std::string(hex);
}

void BikeTracker::setup() {
  this->ws_client_.set_on_message([this](const std::string &payload) {
    this->handle_message_(payload);
  });

  this->ws_client_.set_on_connected([this]() {
    // defer the actual subscribe send and status update to loop()
    this->last_heartbeat_ = millis();
    this->has_ever_connected_ = true;
    this->consecutive_disconnects_ = 0;
    this->pending_subscribe_ = true;
  });

  this->ws_client_.set_on_disconnected([this]() {
    this->on_disconnect_();
  });

  {
    std::string user_agent;
    std::string headers;
    bool has_device_id = false;
    for (const auto &h : this->extra_headers_) {
      if (strcasecmp(h.first.c_str(), "user-agent") == 0) {
        user_agent = h.second;
      } else {
        if (strcasecmp(h.first.c_str(), "x-device-id") == 0) {
          has_device_id = true;
        }
        headers += h.first + ": " + h.second + "\r\n";
      }
    }

    if (!has_device_id) {
      auto device_id = compute_device_id();
      if (!device_id.empty()) {
        headers += "X-Device-Id: " + device_id + "\r\n";
      }
    }

    if (user_agent.empty()) {
#ifdef ESPHOME_PROJECT_NAME
      user_agent = ESPHOME_PROJECT_NAME "/" ESPHOME_PROJECT_VERSION
                   " ESPHome/" ESPHOME_VERSION " (" ESPHOME_VARIANT ") esp-idf/" IDF_VER;
#else
      user_agent = "ESPHome/" ESPHOME_VERSION " (" ESPHOME_VARIANT ") esp-idf/" IDF_VER;
#endif
    }

    this->ws_client_.set_user_agent(user_agent);
    this->ws_client_.set_headers(headers);
  }

  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "No base URL set; websocket will not start");
  } else {
    this->ws_client_.set_uri(this->base_url_);
    this->ws_client_.start();
  }
}

void BikeTracker::loop() {
  if (this->pending_subscribe_.exchange(false)) {
    this->status_clear_error();
    this->send_subscribe_();
  }

  unsigned long heartbeat = this->last_heartbeat_.load();
  if (heartbeat != 0 && millis() - heartbeat > HEARTBEAT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "No heartbeat for %lu ms (last_heartbeat=%lu, uptime=%lu)",
             millis() - heartbeat, heartbeat, millis());
    this->last_heartbeat_ = 0;
  }
}

void BikeTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Bike Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Stations: %s", this->stations_string_.c_str());
  ESP_LOGCONFIG(TAG, "  Limit: %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  Scroll station names: %s", this->scroll_station_names_ ? "true" : "false");
}

void BikeTracker::reconnect(const char *reason) {
  if (this->fully_closed_) {
    return;
  }

  ESP_LOGI(TAG, "Reconnecting websocket (reason: %s)", reason);
  this->last_heartbeat_ = 0;
  this->ws_client_.stop();

  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "Base URL is not set - cannot reconnect");
  } else {
    this->ws_client_.set_uri(this->base_url_); 
    this->ws_client_.start();
  }
}

void BikeTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }
  this->ws_client_.stop();
}

void BikeTracker::on_shutdown() {
  this->close(true);
}

void BikeTracker::on_disconnect_() {
  if (this->fully_closed_) {
    return;
  }

  int attempts = ++this->consecutive_disconnects_;
  ESP_LOGW(TAG, "Websocket disconnected (consecutive=%d, network_connected=%s, free_heap=%u)",
           attempts, esphome::network::is_connected() ? "yes" : "no",
           static_cast<unsigned>(esp_get_free_heap_size()));

  if (attempts >= CONNECT_FAILURE_ERROR_THRESHOLD) {
    this->status_set_error(LOG_STR("Failed to connect to WebSocket server"));
  }

  if (attempts >= CONNECT_FAILURE_REBOOT_THRESHOLD) {
    ESP_LOGE(TAG, "Could not connect to WebSocket server within %d attempts; rebooting to recover",
             CONNECT_FAILURE_REBOOT_THRESHOLD);
    App.reboot();
  }
}

void BikeTracker::send_subscribe_() {
  if (this->stations_string_.empty()) {
    ESP_LOGW(TAG, "No stations configured; not subscribing");
    return;
  }

  auto message = json::build_json([this](JsonObject root) {
    root["event"] = "stations:subscribe";

    auto data = root["data"].to<JsonObject>();
    if (!this->feed_code_.empty()) {
      data["feedCode"] = this->feed_code_;
    }
    data["stationIds"] = this->stations_string_;
  });

  ESP_LOGD(TAG, "Subscribing (%u bytes)", static_cast<unsigned>(message.size()));
  ESP_LOGV(TAG, "Subscribe payload: %s", message.c_str());
  if (!this->ws_client_.send_text(message)) {
    ESP_LOGW(TAG, "Subscribe send failed");
  }
}

void BikeTracker::handle_message_(const std::string &payload) {
  ESP_LOGV(TAG, "Received message (%u bytes): %s", static_cast<unsigned>(payload.size()), payload.c_str());

  bool valid = json::parse_json(payload, [this, &payload](JsonObject root) -> bool {
    auto event = root["event"].as<std::string>();

    if (event == "heartbeat") {
      ESP_LOGD(TAG, "Received heartbeat");
      this->last_heartbeat_ = millis();
      return true;
    }

    if (event != "stations") {
      ESP_LOGW(TAG, "Ignoring unknown event '%s' (%u bytes)", event.c_str(),
               static_cast<unsigned>(payload.size()));
      return true;
    }

    ESP_LOGD(TAG, "Received station update (%u bytes)", static_cast<unsigned>(payload.size()));

    std::vector<Station> new_stations;
    auto data = root["data"].as<JsonObject>();
    auto station_array = data["stations"].as<JsonArray>();
    new_stations.reserve(station_array.size());

    for (auto station : station_array) {
      bool is_installed = station["isInstalled"].as<bool>();
      new_stations.push_back({
        .station_id = station["stationId"].as<std::string>(),
        .name = station["name"].as<std::string>(),
        .docks_available = station["docksAvailable"].as<int>(),
        .mechanical_available = station["mechanicalBikesAvailable"].as<int>(),
        .electric_available = station["electricBikesAvailable"].as<int>(),
        .is_renting = is_installed && station["isRenting"].as<bool>(),
        .is_returning = is_installed && station["isReturning"].as<bool>(),
      });
    }

    {
      std::lock_guard<std::mutex> lock(this->station_state_.mutex);
      this->station_state_.stations = std::move(new_stations);
      this->station_state_.has_received_update = true;
    }

    return true;
  });

  if (!valid) {
    ESP_LOGW(TAG, "Failed to parse message (%u bytes); preview: %.120s",
             static_cast<unsigned>(payload.size()), payload.c_str());
    this->status_set_error(LOG_STR("Failed to parse station data"));
  }
}

void BikeTracker::set_abbreviations_from_text(const std::string &text) {
  this->abbreviations_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');

    if (parts.size() == 1) {
      // If only one part is provided, treat it as a removal (replace with empty string)
      this->add_abbreviation(parts[0], "");
      continue;
    }

    if (parts.size() != 2) {
      ESP_LOGW(TAG, "Invalid abbreviation line: %s", line.c_str());
      continue;
    }

    this->add_abbreviation(parts[0], parts[1]);
  }
}

void BikeTracker::set_station_names_from_text(const std::string &text) {
  this->station_names_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
      ESP_LOGW(TAG, "Invalid station name line: %s", line.c_str());
      continue;
    }
    this->add_station_name(parts[0], parts[1]);
  }
}

void BikeTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

std::string BikeTracker::display_name_(const Station &station) const {
  auto custom_name = this->station_names_.find(station.station_id);
  if (custom_name != this->station_names_.end()) {
    return custom_name->second;
  }

  std::string name = station.name;
  for (const auto &abbr : this->abbreviations_) {
    size_t pos = name.find(abbr.first);
    if (pos != std::string::npos) {
      name.replace(pos, abbr.first.length(), abbr.second);
    }
  }
  return name;
}

void BikeTracker::draw_icon_(const char *const *rows, int left_x, int bottom_y, Color color, Color accent_color) {
  int height = 0;
  while (rows[height] != nullptr) {
    height++;
  }

  for (int row = 0; row < height; row++) {
    int y = bottom_y - (height - 1 - row);
    for (int col = 0; rows[row][col] != '\0'; col++) {
      char pixel = rows[row][col];
      if (pixel == '#') {
        this->display_->draw_pixel_at(left_x + col, y, color);
      } else if (pixel == '+') {
        this->display_->draw_pixel_at(left_x + col, y, accent_color);
      }
    }
  }
}

void BikeTracker::draw_station_(
    const Station &station, int y_offset, unsigned long uptime,
    bool no_draw, int *name_overflow_out, int scroll_cycle_duration
) {
  struct Group {
    const char *const *icon;
    int count;
    Color color;
    bool available;
  };

  // Drawn right to left: e-bikes, mechanical bikes, then free docks
  const Group groups[] = {
    {EBIKE_ICON, station.electric_available, this->electric_color_, station.is_renting},
    {BIKE_ICON, station.mechanical_available, this->mechanical_color_, station.is_renting},
    {DOCK_ICON, station.docks_available, this->docks_color_, station.is_returning},
  };

  int count_width, _;
  this->font_->measure("00", &count_width, &_, &_, &_);
  int icon_bottom_y = y_offset + this->font_->get_ascender() - 1;

  // Each group is an icon followed by its count, in a fixed-width column
  // so that the groups line up from one row to the next.
  int x = this->display_->get_width();
  for (const auto &group : groups) {
    x -= count_width;
    if (!no_draw) {
      auto count = to_string(group.count);
      Color count_color = group.available && group.count > 0 ? this->count_color_ : this->unavailable_color_;
      this->display_->print(x, y_offset, this->font_, count_color, display::TextAlign::TOP_LEFT, count.c_str());
    }

    x -= ICON_COUNT_GAP + strlen(group.icon[0]);
    if (!no_draw) {
      Color icon_color = group.available ? group.color : this->unavailable_color_;
      Color accent_color = group.available ? this->bolt_color_ : this->unavailable_color_;
      this->draw_icon_(group.icon, x, icon_bottom_y, icon_color, accent_color);
    }
    x -= GROUP_GAP;
  }

  auto name = this->display_name_(station);

  int name_clipping_end = x;
  int name_actual_width;
  this->font_->measure(name.c_str(), &name_actual_width, &_, &_, &_);

  int name_overflow = name_actual_width - name_clipping_end;
  if (name_overflow_out) {
    *name_overflow_out = name_overflow;
  }

  if (no_draw) {
    return;
  }

  int scroll_offset = 0;
  if (name_overflow > 0 && scroll_cycle_duration > 0) {
    int scroll_time = name_overflow * 1000 / scroll_speed;
    int scroll_cycle_time = uptime % scroll_cycle_duration;

    // Scroll idle (left side - default)
    if(scroll_cycle_time < idle_time_left) {
      // scroll_offset = 0; do nothing
    } else if (scroll_cycle_time < idle_time_left + scroll_time) {
      // Scrolling left
      int time_since_scroll_start = scroll_cycle_time - idle_time_left;
      scroll_offset = time_since_scroll_start * scroll_speed / 1000;
    } else if (scroll_cycle_time < idle_time_left + scroll_time + idle_time_right) {
      // Scroll idle (right side)
      scroll_offset = name_overflow;
    } else if (scroll_cycle_time < idle_time_left + 2 * scroll_time + idle_time_right){
      // Scrolling right
      int time_since_scroll_start = scroll_cycle_time - (idle_time_left + scroll_time + idle_time_right);
      scroll_offset = name_overflow - (time_since_scroll_start * scroll_speed / 1000);
    } else {
      // Waiting for other names to finish scrolling
      // scroll_offset = 0; do nothing
    }
  }

  Color name_color = station.is_renting || station.is_returning ? Color(0xFFFFFF) : this->unavailable_color_;
  this->display_->start_clipping(0, 0, name_clipping_end, this->display_->get_height());
  this->display_->print(-scroll_offset, y_offset, this->font_, name_color, display::TextAlign::TOP_LEFT, name.c_str());
  this->display_->end_clipping();
}

void HOT BikeTracker::draw_stations() {
  if (this->display_ == nullptr) [[unlikely]] {
    ESP_LOGW(TAG, "No display attached, cannot draw stations");
    return;
  }

  if (!esphome::network::is_connected()) {
    this->draw_text_centered_("Waiting for network", Color(0x252627));
    return;
  }

  if (this->base_url_.empty()) {
    this->draw_text_centered_("No base URL set", Color(0x252627));
    return;
  }

  if (this->stations_string_.empty()) {
    this->draw_text_centered_("No stations selected", Color(0x252627));
    return;
  }

  if (this->status_has_error()) {
    this->draw_text_centered_("Error loading stations", Color(0xFE4C5C));
    return;
  }

  std::lock_guard<std::mutex> lock(this->station_state_.mutex);

  if (!this->has_ever_connected_.load() || !this->station_state_.has_received_update) {
    this->draw_text_centered_("Loading...", Color(0x252627));
    return;
  }

  if (this->station_state_.stations.empty()) {
    this->draw_text_centered_("Stations not found", Color(0x252627));
    return;
  }

  int nominal_font_height = this->font_->get_ascender() + this->font_->get_descender();
  unsigned long uptime = millis();

  // The header text takes the place of one station row
  bool has_header_text = !this->header_text_.empty();
  int max_rows = has_header_text ? this->limit_ - 1 : this->limit_;
  int row_count = std::min<int>(max_rows, this->station_state_.stations.size());

  int scroll_cycle_duration = 0;
  if (this->scroll_station_names_) {
    int largest_name_overflow = 0;
    for (int i = 0; i < row_count; i++) {
      int name_overflow;
      this->draw_station_(this->station_state_.stations[i], 0, uptime, true, &name_overflow);
      largest_name_overflow = std::max(largest_name_overflow, name_overflow);
    }

    if (largest_name_overflow > 0) {
      int longest_scroll_time = largest_name_overflow * 1000 / scroll_speed;
      scroll_cycle_duration = idle_time_left + idle_time_right + 2*longest_scroll_time;
    }
  }

  int max_rows_height = (this->limit_ * this->font_->get_ascender()) + ((this->limit_ - 1) * this->font_->get_descender());
  int y_offset = (this->display_->get_height() % max_rows_height) / 2;

  if (has_header_text) {
    this->display_->print(0, y_offset, this->font_, Color(0x00bdbd), display::TextAlign::LEFT, this->header_text_.c_str());
    y_offset += nominal_font_height;
  }

  for (int i = 0; i < row_count; i++) {
    this->draw_station_(this->station_state_.stations[i], y_offset, uptime, false, nullptr, scroll_cycle_duration);
    y_offset += nominal_font_height;
  }
}

}  // namespace bike_tracker
}  // namespace esphome
