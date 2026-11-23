#include "transit_tracker.h"
#include "string_utils.h"

#include <algorithm>
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
namespace transit_tracker {

static const char *const TAG = "transit_tracker.component";

static constexpr int CONNECT_FAILURE_ERROR_THRESHOLD = 3;
static constexpr int CONNECT_FAILURE_REBOOT_THRESHOLD = 15;
static constexpr unsigned long HEARTBEAT_TIMEOUT_MS = 60000;
// Departures requested for all transit rows together (the API's maximum).
// They come sorted, and each transit row picks the next one of its line.
static constexpr int SCHEDULE_LIMIT = 20;
static constexpr int ICON_COUNT_GAP = 1;
static constexpr int GROUP_GAP = 3;

static const Color MESSAGE_COLOR = Color(0x252627);

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

// The Vélo'v logo (a "V" with an arrow), shown on the left of each bike row
// like the line badge of a transit row.
static const char *const BIKE_BADGE_ICON[] = {
  ".....##.",
  "....###.",
  "##..####",
  "###.##..",
  ".##.##..",
  ".#####..",
  ".####...",
  "..###...",
  nullptr,
};

static const uint8_t REALTIME_ICON[6][6] = {
  {0, 0, 0, 3, 3, 3},
  {0, 0, 3, 0, 0, 0},
  {0, 3, 0, 0, 2, 2},
  {3, 0, 0, 2, 0, 0},
  {3, 0, 2, 0, 0, 1},
  {3, 0, 2, 0, 1, 1}
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

static bool parse_hex_color(const std::string &str, uint32_t &out) {
  if (str.empty()) {
    return false;
  }
  char *end = nullptr;
  unsigned long value = std::strtoul(str.c_str(), &end, 16);
  if (end == str.c_str() || *end != '\0') {
    return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

static std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r");
  if (start == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(" \t\r");
  return s.substr(start, end - start + 1);
}

void TransitTracker::setup() {
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

void TransitTracker::loop() {
  if (millis() - this->last_slot_check_ >= 1000) {
    this->update_active_slot_();
  }

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

void TransitTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  for (const auto &slot : this->slots_) {
    if (slot.is_default) {
      ESP_LOGCONFIG(TAG, "  Default rows: %u", static_cast<unsigned>(slot.rows.size()));
    } else {
      ESP_LOGCONFIG(TAG, "  Rows from %02d:%02d to %02d:%02d: %u", slot.start_minute / 60, slot.start_minute % 60,
                    slot.end_minute / 60, slot.end_minute % 60, static_cast<unsigned>(slot.rows.size()));
    }
    for (const auto &row : slot.rows) {
      ESP_LOGCONFIG(TAG, "    %s %s", row.type == RowType::TRANSIT ? "transit" : "bike", row.id.c_str());
    }
  }
  ESP_LOGCONFIG(TAG, "  Limit: %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  Scroll names: %s", this->scroll_names_ ? "true" : "false");
}

void TransitTracker::reconnect(const char *reason) {
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

void TransitTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }
  this->ws_client_.stop();
}

void TransitTracker::on_shutdown() {
  this->close(true);
}

void TransitTracker::on_disconnect_() {
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

bool TransitTracker::has_transit_rows_() const {
  for (const auto &row : this->rows_) {
    if (row.type == RowType::TRANSIT) {
      return true;
    }
  }
  return false;
}

bool TransitTracker::has_bike_rows_() const {
  for (const auto &row : this->rows_) {
    if (row.type == RowType::BIKE) {
      return true;
    }
  }
  return false;
}

void TransitTracker::send_subscribe_() {
  if (this->rows_.empty()) {
    ESP_LOGW(TAG, "No rows configured; not subscribing");
    return;
  }

  std::string route_stop_pairs;
  std::string station_ids;
  for (const auto &row : this->rows_) {
    if (row.type == RowType::TRANSIT) {
      std::string pair = row.route_id + "," + row.stop_id + "," + to_string(row.time_offset);
      // The same line can fill several rows; subscribe to it once
      if ((";" + route_stop_pairs + ";").find(";" + pair + ";") != std::string::npos) {
        continue;
      }
      if (!route_stop_pairs.empty()) {
        route_stop_pairs += ";";
      }
      route_stop_pairs += pair;
    } else {
      if (!station_ids.empty()) {
        station_ids += ";";
      }
      station_ids += row.id;
    }
  }

  if (!route_stop_pairs.empty()) {
    auto message = json::build_json([this, &route_stop_pairs](JsonObject root) {
      root["event"] = "schedule:subscribe";

      auto data = root["data"].to<JsonObject>();
      if (!this->feed_code_.empty()) {
        data["feedCode"] = this->feed_code_;
      }
      data["routeStopPairs"] = route_stop_pairs;
      data["limit"] = SCHEDULE_LIMIT;
      data["sortByDeparture"] = this->display_departure_times_;
      data["listMode"] = "sequential";
    });

    ESP_LOGD(TAG, "Subscribing to schedule (%u bytes)", static_cast<unsigned>(message.size()));
    ESP_LOGV(TAG, "Subscribe payload: %s", message.c_str());
    if (!this->ws_client_.send_text(message)) {
      ESP_LOGW(TAG, "Schedule subscribe send failed");
    }
  }

  if (!station_ids.empty()) {
    auto message = json::build_json([this, &station_ids](JsonObject root) {
      root["event"] = "stations:subscribe";

      auto data = root["data"].to<JsonObject>();
      if (!this->feed_code_.empty()) {
        data["feedCode"] = this->feed_code_;
      }
      data["stationIds"] = station_ids;
    });

    ESP_LOGD(TAG, "Subscribing to stations (%u bytes)", static_cast<unsigned>(message.size()));
    ESP_LOGV(TAG, "Subscribe payload: %s", message.c_str());
    if (!this->ws_client_.send_text(message)) {
      ESP_LOGW(TAG, "Stations subscribe send failed");
    }
  }
}

void TransitTracker::handle_message_(const std::string &payload) {
  ESP_LOGV(TAG, "Received message (%u bytes): %s", static_cast<unsigned>(payload.size()), payload.c_str());

  bool valid = json::parse_json(payload, [this, &payload](JsonObject root) -> bool {
    auto event = root["event"].as<std::string>();

    if (event == "heartbeat") {
      ESP_LOGD(TAG, "Received heartbeat");
      this->last_heartbeat_ = millis();
      return true;
    }

    if (event == "schedule") {
      ESP_LOGD(TAG, "Received schedule update (%u bytes)", static_cast<unsigned>(payload.size()));

      std::vector<Trip> new_trips;
      auto data = root["data"].as<JsonObject>();
      auto trip_array = data["trips"].as<JsonArray>();
      new_trips.reserve(trip_array.size());

      for (auto trip : trip_array) {
        auto route_id = trip["routeId"].as<std::string>();
        auto route_style = this->route_styles_.find(route_id);

        Color route_color = this->default_route_color_;
        std::string route_name = trip["routeName"].as<std::string>();

        if (route_style != this->route_styles_.end()) {
          route_color = route_style->second.color;
          route_name = route_style->second.name;
        } else if (!trip["routeColor"].isNull()) {
          auto color_str = trip["routeColor"].as<std::string>();
          uint32_t parsed_color;
          if (parse_hex_color(color_str, parsed_color)) {
            route_color = Color(parsed_color);
          } else if (!color_str.empty()) {
            ESP_LOGW(TAG, "Ignoring invalid routeColor '%s' for route %s",
                     color_str.c_str(), route_id.c_str());
          }
        }

        new_trips.push_back({
          .route_id = route_id,
          .stop_id = trip["stopId"].as<std::string>(),
          .route_name = route_name,
          .route_color = route_color,
          .headsign = this->abbreviate_(trip["headsign"].as<std::string>()),
          .arrival_time = trip["arrivalTime"].as<time_t>(),
          .departure_time = trip["departureTime"].as<time_t>(),
          .is_realtime = trip["isRealtime"].as<bool>(),
        });
      }

      std::lock_guard<std::mutex> lock(this->state_.mutex);
      this->state_.trips = std::move(new_trips);
      this->state_.has_schedule_update = true;
      return true;
    }

    if (event == "stations") {
      ESP_LOGD(TAG, "Received station update (%u bytes)", static_cast<unsigned>(payload.size()));

      std::map<std::string, Station> new_stations;
      auto data = root["data"].as<JsonObject>();
      for (auto station : data["stations"].as<JsonArray>()) {
        bool is_installed = station["isInstalled"].as<bool>();
        auto station_id = station["stationId"].as<std::string>();
        new_stations[station_id] = {
          .station_id = station_id,
          .name = station["name"].as<std::string>(),
          .docks_available = station["docksAvailable"].as<int>(),
          .mechanical_available = station["mechanicalBikesAvailable"].as<int>(),
          .electric_available = station["electricBikesAvailable"].as<int>(),
          .is_renting = is_installed && station["isRenting"].as<bool>(),
          .is_returning = is_installed && station["isReturning"].as<bool>(),
        };
      }

      std::lock_guard<std::mutex> lock(this->state_.mutex);
      this->state_.stations = std::move(new_stations);
      this->state_.has_stations_update = true;
      return true;
    }

    ESP_LOGW(TAG, "Ignoring unknown event '%s' (%u bytes)", event.c_str(),
             static_cast<unsigned>(payload.size()));
    return true;
  });

  if (!valid) {
    ESP_LOGW(TAG, "Failed to parse message (%u bytes); preview: %.120s",
             static_cast<unsigned>(payload.size()), payload.c_str());
    this->status_set_error(LOG_STR("Failed to parse data"));
  }
}

// Parses "HHMM-HHMM" into minutes since midnight
static bool parse_slot_times(const std::string &text, int &start, int &end) {
  auto times = split(text, '-');
  if (times.size() != 2) {
    return false;
  }
  int values[2];
  for (int i = 0; i < 2; i++) {
    auto t = trim(times[i]);
    if (t.size() != 4 || t.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    int hours = atoi(t.substr(0, 2).c_str());
    int minutes = atoi(t.substr(2, 2).c_str());
    if (hours > 24 || minutes > 59 || (hours == 24 && minutes != 0)) {
      return false;
    }
    values[i] = (hours * 60 + minutes) % (24 * 60);
  }
  start = values[0];
  end = values[1];
  return true;
}

void TransitTracker::set_rows_from_text(const std::string &text) {
  if (text == this->rows_text_) {
    return;
  }
  this->rows_text_ = text;

  // slots[0] is the default slot; each "s;" line starts a new one
  std::vector<TimeSlot> slots(1);
  for (const auto &raw_line : split(text, '\n')) {
    auto line = trim(raw_line);
    if (line.empty()) {
      continue;
    }

    auto parts = split(line, ';');
    if (parts.size() != 2 || parts[1].empty()) {
      ESP_LOGW(TAG, "Invalid row line: %s", line.c_str());
      continue;
    }

    if (parts[0] == "s") {
      TimeSlot slot;
      slot.is_default = false;
      if (!parse_slot_times(parts[1], slot.start_minute, slot.end_minute)) {
        ESP_LOGW(TAG, "Invalid time slot (expected s;HHMM-HHMM): %s", line.c_str());
        // Keep its rows out of the previous slot
        slot.start_minute = slot.end_minute = -1;
      }
      slots.push_back(slot);
    } else if (parts[0] == "b") {
      Row row;
      row.type = RowType::BIKE;
      row.id = trim(parts[1]);
      slots.back().rows.push_back(row);
    } else if (parts[0] == "t") {
      auto fields = split(parts[1], ',');
      if (fields.size() < 2 || fields.size() > 3 || fields[0].empty() || fields[1].empty()) {
        ESP_LOGW(TAG, "Invalid transit row (expected routeId,stopId[,offset]): %s", line.c_str());
        continue;
      }
      Row row;
      row.type = RowType::TRANSIT;
      row.route_id = trim(fields[0]);
      row.stop_id = trim(fields[1]);
      row.time_offset = fields.size() == 3 ? atoi(fields[2].c_str()) : 0;
      row.id = row.route_id + "," + row.stop_id;
      slots.back().rows.push_back(row);
    } else {
      ESP_LOGW(TAG, "Unknown row type '%s' (expected t, b or s): %s", parts[0].c_str(), line.c_str());
    }
  }

  // Drop slots whose times could not be read
  slots.erase(std::remove_if(slots.begin() + 1, slots.end(),
                             [](const TimeSlot &slot) { return slot.start_minute < 0; }),
              slots.end());

  this->slots_ = std::move(slots);
  this->update_active_slot_(true);
}

int TransitTracker::current_slot_index_() const {
  if (this->rtc_ != nullptr) {
    auto now = this->rtc_->now();
    if (now.is_valid()) {
      int minute = now.hour * 60 + now.minute;
      for (size_t i = 1; i < this->slots_.size(); i++) {
        if (this->slots_[i].contains(minute)) {
          return i;
        }
      }
    }
  }
  if (!this->slots_.empty() && !this->slots_[0].rows.empty()) {
    return 0;
  }
  return -1;
}

void TransitTracker::update_active_slot_(bool force) {
  this->last_slot_check_ = millis();
  int index = this->current_slot_index_();
  if (!force && index == this->active_slot_) {
    return;
  }

  if (index != this->active_slot_) {
    ESP_LOGI(TAG, "Showing time slot %d", index);
  }
  this->active_slot_ = index;

  {
    std::lock_guard<std::mutex> lock(this->state_.mutex);
    if (index >= 0) {
      this->rows_ = this->slots_[index].rows;
    } else {
      this->rows_.clear();
    }
    this->state_.trips.clear();
    this->state_.stations.clear();
    this->state_.has_schedule_update = false;
    this->state_.has_stations_update = false;
  }

  // The API only allows one subscription of each kind per connection, so
  // new rows need a fresh connection.
  if (this->has_ever_connected_.load()) {
    this->reconnect("rows changed");
  }
}

void TransitTracker::set_row_names_from_text(const std::string &text) {
  this->row_names_.clear();
  for (const auto &raw_line : split(text, '\n')) {
    auto line = trim(raw_line);
    if (line.empty()) {
      continue;
    }
    auto parts = split(line, ';');
    if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
      ESP_LOGW(TAG, "Invalid row name line: %s", line.c_str());
      continue;
    }
    this->add_row_name(trim(parts[0]), parts[1]);
  }
}

void TransitTracker::set_abbreviations_from_text(const std::string &text) {
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

void TransitTracker::set_route_styles_from_text(const std::string &text) {
  this->route_styles_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 3) {
      ESP_LOGW(TAG, "Invalid route style line: %s", line.c_str());
      continue;
    }
    uint32_t color;
    if (!parse_hex_color(parts[2], color)) {
      ESP_LOGW(TAG, "Invalid route style color '%s' in line: %s", parts[2].c_str(), line.c_str());
      continue;
    }
    this->add_route_style(parts[0], parts[1], Color(color));
  }
}

void TransitTracker::set_realtime_color(const Color &color) {
  this->realtime_color_ = color;
  this->realtime_color_dark_ = Color(
    (color.r * 0.5),
    (color.g * 0.5),
    (color.b * 0.5)
  );
}

std::string TransitTracker::abbreviate_(const std::string &text) const {
  std::string result = text;
  for (const auto &abbr : this->abbreviations_) {
    if (abbr.first.empty()) {
      continue;
    }
    size_t pos = result.find(abbr.first);
    if (pos != std::string::npos) {
      result.replace(pos, abbr.first.length(), abbr.second);
    }
  }
  return result;
}

const std::string *TransitTracker::custom_name_(const std::string &row_id) const {
  auto it = this->row_names_.find(row_id);
  if (it == this->row_names_.end()) {
    return nullptr;
  }
  return &it->second;
}

void TransitTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

void TransitTracker::draw_icon_(const char *const *rows, int left_x, int bottom_y, Color color, Color accent_color) {
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

int TransitTracker::draw_bike_badge_(int left_x, int y_offset, bool available) {
  int bottom_y = y_offset + this->font_->get_ascender() - 1;
  this->draw_icon_(BIKE_BADGE_ICON, left_x, bottom_y, available ? this->bike_badge_color_ : this->unavailable_color_,
                   Color(0xFFFFFF));
  return strlen(BIKE_BADGE_ICON[0]);
}

void HOT TransitTracker::draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long uptime) {
  const int num_frames = 6;
  const int idle_frame_duration = 3000;
  const int anim_frame_duration = 200;
  const int cycle_duration = idle_frame_duration + (num_frames - 1) * anim_frame_duration;

  unsigned long cycle_time = uptime % cycle_duration;

  int frame;
  if (cycle_time < idle_frame_duration) {
    frame = 0;
  } else {
    frame = 1 + (cycle_time - idle_frame_duration) / anim_frame_duration;
  }

  auto is_segment_lit = [frame](uint8_t segment) {
    switch (segment) {
      case 1: return frame >= 1 && frame <= 3;
      case 2: return frame >= 2 && frame <= 4;
      case 3: return frame >= 3 && frame <= 5;
      default: return false;
    }
  };

  for (uint8_t i = 0; i < 6; ++i) {
    for (uint8_t j = 0; j < 6; ++j) {
      uint8_t segment_number = REALTIME_ICON[i][j];
      if (segment_number == 0) {
        continue;
      }

      Color icon_color = is_segment_lit(segment_number) ? this->realtime_color_ : this->realtime_color_dark_;
      this->display_->draw_pixel_at(bottom_right_x - (5 - j), bottom_right_y - (5 - i), icon_color);
    }
  }
}

void TransitTracker::draw_scrolling_text_(const std::string &text, Color color, int clip_start, int clip_end,
                                          int y_offset, unsigned long uptime, int scroll_cycle_duration) {
  int text_width, _;
  this->font_->measure(text.c_str(), &text_width, &_, &_, &_);
  int overflow = text_width - (clip_end - clip_start);

  int scroll_offset = 0;
  if (overflow > 0 && scroll_cycle_duration > 0) {
    int scroll_time = overflow * 1000 / scroll_speed;
    int scroll_cycle_time = uptime % scroll_cycle_duration;

    if (scroll_cycle_time < idle_time_left) {
      // Scroll idle (left side - default)
    } else if (scroll_cycle_time < idle_time_left + scroll_time) {
      // Scrolling left
      int time_since_scroll_start = scroll_cycle_time - idle_time_left;
      scroll_offset = time_since_scroll_start * scroll_speed / 1000;
    } else if (scroll_cycle_time < idle_time_left + scroll_time + idle_time_right) {
      // Scroll idle (right side)
      scroll_offset = overflow;
    } else if (scroll_cycle_time < idle_time_left + 2 * scroll_time + idle_time_right) {
      // Scrolling right
      int time_since_scroll_start = scroll_cycle_time - (idle_time_left + scroll_time + idle_time_right);
      scroll_offset = overflow - (time_since_scroll_start * scroll_speed / 1000);
    } else {
      // Waiting for other names to finish scrolling
    }
  }

  this->display_->start_clipping(clip_start, 0, clip_end, this->display_->get_height());
  this->display_->print(clip_start - scroll_offset, y_offset, this->font_, color, display::TextAlign::TOP_LEFT,
                        text.c_str());
  this->display_->end_clipping();
}

const Trip *TransitTracker::trip_for_row_(int index, uint rtc_now) const {
  const Row &row = this->rows_[index];

  // When the same line at the same stop fills several rows, the first shows
  // the next departure, the second the one after, and so on.
  int occurrence = 0;
  for (int i = 0; i < index; i++) {
    if (this->rows_[i].type == RowType::TRANSIT && this->rows_[i].id == row.id) {
      occurrence++;
    }
  }

  for (const auto &trip : this->state_.trips) {
    if (trip.route_id != row.route_id || trip.stop_id != row.stop_id) {
      continue;
    }
    time_t time = this->display_departure_times_ ? trip.departure_time : trip.arrival_time;
    if (time < static_cast<time_t>(rtc_now)) {
      continue;
    }
    if (occurrence == 0) {
      return &trip;
    }
    occurrence--;
  }
  return nullptr;
}

int TransitTracker::draw_transit_row_(const Row &row, const Trip *trip, int y_offset, int font_height,
                                      unsigned long uptime, uint rtc_now, bool no_draw, int scroll_cycle_duration) {
  int _;

  // Route badge: the route's short name in its color. Without an upcoming
  // trip, fall back to the route style or the route ID.
  std::string route_name;
  Color route_color = this->default_route_color_;
  if (trip != nullptr) {
    route_name = trip->route_name;
    route_color = trip->route_color;
  } else {
    auto style = this->route_styles_.find(row.route_id);
    if (style != this->route_styles_.end()) {
      route_name = style->second.name;
      route_color = style->second.color;
    } else {
      auto colon = row.route_id.find(':');
      route_name = colon == std::string::npos ? row.route_id : row.route_id.substr(colon + 1);
    }
  }

  int route_width;
  this->font_->measure(route_name.c_str(), &route_width, &_, &_, &_);
  if (!no_draw) {
    this->display_->print(0, y_offset, this->font_, route_color, display::TextAlign::TOP_LEFT, route_name.c_str());
  }

  std::string time_display = "--";
  if (trip != nullptr) {
    time_display = this->localization_.fmt_duration_from_now(
      this->display_departure_times_ ? trip->departure_time : trip->arrival_time, rtc_now);
  }

  int time_width;
  this->font_->measure(time_display.c_str(), &time_width, &_, &_, &_);

  int name_clipping_start = route_width + 3;
  int name_clipping_end = this->display_->get_width() - time_width - 2;

  bool is_realtime = trip != nullptr && trip->is_realtime;
  if (!no_draw) {
    Color time_color = is_realtime ? this->realtime_color_ : Color(0xa7a7a7);
    this->display_->print(this->display_->get_width() + 1, y_offset, this->font_, time_color,
                          display::TextAlign::TOP_RIGHT, time_display.c_str());
  }

  if (is_realtime) {
    name_clipping_end -= 8;
    if (!no_draw) {
      int icon_bottom_right_x = this->display_->get_width() - time_width - 2;
      int icon_bottom_right_y = y_offset + font_height - 6;
      this->draw_realtime_icon_(icon_bottom_right_x, icon_bottom_right_y, uptime);
    }
  }

  // The row's custom name if it has one, else the trip's destination
  const std::string *custom_name = this->custom_name_(row.id);
  std::string name;
  Color name_color = Color(0xFFFFFF);
  if (custom_name != nullptr) {
    name = *custom_name;
  } else if (trip != nullptr) {
    name = trip->headsign;
  } else {
    name = "No departures";
    name_color = this->unavailable_color_;
  }

  int name_width;
  this->font_->measure(name.c_str(), &name_width, &_, &_, &_);
  int overflow = name_width - (name_clipping_end - name_clipping_start);

  if (!no_draw) {
    this->draw_scrolling_text_(name, name_color, name_clipping_start, name_clipping_end, y_offset, uptime,
                               scroll_cycle_duration);
  }
  return overflow;
}

int TransitTracker::draw_bike_row_(const Row &row, const Station *station, int y_offset, unsigned long uptime,
                                   bool no_draw, int scroll_cycle_duration) {
  struct Group {
    const char *const *icon;
    int count;
    Color color;
    bool available;
  };

  bool is_renting = station != nullptr && station->is_renting;
  bool is_returning = station != nullptr && station->is_returning;

  // Drawn right to left: e-bikes, mechanical bikes, then free docks
  const Group groups[] = {
    {EBIKE_ICON, station ? station->electric_available : 0, this->electric_color_, is_renting},
    {BIKE_ICON, station ? station->mechanical_available : 0, this->mechanical_color_, is_renting},
    {DOCK_ICON, station ? station->docks_available : 0, this->docks_color_, is_returning},
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
      auto count = station ? to_string(group.count) : std::string("-");
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

  const std::string *custom_name = this->custom_name_(row.id);
  std::string name;
  if (custom_name != nullptr) {
    name = *custom_name;
  } else if (station != nullptr) {
    name = this->abbreviate_(station->name);
  } else {
    name = "Station not found";
  }

  int badge_width = strlen(BIKE_BADGE_ICON[0]);
  if (!no_draw) {
    this->draw_bike_badge_(0, y_offset, is_renting || is_returning);
  }
  int name_start = badge_width + 3;

  int name_width;
  this->font_->measure(name.c_str(), &name_width, &_, &_, &_);
  int overflow = name_width - (x - name_start);

  if (!no_draw) {
    Color name_color = is_renting || is_returning ? Color(0xFFFFFF) : this->unavailable_color_;
    this->draw_scrolling_text_(name, name_color, name_start, x, y_offset, uptime, scroll_cycle_duration);
  }
  return overflow;
}

int TransitTracker::draw_row_(int index, int y_offset, int font_height, unsigned long uptime, uint rtc_now,
                              bool no_draw, int scroll_cycle_duration) {
  const Row &row = this->rows_[index];
  if (row.type == RowType::TRANSIT) {
    return this->draw_transit_row_(row, this->trip_for_row_(index, rtc_now), y_offset, font_height, uptime, rtc_now,
                                   no_draw, scroll_cycle_duration);
  }

  auto it = this->state_.stations.find(row.id);
  const Station *station = it == this->state_.stations.end() ? nullptr : &it->second;
  return this->draw_bike_row_(row, station, y_offset, uptime, no_draw, scroll_cycle_duration);
}

void HOT TransitTracker::draw_schedule() {
  if (this->display_ == nullptr) [[unlikely]] {
    ESP_LOGW(TAG, "No display attached, cannot draw rows");
    return;
  }

  if (!esphome::network::is_connected()) {
    this->draw_text_centered_("Waiting for network", MESSAGE_COLOR);
    return;
  }

  if (this->base_url_.empty()) {
    this->draw_text_centered_("No base URL set", MESSAGE_COLOR);
    return;
  }

  std::lock_guard<std::mutex> lock(this->state_.mutex);

  bool has_any_rows = false;
  for (const auto &slot : this->slots_) {
    has_any_rows = has_any_rows || !slot.rows.empty();
  }
  if (!has_any_rows) {
    this->draw_text_centered_("No rows configured", MESSAGE_COLOR);
    return;
  }

  if (this->rows_.empty()) {
    // Only time slots, and none active: wait for the clock, else stay dark
    if (this->rtc_ == nullptr || !this->rtc_->now().is_valid()) {
      this->draw_text_centered_("Waiting for time sync", MESSAGE_COLOR);
    }
    return;
  }

  bool has_transit_rows = this->has_transit_rows_();
  bool has_bike_rows = this->has_bike_rows_();

  // Departure times need the clock; bike rows don't
  bool time_valid = this->rtc_ != nullptr && this->rtc_->now().is_valid();
  if (has_transit_rows && !time_valid) {
    this->draw_text_centered_("Waiting for time sync", MESSAGE_COLOR);
    return;
  }

  if (this->status_has_error()) {
    this->draw_text_centered_("Error loading data", Color(0xFE4C5C));
    return;
  }

  if (!this->has_ever_connected_.load() ||
      (has_transit_rows && !this->state_.has_schedule_update) ||
      (has_bike_rows && !this->state_.has_stations_update)) {
    this->draw_text_centered_("Loading...", MESSAGE_COLOR);
    return;
  }

  int nominal_font_height = this->font_->get_ascender() + this->font_->get_descender();
  unsigned long uptime = millis();
  uint rtc_now = time_valid ? this->rtc_->now().timestamp : 0;

  // The header text takes the place of one row
  bool has_header_text = !this->header_text_.empty();
  int max_rows = has_header_text ? this->limit_ - 1 : this->limit_;
  int row_count = std::min<int>(max_rows, this->rows_.size());

  // All names scroll in step, so the cycle lasts as long as the longest one needs
  int scroll_cycle_duration = 0;
  if (this->scroll_names_) {
    int largest_overflow = 0;
    for (int i = 0; i < row_count; i++) {
      int overflow = this->draw_row_(i, 0, nominal_font_height, uptime, rtc_now, true, 0);
      largest_overflow = std::max(largest_overflow, overflow);
    }

    if (largest_overflow > 0) {
      int longest_scroll_time = largest_overflow * 1000 / scroll_speed;
      scroll_cycle_duration = idle_time_left + idle_time_right + 2 * longest_scroll_time;
    }
  }

  int max_rows_height = (this->limit_ * this->font_->get_ascender()) + ((this->limit_ - 1) * this->font_->get_descender());
  int y_offset = (this->display_->get_height() % max_rows_height) / 2;

  if (has_header_text) {
    this->display_->print(0, y_offset, this->font_, Color(0x00bdbd), display::TextAlign::LEFT, this->header_text_.c_str());
    y_offset += nominal_font_height;
  }

  for (int i = 0; i < row_count; i++) {
    this->draw_row_(i, y_offset, nominal_font_height, uptime, rtc_now, false, scroll_cycle_duration);
    y_offset += nominal_font_height;
  }
}

}  // namespace transit_tracker
}  // namespace esphome
