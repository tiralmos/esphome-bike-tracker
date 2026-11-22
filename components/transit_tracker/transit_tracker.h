#pragma once

#include <atomic>
#include <map>
#include <utility>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

#include "localization.h"
#include "row_state.h"
#include "websocket_client.h"

namespace esphome {
namespace transit_tracker {

struct RouteStyle {
  std::string name;
  Color color;
};

// Shows a list of rows on the display. Each row is either a transit line at
// a stop (next departure, from the API's schedule:subscribe) or a bike-share
// station (free docks, bikes and e-bikes, from stations:subscribe). Both
// subscriptions share one WebSocket connection.
class TransitTracker : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    bool is_connected() const { return this->ws_client_.is_connected(); }
    void reconnect(const char *reason);
    void close(bool fully = false);

    void draw_schedule();

    Localization *get_localization() { return &this->localization_; }

    void set_display(display::Display *display) { display_ = display; }
    void set_font(font::Font *font) { font_ = font; }
    void set_rtc(time::RealTimeClock *rtc) { rtc_ = rtc; }

    void set_base_url(const std::string &base_url) { base_url_ = base_url; }
    void set_feed_code(const std::string &feed_code) { feed_code_ = feed_code; }

    // One row per line: "t;routeId,stopId[,offsetSeconds]" or "b;stationId".
    // Changing the rows resubscribes when connected.
    void set_rows_from_text(const std::string &text);
    // One custom name per line: "rowId;name", where rowId is
    // "routeId,stopId" for a transit row or the station ID for a bike row.
    void set_row_names_from_text(const std::string &text);
    void add_row(const Row &row) { rows_.push_back(row); }
    void add_row_name(const std::string &row_id, const std::string &name) { row_names_[row_id] = name; }

    void set_limit(int limit) { limit_ = limit; }
    void set_display_departure_times(bool display_departure_times) { display_departure_times_ = display_departure_times; }
    void set_scroll_names(bool scroll_names) { scroll_names_ = scroll_names; }
    void set_header_text(const std::string &header_text) { header_text_ = header_text; }
    void set_unit_display(UnitDisplay unit_display) { this->localization_.set_unit_display(unit_display); }

    void add_abbreviation(const std::string &from, const std::string &to) { abbreviations_[from] = to; }
    void add_header(const std::string &name, const std::string &value) { extra_headers_.emplace_back(name, value); }
    void add_route_style(const std::string &route_id, const std::string &name, const Color &color) { route_styles_[route_id] = RouteStyle{name, color}; }
    void set_abbreviations_from_text(const std::string &text);
    void set_route_styles_from_text(const std::string &text);

    void set_default_route_color(const Color &color) { default_route_color_ = color; }
    void set_realtime_color(const Color &color);
    void set_docks_color(const Color &color) { docks_color_ = color; }
    void set_mechanical_color(const Color &color) { mechanical_color_ = color; }
    void set_electric_color(const Color &color) { electric_color_ = color; }

  protected:
    static constexpr int scroll_speed = 10; // pixels/second
    static constexpr int idle_time_left = 5000;
    static constexpr int idle_time_right = 1000;

    void draw_text_centered_(const char *text, Color color);
    void draw_icon_(const char *const *rows, int left_x, int bottom_y, Color color, Color accent_color);
    void draw_realtime_icon_(int bottom_right_x, int bottom_right_y, unsigned long uptime);
    void draw_scrolling_text_(const std::string &text, Color color, int clip_start, int clip_end, int y_offset,
                              unsigned long uptime, int scroll_cycle_duration);

    std::string abbreviate_(const std::string &text) const;
    const std::string *custom_name_(const std::string &row_id) const;
    bool has_transit_rows_() const;
    bool has_bike_rows_() const;

    // Each returns how many pixels its name is too wide for (<= 0 if it fits).
    // With no_draw, only measures.
    int draw_transit_row_(const Row &row, const Trip *trip, int y_offset, int font_height, unsigned long uptime,
                          uint rtc_now, bool no_draw, int scroll_cycle_duration);
    int draw_bike_row_(const Row &row, const Station *station, int y_offset, unsigned long uptime, bool no_draw,
                       int scroll_cycle_duration);
    int draw_row_(int index, int y_offset, int font_height, unsigned long uptime, uint rtc_now, bool no_draw,
                  int scroll_cycle_duration);
    const Trip *trip_for_row_(int index, uint rtc_now) const;

    Localization localization_{};
    TrackerState state_;

    display::Display *display_{nullptr};
    font::Font *font_{nullptr};
    time::RealTimeClock *rtc_{nullptr};

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
    std::vector<Row> rows_;
    std::string rows_text_;
    std::map<std::string, std::string> row_names_;
    int limit_ = 3;
    bool display_departure_times_ = true;
    bool scroll_names_ = true;

    std::string header_text_;
    std::map<std::string, std::string> abbreviations_;
    std::map<std::string, RouteStyle> route_styles_;

    Color default_route_color_ = Color(0x028e51);
    Color realtime_color_ = Color(0x20FF00);
    Color realtime_color_dark_ = Color(0x00A700);
    Color docks_color_ = Color(0x00A8FF);
    Color mechanical_color_ = Color(0xE2001A);
    Color electric_color_ = Color(0x2ECC40);
    Color bolt_color_ = Color(0xFFD200);
    Color count_color_ = Color(0xFFFFFF);
    Color unavailable_color_ = Color(0x404040);
};

}  // namespace transit_tracker
}  // namespace esphome
