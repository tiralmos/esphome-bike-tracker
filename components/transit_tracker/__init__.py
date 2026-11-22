import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.display import Display
from esphome.components.font import Font
from esphome.components.time import RealTimeClock
from esphome.components import color
from esphome.const import CONF_ID, CONF_DISPLAY_ID, CONF_NAME, CONF_TIME_ID, __version__ as ESPHOME_VERSION
from esphome.types import ConfigType
from esphome.components import esp32
from esphome.components.esp32 import (
    add_idf_component,
)

_MINIMUM_ESPHOME_VERSION = "2025.11.0"

DEPENDENCIES = ["network", "display", "font", "esp32", "time"]
AUTO_LOAD = ["json"]

transit_tracker_ns = cg.esphome_ns.namespace("transit_tracker")
TransitTracker = transit_tracker_ns.class_("TransitTracker", cg.Component)

UnitDisplay = transit_tracker_ns.enum("UnitDisplay")
UNIT_DISPLAY_VALUES = {
    "long": UnitDisplay.UNIT_DISPLAY_LONG,
    "short": UnitDisplay.UNIT_DISPLAY_SHORT,
    "none": UnitDisplay.UNIT_DISPLAY_NONE,
}

CONF_ROWS = "rows"
CONF_TIME_SLOTS = "time_slots"
CONF_FROM = "from"
CONF_TO = "to"
CONF_ROUTE_ID = "route_id"
CONF_STOP_ID = "stop_id"
CONF_STATION_ID = "station_id"
CONF_TIME_OFFSET = "time_offset"
CONF_BASE_URL = "base_url"
CONF_FONT_ID = "font_id"
CONF_LIMIT = "limit"
CONF_ABBREVIATIONS = "abbreviations"
CONF_FEED_CODE = "feed_code"
CONF_TIME_DISPLAY = "time_display"
CONF_SHOW_UNITS = "show_units"
CONF_SCROLL_NAMES = "scroll_names"
CONF_HEADERS = "headers"
CONF_HEADER_TEXT = "header_text"
CONF_STYLES = "styles"
CONF_DEFAULT_ROUTE_COLOR = "default_route_color"
CONF_REALTIME_COLOR = "realtime_color"
CONF_DOCKS_COLOR = "docks_color"
CONF_MECHANICAL_COLOR = "mechanical_color"
CONF_ELECTRIC_COLOR = "electric_color"


def validate_ws_url(value):
    url = cv.url(value)
    if not value.startswith("ws://") and not value.startswith("wss://"):
        raise cv.Invalid("URL must start with 'ws://' or 'wss://")

    return url


def validate_esphome_version(obj):
    if cv.Version.parse(ESPHOME_VERSION) < cv.Version.parse(_MINIMUM_ESPHOME_VERSION):
        raise cv.Invalid(
            "The transit_tracker component requires ESPHome version " +
            f"{_MINIMUM_ESPHOME_VERSION} or later."
        )
    return obj


def validate_row(value):
    """A row is either a transit line at a stop or a bike-share station."""
    if not isinstance(value, dict):
        raise cv.Invalid("Each row needs either route_id + stop_id, or station_id")

    if CONF_STATION_ID in value:
        if CONF_ROUTE_ID in value or CONF_STOP_ID in value:
            raise cv.Invalid("A row is either a transit line (route_id + stop_id) or a station (station_id), not both")
        return cv.Schema(
            {
                cv.Required(CONF_STATION_ID): cv.string,
                # Shown instead of the station's own name
                cv.Optional(CONF_NAME): cv.string,
            }
        )(value)

    return cv.Schema(
        {
            cv.Required(CONF_ROUTE_ID): cv.string,
            cv.Required(CONF_STOP_ID): cv.string,
            cv.Optional(CONF_TIME_OFFSET, default="0s"): cv.time_period,
            # Shown instead of the trip's destination
            cv.Optional(CONF_NAME): cv.string,
        }
    )(value)


def validate_clock_time(value):
    """A local time of day, "HH:MM" (24:00 allowed as the end of the day)."""
    value = cv.string(value).strip()
    parts = value.split(":")
    if len(parts) != 2 or not all(p.isdigit() and len(p) == 2 for p in parts):
        raise cv.Invalid(f"Expected a time like 07:30, got {value!r}")
    hours, minutes = int(parts[0]), int(parts[1])
    if hours > 24 or minutes > 59 or (hours == 24 and minutes != 0):
        raise cv.Invalid(f"Not a valid time of day: {value!r}")
    return (hours * 60 + minutes) % (24 * 60)


def _slot_minutes(slot):
    """The minutes of the day a slot covers."""
    start, end = slot[CONF_FROM], slot[CONF_TO]
    if start == end:
        return set(range(24 * 60))
    if start < end:
        return set(range(start, end))
    return set(range(start, 24 * 60)) | set(range(0, end))


def validate_time_slots(slots):
    for i, slot in enumerate(slots):
        for other in slots[i + 1:]:
            if _slot_minutes(slot) & _slot_minutes(other):
                raise cv.Invalid("Time slots must not overlap")
    return slots


TIME_SLOT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_FROM): validate_clock_time,
        cv.Required(CONF_TO): validate_clock_time,
        cv.Required(CONF_ROWS): cv.ensure_list(validate_row),
    }
)


def _consume_transit_tracker_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for transit_tracker component."""
    from esphome.components import socket
    socket.consume_sockets(1, "transit_tracker")(config)
    return config


COLOR_SCHEMA = cv.All(
    cv.requires_component("color"),
    cv.use_id(color.ColorStruct)
)


CONFIG_SCHEMA = cv.All(
    validate_esphome_version,
    cv.only_on_esp32,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TransitTracker),
            cv.GenerateID(CONF_DISPLAY_ID): cv.use_id(Display),
            cv.GenerateID(CONF_FONT_ID): cv.use_id(Font),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(RealTimeClock),
            cv.Optional(CONF_BASE_URL): validate_ws_url,
            cv.Optional(CONF_LIMIT, default=3): cv.positive_int,
            cv.Optional(CONF_FEED_CODE, default=""): cv.string,
            cv.Optional(CONF_TIME_DISPLAY, default="departure"): cv.one_of(
                "departure", "arrival"
            ),
            cv.Optional(CONF_SHOW_UNITS, default="long"): cv.enum(UNIT_DISPLAY_VALUES),
            cv.Optional(CONF_SCROLL_NAMES, default=True): cv.boolean,
            # Shown when no time slot is active
            cv.Optional(CONF_ROWS, default=[]): cv.ensure_list(validate_row),
            # Rows shown only between two times of the day
            cv.Optional(CONF_TIME_SLOTS, default=[]): cv.All(
                cv.ensure_list(TIME_SLOT_SCHEMA), validate_time_slots
            ),
            cv.Optional(CONF_HEADER_TEXT, default=""): cv.string,
            cv.Optional(CONF_DEFAULT_ROUTE_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_REALTIME_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_DOCKS_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_MECHANICAL_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_ELECTRIC_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_STYLES): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("route_id"): cv.string,
                        cv.Required("name"): cv.string,
                        cv.Required("color"): COLOR_SCHEMA,
                    }
                )
            ),
            cv.Optional(CONF_ABBREVIATIONS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("from"): cv.string,
                        cv.Required("to"): cv.string,
                    }
                )
            ),
            cv.Optional(CONF_HEADERS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("name"): cv.string,
                        cv.Required("value"): cv.string,
                    }
                )
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_transit_tracker_sockets,
)


def _row_id(row):
    if CONF_STATION_ID in row:
        return row[CONF_STATION_ID]
    return f"{row[CONF_ROUTE_ID]},{row[CONF_STOP_ID]}"


def _rows_text(rows, time_slots=()):
    """Rows in the same text format as the rows setting on the device."""
    lines = []
    for row in rows:
        lines.append(_row_line(row))
    for slot in time_slots:
        start, end = slot[CONF_FROM], slot[CONF_TO]
        lines.append(f"s;{start // 60:02d}{start % 60:02d}-{end // 60:02d}{end % 60:02d}")
        for row in slot[CONF_ROWS]:
            lines.append(_row_line(row))
    return "\n".join(lines)


def _row_line(row):
    if CONF_STATION_ID in row:
        return f"b;{row[CONF_STATION_ID]}"
    offset = row[CONF_TIME_OFFSET].total_seconds
    return f"t;{row[CONF_ROUTE_ID]},{row[CONF_STOP_ID]},{int(offset)}"


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])

    drawing_display = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(drawing_display))

    font = await cg.get_variable(config[CONF_FONT_ID])
    cg.add(var.set_font(font))

    time = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_rtc(time))

    if CONF_BASE_URL in config:
        cg.add(var.set_base_url(config[CONF_BASE_URL]))

    cg.add(var.set_feed_code(config[CONF_FEED_CODE]))

    if config[CONF_ROWS] or config[CONF_TIME_SLOTS]:
        cg.add(var.set_rows_from_text(_rows_text(config[CONF_ROWS], config[CONF_TIME_SLOTS])))

    all_rows = list(config[CONF_ROWS])
    for slot in config[CONF_TIME_SLOTS]:
        all_rows.extend(slot[CONF_ROWS])
    for row in all_rows:
        if CONF_NAME in row:
            cg.add(var.add_row_name(_row_id(row), row[CONF_NAME]))

    cg.add(var.set_display_departure_times(config[CONF_TIME_DISPLAY] == "departure"))
    cg.add(var.set_unit_display(config[CONF_SHOW_UNITS]))
    cg.add(var.set_scroll_names(config[CONF_SCROLL_NAMES]))
    cg.add(var.set_limit(config[CONF_LIMIT]))

    if CONF_HEADER_TEXT in config:
        cg.add(var.set_header_text(config[CONF_HEADER_TEXT]))

    if CONF_HEADERS in config:
        for header in config[CONF_HEADERS]:
            cg.add(var.add_header(header["name"], header["value"]))

    if CONF_ABBREVIATIONS in config:
        for abbreviation in config[CONF_ABBREVIATIONS]:
            cg.add(var.add_abbreviation(abbreviation["from"], abbreviation["to"]))

    if CONF_STYLES in config:
        for style in config[CONF_STYLES]:
            color_struct = await cg.get_variable(style["color"])
            cg.add(var.add_route_style(style["route_id"], style["name"], color_struct))

    if CONF_DEFAULT_ROUTE_COLOR in config:
        cg.add(var.set_default_route_color(await cg.get_variable(config[CONF_DEFAULT_ROUTE_COLOR])))

    if CONF_REALTIME_COLOR in config:
        cg.add(var.set_realtime_color(await cg.get_variable(config[CONF_REALTIME_COLOR])))

    if CONF_DOCKS_COLOR in config:
        cg.add(var.set_docks_color(await cg.get_variable(config[CONF_DOCKS_COLOR])))

    if CONF_MECHANICAL_COLOR in config:
        cg.add(var.set_mechanical_color(await cg.get_variable(config[CONF_MECHANICAL_COLOR])))

    if CONF_ELECTRIC_COLOR in config:
        cg.add(var.set_electric_color(await cg.get_variable(config[CONF_ELECTRIC_COLOR])))

    await cg.register_component(var, config)

    add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )

    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
