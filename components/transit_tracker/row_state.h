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

class TrackerState {
  public:
    std::mutex mutex;
    // Upcoming departures of every transit row, soonest first
    std::vector<Trip> trips;
    std::map<std::string, Station> stations;
    bool has_schedule_update = false;
    bool has_stations_update = false;
};

}  // namespace transit_tracker
}  // namespace esphome
