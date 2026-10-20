# ESPHome Bike Tracker Component

An external component for [ESPHome](https://esphome.io/) that shows live bike-share availability on an LED matrix: for each station, its name, then the number of free docks, mechanical bikes and electric bikes, each with a small pixel-art icon.

```
ROUVILLE   [P] 5   [bike] 0   [e-bike] 11
```

It is a fork of [esphome-transit-tracker](https://github.com/tjhorner/esphome-transit-tracker), adapted for the Lyon Vélo'v system, and gets its data from [Bike Tracker API](https://github.com/tiralmos/bike-tracker-api), which reads any GBFS bike-share feed. It is optimized for the 128x32 display of the [Transit Tracker](https://transit-tracker.eastsideurbanism.org/) hardware.

## Usage

The complete firmware, with Wi-Fi setup and configuration from the [configurator](https://github.com/tiralmos/bike-tracker-configurator), lives in [tiralmos/bike-tracker](https://github.com/tiralmos/bike-tracker). To use the component in your own ESPHome configuration, import it with `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/tiralmos/esphome-bike-tracker
      ref: main
    components: [bike_tracker]
```

You will need a [display](https://esphome.io/components/display/) and a [font](https://esphome.io/components/font/). The configurator's "Generate YAML" button writes the `bike_tracker` block for you. All options:

```yaml
bike_tracker:
  id: tracker

  # WebSocket URL of your Bike Tracker API
  base_url: "ws://192.168.1.10:3000/"

  # Additional headers to send in the HTTP request
  headers:
    - name: "X-Hello"
      value: "world"

  # Maximum number of rows on the display (header text takes one)
  limit: 3

  # Scroll station names that don't fit next to the counts
  scroll_station_names: true

  # Stations to show, top to bottom
  stations:
    - station_id: "velov:1024"
    - station_id: "velov:7024"
      # Shown instead of the station's own name
      name: "Home"

  # Add a static string to the top of the display
  header_text: "Velo'v"

  # Shorten station names
  abbreviations:
    - from: "Part-Dieu"
      to: "P-Dieu"

  # Icon colors (see https://esphome.io/components/display/#color)
  docks_color: my_green
  mechanical_color: my_red
  electric_color: my_blue
```

Then, in your display's draw lambda:

```yaml
display:
  - platform: # ...
    lambda: |-
      id(tracker).draw_stations();
```

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