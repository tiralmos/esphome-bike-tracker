#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "esphome/components/display/display.h"

namespace esphome {
namespace transit_tracker {

enum class RowType : uint8_t {
  // A transit line at a stop: shows its next departure
  TRANSIT,
  // A bike-share station: shows free docks, bikes and e-bikes
  BIKE,
};

class Row {
  public:
    RowType type;
    // TRANSIT: "routeId,stopId" (the time offset is kept separately)
    // BIKE: the station ID
    std::string id;
    std::string route_id;
    std::string stop_id;
    int time_offset = 0;
};

// A set of rows shown during part of the day. The default slot (no times)
// is shown whenever no other slot is active.
class TimeSlot {
  public:
    bool is_default = true;
    // Minutes since local midnight. A slot whose end is before its start
    // crosses midnight; start == end means the whole day.
    int start_minute = 0;
    int end_minute = 0;
    std::vector<Row> rows;

    bool contains(int minute) const {
      if (this->is_default) {
        return false;
      }
      if (this->start_minute == this->end_minute) {
        return true;
      }
      if (this->start_minute < this->end_minute) {
        return minute >= this->start_minute && minute < this->end_minute;
      }
      return minute >= this->start_minute || minute < this->end_minute;
    }
};

class Trip {
  public:
    std::string route_id;
    std::string stop_id;
    std::string route_name;
    Color route_color;
    std::string headsign;
    time_t arrival_time;
    time_t departure_time;
    bool is_realtime;
};

class Station {
  public:
    std::string station_id;
    std::string name;
    int docks_available;
    int mechanical_available;
    int electric_available;
    bool is_renting;
    bool is_returning;
};

// How bad a disruption is; it picks the color of the alert
enum class AlertLevel : uint8_t {
  SEVERE,   // the line does not run (red)
  WARNING,  // the line runs, but not as usual (yellow)
  INFO,     // works announced, other information (light blue)
};

class Alert {
  public:
    std::vector<std::string> route_ids;
    AlertLevel level;
    // One short line: the alert's title and first sentence
    std::string text;
};

class TrackerState {
  public:
    std::mutex mutex;
    // Upcoming departures of every transit row, soonest first
    std::vector<Trip> trips;
    std::map<std::string, Station> stations;
    bool has_schedule_update = false;
    bool has_stations_update = false;

    // Active alerts for the transit rows' lines, most severe first
    std::vector<Alert> alerts;

    bool has_weather = false;
    int temperature = 0;
    // "clear", "partly", "cloudy", "fog", "rain", "snow" or "storm"
    std::string weather_kind;
    bool is_day = true;
};

}  // namespace transit_tracker
}  // namespace esphome
