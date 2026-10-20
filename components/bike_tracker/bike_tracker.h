#pragma once

#include <atomic>
#include <map>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"

#include "station_state.h"
#include "websocket_client.h"

namespace esphome {
namespace bike_tracker {

class BikeTracker : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    bool is_connected() const { return this->ws_client_.is_connected(); }
    void reconnect(const char *reason);
    void close(bool fully = false);

    void draw_stations();

    void set_display(display::Display *display) { display_ = display; }
    void set_font(font::Font *font) { font_ = font; }

    void set_base_url(const std::string &base_url) { base_url_ = base_url; }
    void set_feed_code(const std::string &feed_code) { feed_code_ = feed_code; }
    void set_stations_string(const std::string &stations_string) { stations_string_ = stations_string; }
    void set_limit(int limit) { limit_ = limit; }
    void set_scroll_station_names(bool scroll_station_names) { scroll_station_names_ = scroll_station_names; }

    void set_header_text(const std::string &header_text) { header_text_ = header_text; }
    void add_abbreviation(const std::string &from, const std::string &to) { abbreviations_[from] = to; }
    void add_station_name(const std::string &station_id, const std::string &name) { station_names_[station_id] = name; }
    void add_header(const std::string &name, const std::string &value) { extra_headers_.emplace_back(name, value); }

    void set_docks_color(const Color &color) { docks_color_ = color; }
    void set_mechanical_color(const Color &color) { mechanical_color_ = color; }
    void set_electric_color(const Color &color) { electric_color_ = color; }

    void set_abbreviations_from_text(const std::string &text);
    void set_station_names_from_text(const std::string &text);

  protected:
    static constexpr int scroll_speed = 10; // pixels/second
    static constexpr int idle_time_left = 5000;
    static constexpr int idle_time_right = 1000;

    void draw_text_centered_(const char *text, Color color);
    void draw_icon_(const char *const *rows, int left_x, int bottom_y, Color color, Color accent_color);
    std::string display_name_(const Station &station) const;

    void draw_station_(
      const Station &station, int y_offset, unsigned long uptime,
      bool no_draw = false, int *name_overflow_out = nullptr, int scroll_cycle_duration = 0
    );

    StationState station_state_;

    display::Display *display_;
    font::Font *font_;

    WebSocketClient ws_client_;

    void handle_message_(const std::string &payload);
    void send_subscribe_();
    void on_disconnect_();

    std::atomic<int> consecutive_disconnects_{0};
    std::atomic<unsigned long> last_heartbeat_{0};
    std::atomic<bool> has_ever_connected_{false};
    std::atomic<bool> pending_subscribe_{false};
    std::atomic<bool> fully_closed_{false};

    std::string base_url_;
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    std::string feed_code_;
    std::string stations_string_;
    int limit_;

    std::string header_text_;
    std::map<std::string, std::string> abbreviations_;
    std::map<std::string, std::string> station_names_;
    bool scroll_station_names_ = true;

    Color docks_color_ = Color(0x9A9A9A);
    Color mechanical_color_ = Color(0xE2001A);
    Color electric_color_ = Color(0x2ECC40);
    Color bolt_color_ = Color(0xFFD200);
    Color count_color_ = Color(0xFFFFFF);
    Color unavailable_color_ = Color(0x404040);
};


}  // namespace bike_tracker
}  // namespace esphome
