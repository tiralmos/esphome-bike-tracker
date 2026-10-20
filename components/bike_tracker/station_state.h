#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace esphome {
namespace bike_tracker {

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

class StationState {
  public:
    std::mutex mutex;
    std::vector<Station> stations;
    bool has_received_update = false;
};

} // namespace bike_tracker
} // namespace esphome
