# ESPHome Transit Tracker Component (Lyon)

An external component for [ESPHome](https://esphome.io/) that shows Lyon's public transport on an LED matrix. Each row is either:

- **a transit line at a stop**: the line's badge, its destination (or your own name for the row), the time to the next departure, and an animated icon when the time is real-time. Add the same line at the same stop twice to see the next two departures.
- **a bike-share station**: its name, then the number of free docks, mechanical bikes and electric bikes, each with a small pixel-art icon.

```
C3  Gare St-Paul          [rt] 4min
C3  Gare St-Paul              12min
Bancel  [P] 5  [bike] 0  [e-bike] 11
```

It merges [esphome-transit-tracker](https://github.com/tjhorner/esphome-transit-tracker) with the Vélo'v bike tracker fork. It gets its data from [Transit Tracker Lyon API](https://github.com/tiralmos/transit-tracker-lyon-api), which serves TCL departures (GTFS) and Vélo'v stations (GBFS) over one WebSocket. It is optimized for the 128x32 display of the [Transit Tracker](https://transit-tracker.eastsideurbanism.org/) hardware.

## Usage

The complete firmware, with Wi-Fi setup and configuration from the [configurator](https://github.com/tiralmos/transit-tracker-lyon-configurator), lives in [tiralmos/transit-tracker-lyon-firmware](https://github.com/tiralmos/transit-tracker-lyon-firmware). To use the component in your own ESPHome configuration, import it with `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/tiralmos/esphome-transit-tracker-lyon
      ref: main
    components: [transit_tracker]
```

You will need a [display](https://esphome.io/components/display/), a [font](https://esphome.io/components/font/) and a [time source](https://esphome.io/components/time/) (for departure times). The configurator's "Generate YAML" button writes the `transit_tracker` block for you. All options:

```yaml
transit_tracker:
  id: tracker

  # WebSocket URL of your Transit Tracker Lyon API
  base_url: "ws://192.168.1.10:3001/"

  # Additional headers to send in the HTTP request
  headers:
    - name: "X-Hello"
      value: "world"

  # Maximum number of rows on the display (header text takes one)
  limit: 3

  # Show departure or arrival times
  time_display: departure

  # Units after the times: long ("5min"), short ("5m") or none ("5")
  show_units: long

  # Scroll names that don't fit
  scroll_names: true

  # Rows to show, top to bottom
  rows:
    # A transit line at a stop
    - route_id: "tcl:C3"
      stop_id: "tcl:1234"
      # Optional: added to the times shown
      time_offset: 1min
      # Optional: shown instead of the destination
      name: "Gare St-Paul"
    # A bike-share station
    - station_id: "velov:7024"
      # Optional: shown instead of the station's own name
      name: "Home"

  # Add a static string to the top of the display
  header_text: "Lyon"

  # Shorten destinations and station names
  abbreviations:
    - from: "Part-Dieu"
      to: "P-Dieu"

  # Override a line's badge text and color
  styles:
    - route_id: "tcl:C3"
      name: "C3"
      color: my_red

  # Colors (see https://esphome.io/components/display/#color)
  default_route_color: my_green
  realtime_color: my_green
  docks_color: my_blue
  mechanical_color: my_red
  electric_color: my_green
```

Then, in your display's draw lambda:

```yaml
display:
  - platform: # ...
    lambda: |-
      id(tracker).draw_schedule();
```

### Changing rows at runtime

`set_rows_from_text()` takes one row per line, `t;routeId,stopId[,offsetSeconds]` or `b;stationId`, and reconnects to subscribe to the new rows. `set_row_names_from_text()` takes `rowId;name` lines, where `rowId` is `routeId,stopId` or the station ID. The firmware stores both as device settings that the configurator writes.

## License

```
MIT License

Copyright (c) 2025 TJ Horner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```