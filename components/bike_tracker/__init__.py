import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.display import Display
from esphome.components.font import Font
from esphome.components import color
from esphome.const import CONF_ID, CONF_DISPLAY_ID, CONF_NAME, __version__ as ESPHOME_VERSION
from esphome.types import ConfigType
from esphome.components import esp32
from esphome.components.esp32 import (
    add_idf_component,
)

_MINIMUM_ESPHOME_VERSION = "2025.11.0"

DEPENDENCIES = ["network", "display", "font", "esp32"]
AUTO_LOAD = ["json"]

bike_tracker_ns = cg.esphome_ns.namespace("bike_tracker")
BikeTracker = bike_tracker_ns.class_("BikeTracker", cg.Component)

CONF_STATIONS = "stations"
CONF_STATION_ID = "station_id"
CONF_BASE_URL = "base_url"
CONF_FONT_ID = "font_id"
CONF_LIMIT = "limit"
CONF_ABBREVIATIONS = "abbreviations"
CONF_FEED_CODE = "feed_code"
CONF_SCROLL_STATION_NAMES = "scroll_station_names"
CONF_HEADERS = "headers"
CONF_HEADER_TEXT = "header_text"
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
            "The bike_tracker component requires ESPHome version " +
            f"{_MINIMUM_ESPHOME_VERSION} or later."
        )
    return obj


def _consume_bike_tracker_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for bike_tracker component."""
    from esphome.components import socket
    socket.consume_sockets(1, "bike_tracker")(config)
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
            cv.GenerateID(): cv.declare_id(BikeTracker),
            cv.GenerateID(CONF_DISPLAY_ID): cv.use_id(Display),
            cv.GenerateID(CONF_FONT_ID): cv.use_id(Font),
            cv.Optional(CONF_BASE_URL): validate_ws_url,
            cv.Optional(CONF_LIMIT, default=3): cv.positive_int,
            cv.Optional(CONF_FEED_CODE, default=""): cv.string,
            cv.Optional(CONF_SCROLL_STATION_NAMES, default=True): cv.boolean,
            cv.Optional(CONF_STATIONS, default=[]): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required(CONF_STATION_ID): cv.string,
                        # Shown instead of the station's own name
                        cv.Optional(CONF_NAME): cv.string,
                    }
                )
            ),
            cv.Optional(CONF_HEADER_TEXT, default=""): cv.string,
            cv.Optional(CONF_DOCKS_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_MECHANICAL_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_ELECTRIC_COLOR): COLOR_SCHEMA,
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
    _consume_bike_tracker_sockets,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])

    drawing_display = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(drawing_display))

    font = await cg.get_variable(config[CONF_FONT_ID])
    cg.add(var.set_font(font))

    if CONF_BASE_URL in config:
        cg.add(var.set_base_url(config[CONF_BASE_URL]))

    cg.add(var.set_feed_code(config[CONF_FEED_CODE]))
    cg.add(var.set_stations_string(
        ";".join(station[CONF_STATION_ID] for station in config[CONF_STATIONS])
    ))

    for station in config[CONF_STATIONS]:
        if CONF_NAME in station:
            cg.add(var.add_station_name(station[CONF_STATION_ID], station[CONF_NAME]))

    cg.add(var.set_scroll_station_names(config[CONF_SCROLL_STATION_NAMES]))

    cg.add(var.set_limit(config[CONF_LIMIT]))

    if CONF_HEADER_TEXT in config:
        cg.add(var.set_header_text(config[CONF_HEADER_TEXT]))

    if CONF_HEADERS in config:
        for header in config[CONF_HEADERS]:
            cg.add(var.add_header(header["name"], header["value"]))

    if CONF_ABBREVIATIONS in config:
        for abbreviation in config[CONF_ABBREVIATIONS]:
            cg.add(var.add_abbreviation(abbreviation["from"], abbreviation["to"]))

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
