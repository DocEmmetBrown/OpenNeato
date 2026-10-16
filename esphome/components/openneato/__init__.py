"""OpenNeato hub for ESPHome.

Bridges a Neato Botvac (D3-D7) serial port to native ESPHome entities. The hub
creates every entity of the Home Assistant contract (docs/esphome-contract.md)
with its contract name; each one can optionally be customised with an override
block under its key (icon, disabled_by_default, ...). Renaming an entity breaks
the OpenNeato integration's lookup, so keep the default names.
"""

import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    button,
    datetime,
    event,
    number,
    select,
    sensor,
    switch,
    text_sensor,
    time as time_,
    uart,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISABLED_BY_DEFAULT,
    CONF_ID,
    CONF_NAME,
    CONF_TIME_ID,
    DEVICE_CLASS_AREA,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DISTANCE,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_PLUG,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RESTART,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_TIMESTAMP,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_MILLIAMP,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
)

CODEOWNERS = ["@Leicas"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = [
    "binary_sensor",
    "button",
    "datetime",
    "event",
    "json",
    "number",
    "select",
    "sensor",
    "switch",
    "text_sensor",
]

CONF_POSE_BUFFER_SIZE = "pose_buffer_size"
CONF_ROBOT_CLOCK = "robot_clock"
CONF_IDLE_POLL_INTERVAL = "idle_poll_interval"

UNIT_RPM = "rpm"
UNIT_DAYS = "d"
UNIT_SQUARE_METERS = "m²"
UNIT_METERS = "m"
UNIT_MILLIWATT = "mW"

openneato_ns = cg.esphome_ns.namespace("openneato")
OpenNeatoComponent = openneato_ns.class_(
    "OpenNeatoComponent", cg.Component, uart.UARTDevice
)
OpenNeatoSwitch = openneato_ns.class_("OpenNeatoSwitch", switch.Switch)
OpenNeatoNumber = openneato_ns.class_("OpenNeatoNumber", number.Number)
OpenNeatoSelect = openneato_ns.class_("OpenNeatoSelect", select.Select)
OpenNeatoButton = openneato_ns.class_("OpenNeatoButton", button.Button)
OpenNeatoTime = openneato_ns.class_("OpenNeatoTime", datetime.TimeEntity)

DAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]
SLOT_DEFAULT_TIMES = [(9, 0), (14, 0)]

NAVIGATION_MODES = ["Normal", "Gentle", "Deep", "Quick"]
# PlaySound SoundID 0-20 (keep in sync with neatocore::SOUND_NAMES).
SOUND_NAMES = [
    "Waking up",
    "Starting cleaning",
    "Cleaning completed",
    "Attention needed",
    "Backing up into base",
    "Docking completed",
    "Test sound 1",
    "Test sound 2",
    "Test sound 3",
    "Test sound 4",
    "Test sound 5",
    "Exploring",
    "Shutting down",
    "Picked up",
    "Going to sleep",
    "Returning home",
    "User canceled cleaning",
    "User terminated cleaning",
    "Slipped off base",
    "Alert",
    "Thank you",
]
EVENT_TYPES = [
    "cleaning_started",
    "cleaning_done",
    "docking_recharge",
    "returned_to_base",
    "error",
    "alert",
]

DIAG = ENTITY_CATEGORY_DIAGNOSTIC
CONFIG = ENTITY_CATEGORY_CONFIG


def _entity(schema, name, disabled=False, **defaults):
    """Validate an optional override block, defaulting the contract name."""

    def validator(value):
        if value is None:
            value = {}
        if not isinstance(value, dict):
            raise cv.Invalid("Expected an entity override mapping")
        value = dict(value)
        value.setdefault(CONF_NAME, name)
        if disabled:
            value.setdefault(CONF_DISABLED_BY_DEFAULT, True)
        for key, default in defaults.items():
            value.setdefault(key, default)
        return schema(value)

    return validator


# (key, enum, name, schema, disabled_by_default)
TEXT_SENSORS = [
    ("ui_state", "TEXT_UI_STATE", "UI state", {"icon": "mdi:robot-vacuum"}),
    ("robot_state", "TEXT_ROBOT_STATE", "Robot state", {"icon": "mdi:state-machine"}),
    ("activity", "TEXT_ACTIVITY", "Activity", {"icon": "mdi:robot-vacuum-variant"}),
    ("error_message", "TEXT_ERROR_MESSAGE", "Error message", {"icon": "mdi:alert-circle-outline"}),
    ("error_kind", "TEXT_ERROR_KIND", "Error kind", {"icon": "mdi:alert-outline"}),
    ("model", "TEXT_MODEL", "Model", {"icon": "mdi:robot-vacuum", "entity_category": DIAG}),
    ("serial_number", "TEXT_SERIAL_NUMBER", "Serial number", {"icon": "mdi:barcode", "entity_category": DIAG}),
    ("robot_firmware", "TEXT_ROBOT_FIRMWARE", "Robot firmware", {"icon": "mdi:chip", "entity_category": DIAG}),
    ("lds_firmware", "TEXT_LDS_FIRMWARE", "LDS firmware", {"icon": "mdi:radar", "entity_category": DIAG}),
    ("main_board", "TEXT_MAIN_BOARD", "Main board", {"icon": "mdi:expansion-card", "entity_category": DIAG}),
]

SENSORS = [
    ("battery", "SENSOR_BATTERY", "Battery",
     dict(unit_of_measurement=UNIT_PERCENT, device_class=DEVICE_CLASS_BATTERY,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=0), False),
    ("battery_voltage", "SENSOR_BATTERY_VOLTAGE", "Battery voltage",
     dict(unit_of_measurement=UNIT_VOLT, device_class=DEVICE_CLASS_VOLTAGE,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=2), False),
    ("battery_current", "SENSOR_BATTERY_CURRENT", "Battery current",
     dict(unit_of_measurement=UNIT_MILLIAMP, device_class=DEVICE_CLASS_CURRENT,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=0, entity_category=DIAG), False),
    ("battery_temperature", "SENSOR_BATTERY_TEMPERATURE", "Battery temperature",
     dict(unit_of_measurement=UNIT_CELSIUS, device_class=DEVICE_CLASS_TEMPERATURE,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=0, entity_category=DIAG), False),
    ("external_voltage", "SENSOR_EXTERNAL_VOLTAGE", "External voltage",
     dict(unit_of_measurement=UNIT_VOLT, device_class=DEVICE_CLASS_VOLTAGE,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=2, entity_category=DIAG), False),
    ("battery_cycles", "SENSOR_BATTERY_CYCLES", "Battery cycles",
     dict(icon="mdi:battery-sync", state_class=STATE_CLASS_TOTAL_INCREASING, accuracy_decimals=0,
          entity_category=DIAG), False),
    ("total_cleaning_time", "SENSOR_TOTAL_CLEANING_TIME", "Total cleaning time",
     dict(unit_of_measurement=UNIT_SECOND, device_class=DEVICE_CLASS_DURATION,
          state_class=STATE_CLASS_TOTAL_INCREASING, accuracy_decimals=0, entity_category=DIAG), False),
    ("error_code", "SENSOR_ERROR_CODE", "Error code",
     dict(icon="mdi:alert-circle-outline", accuracy_decimals=0), False),
    ("brush_rpm", "SENSOR_BRUSH_RPM", "Brush RPM",
     dict(unit_of_measurement=UNIT_RPM, icon="mdi:rotate-right", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0, entity_category=DIAG), True),
    ("vacuum_rpm", "SENSOR_VACUUM_RPM", "Vacuum RPM",
     dict(unit_of_measurement=UNIT_RPM, icon="mdi:fan", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0, entity_category=DIAG), True),
    ("left_wheel_load", "SENSOR_LEFT_WHEEL_LOAD", "Left wheel load",
     dict(unit_of_measurement=UNIT_PERCENT, icon="mdi:tire", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0, entity_category=DIAG), True),
    ("right_wheel_load", "SENSOR_RIGHT_WHEEL_LOAD", "Right wheel load",
     dict(unit_of_measurement=UNIT_PERCENT, icon="mdi:tire", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0, entity_category=DIAG), True),
    ("lds_rpm", "SENSOR_LDS_RPM", "LDS RPM",
     dict(unit_of_measurement=UNIT_RPM, icon="mdi:radar", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0, entity_category=DIAG), True),
    ("next_clean", "SENSOR_NEXT_CLEAN", "Next clean",
     dict(device_class=DEVICE_CLASS_TIMESTAMP, icon="mdi:calendar-clock"), False),
    ("clean_started", "SENSOR_CLEAN_STARTED", "Clean started",
     dict(device_class=DEVICE_CLASS_TIMESTAMP, icon="mdi:clock-start"), False),
    ("clean_duration", "SENSOR_CLEAN_DURATION", "Clean duration",
     dict(unit_of_measurement=UNIT_SECOND, device_class=DEVICE_CLASS_DURATION,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=0), False),
    ("clean_area", "SENSOR_CLEAN_AREA", "Clean area",
     dict(unit_of_measurement=UNIT_SQUARE_METERS, device_class=DEVICE_CLASS_AREA,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=2), False),
    ("clean_distance", "SENSOR_CLEAN_DISTANCE", "Clean distance",
     dict(unit_of_measurement=UNIT_METERS, device_class=DEVICE_CLASS_DISTANCE,
          state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=1), False),
    ("clean_recharges", "SENSOR_CLEAN_RECHARGES", "Clean recharges",
     dict(icon="mdi:battery-charging", accuracy_decimals=0), False),
    ("clean_battery_used", "SENSOR_CLEAN_BATTERY_USED", "Clean battery used",
     dict(unit_of_measurement=UNIT_PERCENT, icon="mdi:battery-minus", state_class=STATE_CLASS_MEASUREMENT,
          accuracy_decimals=0), False),
]

BINARY_SENSORS = [
    ("charging", "BINARY_CHARGING", "Charging", dict(device_class=DEVICE_CLASS_BATTERY_CHARGING)),
    ("external_power", "BINARY_EXTERNAL_POWER", "External power", dict(device_class=DEVICE_CLASS_PLUG)),
    ("problem", "BINARY_PROBLEM", "Problem", dict(device_class=DEVICE_CLASS_PROBLEM)),
    ("dustbin", "BINARY_DUSTBIN", "Dustbin", dict(icon="mdi:delete-variant")),
    ("wheel_lifted", "BINARY_WHEEL_LIFTED", "Wheel lifted", dict(icon="mdi:arrow-up-bold-box-outline")),
    ("front_left_bumper", "BINARY_FRONT_LEFT_BUMPER", "Front left bumper",
     dict(icon="mdi:car-brake-alert", entity_category=DIAG)),
    ("front_right_bumper", "BINARY_FRONT_RIGHT_BUMPER", "Front right bumper",
     dict(icon="mdi:car-brake-alert", entity_category=DIAG)),
    ("side_left_bumper", "BINARY_SIDE_LEFT_BUMPER", "Side left bumper",
     dict(icon="mdi:car-brake-alert", entity_category=DIAG)),
    ("side_right_bumper", "BINARY_SIDE_RIGHT_BUMPER", "Side right bumper",
     dict(icon="mdi:car-brake-alert", entity_category=DIAG)),
    ("battery_overheating", "BINARY_BATTERY_OVERHEATING", "Battery overheating",
     dict(device_class=DEVICE_CLASS_HEAT, entity_category=DIAG)),
    ("battery_failure", "BINARY_BATTERY_FAILURE", "Battery failure",
     dict(device_class=DEVICE_CLASS_PROBLEM, entity_category=DIAG)),
    ("robot_connected", "BINARY_ROBOT_CONNECTED", "Robot connected",
     dict(device_class=DEVICE_CLASS_CONNECTIVITY, entity_category=DIAG)),
    ("stall_front", "BINARY_STALL_FRONT", "Stall front", dict(icon="mdi:arrow-up-bold-octagon")),
    ("stall_rear", "BINARY_STALL_REAR", "Stall rear", dict(icon="mdi:arrow-down-bold-octagon")),
]

# (key, enum, name, icon, entity_category, restore_mode)
SWITCHES = [
    ("eco_mode", "SWITCH_ECO_MODE", "Eco mode", "mdi:leaf", CONFIG, "DISABLED"),
    ("intense_clean", "SWITCH_INTENSE_CLEAN", "Intense clean", "mdi:broom", CONFIG, "DISABLED"),
    ("bin_full_detection", "SWITCH_BIN_FULL_DETECTION", "Bin full detection", "mdi:delete-alert", CONFIG,
     "DISABLED"),
    ("wall_following", "SWITCH_WALL_FOLLOWING", "Wall following", "mdi:wall", CONFIG, "DISABLED"),
    ("button_click_sound", "SWITCH_BUTTON_CLICK_SOUND", "Button click sound", "mdi:gesture-tap-button", CONFIG,
     "DISABLED"),
    ("melodies", "SWITCH_MELODIES", "Melodies", "mdi:music", CONFIG, "DISABLED"),
    ("warning_sounds", "SWITCH_WARNING_SOUNDS", "Warning sounds", "mdi:volume-high", CONFIG, "DISABLED"),
    ("stealth_led", "SWITCH_STEALTH_LED", "Stealth LED", "mdi:led-off", CONFIG, "DISABLED"),
    ("schedule", "SWITCH_SCHEDULE", "Schedule", "mdi:calendar-check", CONFIG, "RESTORE_DEFAULT_OFF"),
    ("skip_next_clean", "SWITCH_SKIP_NEXT_CLEAN", "Skip next clean", "mdi:calendar-remove", CONFIG,
     "RESTORE_DEFAULT_OFF"),
    ("restart_before_clean", "SWITCH_RESTART_BEFORE_CLEAN", "Restart before clean", "mdi:restart", CONFIG,
     "RESTORE_DEFAULT_OFF"),
    ("manual_mode", "SWITCH_MANUAL_MODE", "Manual mode", "mdi:gamepad-variant", None, "DISABLED"),
    ("manual_brush", "SWITCH_MANUAL_BRUSH", "Manual brush", "mdi:broom", None, "DISABLED"),
    ("manual_vacuum", "SWITCH_MANUAL_VACUUM", "Manual vacuum", "mdi:fan", None, "DISABLED"),
    ("manual_side_brush", "SWITCH_MANUAL_SIDE_BRUSH", "Manual side brush", "mdi:pinwheel", None, "DISABLED"),
]

# (key, enum, name, unit, icon, min, max, step, restored initial value or None)
NUMBERS = [
    ("filter_change_interval", "NUMBER_FILTER_CHANGE_INTERVAL", "Filter change interval", UNIT_DAYS,
     "mdi:air-filter", 1, 365, 1, None),
    ("brush_change_interval", "NUMBER_BRUSH_CHANGE_INTERVAL", "Brush change interval", UNIT_DAYS,
     "mdi:broom", 1, 365, 1, None),
    ("dirt_bin_reminder", "NUMBER_DIRT_BIN_REMINDER", "Dirt bin reminder", UNIT_MINUTE,
     "mdi:delete-clock", 1, 600, 1, None),
    ("stall_threshold", "NUMBER_STALL_THRESHOLD", "Stall threshold", UNIT_PERCENT,
     "mdi:car-brake-alert", 30, 80, 1, 80),
    ("manual_brush_speed", "NUMBER_MANUAL_BRUSH_SPEED", "Manual brush speed", UNIT_RPM,
     "mdi:rotate-right", 500, 1600, 10, 1200),
    ("manual_vacuum_speed", "NUMBER_MANUAL_VACUUM_SPEED", "Manual vacuum speed", UNIT_PERCENT,
     "mdi:fan", 40, 100, 1, 80),
    ("manual_side_brush_power", "NUMBER_MANUAL_SIDE_BRUSH_POWER", "Manual side brush power", UNIT_MILLIWATT,
     "mdi:pinwheel", 500, 1500, 50, 1500),
]

# (key, enum, name, icon, entity_category, device_class)
BUTTONS = [
    ("start_house_clean", "BUTTON_START_HOUSE_CLEAN", "Start house clean", "mdi:play", None, None),
    ("start_spot_clean", "BUTTON_START_SPOT_CLEAN", "Start spot clean", "mdi:target", None, None),
    ("pause", "BUTTON_PAUSE", "Pause", "mdi:pause", None, None),
    ("stop", "BUTTON_STOP", "Stop", "mdi:stop", None, None),
    ("return_to_base", "BUTTON_RETURN_TO_BASE", "Return to base", "mdi:home-import-outline", None, None),
    ("locate", "BUTTON_LOCATE", "Locate", "mdi:map-marker-question", None, None),
    ("play_sound", "BUTTON_PLAY_SOUND", "Play sound", "mdi:volume-high", None, None),
    ("clear_errors", "BUTTON_CLEAR_ERRORS", "Clear errors", "mdi:alert-remove", None, None),
    ("restart_robot", "BUTTON_RESTART_ROBOT", "Restart robot", None, CONFIG, DEVICE_CLASS_RESTART),
    ("shut_down_robot", "BUTTON_SHUT_DOWN_ROBOT", "Shut down robot", "mdi:power", CONFIG, None),
    ("new_battery", "BUTTON_NEW_BATTERY", "New battery", "mdi:battery-plus", CONFIG, None),
    ("sync_robot_clock", "BUTTON_SYNC_ROBOT_CLOCK", "Sync robot clock", "mdi:clock-check", CONFIG, None),
    ("start_mapping_run", "BUTTON_START_MAPPING_RUN", "Start mapping run", "mdi:map-search", None, None),
]


def _slot_keys():
    for day_index, day in enumerate(DAYS):
        for slot in range(2):
            yield day_index * 2 + slot, day, slot


def _build_schema():
    schema = {
        cv.GenerateID(): cv.declare_id(OpenNeatoComponent),
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_POSE_BUFFER_SIZE, default=256): cv.int_range(min=256, max=4096),
        cv.Optional(CONF_ROBOT_CLOCK, default="utc"): cv.one_of("utc", "local", lower=True),
        cv.Optional(CONF_IDLE_POLL_INTERVAL, default="10s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=2), max=cv.TimePeriod(seconds=60)),
        ),
    }
    for key, _, name, kwargs in TEXT_SENSORS:
        schema[cv.Optional(key, default={})] = _entity(text_sensor.text_sensor_schema(**kwargs), name)
    for key, _, name, kwargs, disabled in SENSORS:
        schema[cv.Optional(key, default={})] = _entity(sensor.sensor_schema(**kwargs), name, disabled)
    for key, _, name, kwargs in BINARY_SENSORS:
        schema[cv.Optional(key, default={})] = _entity(binary_sensor.binary_sensor_schema(**kwargs), name)
    for key, _, name, icon, category, restore in SWITCHES:
        kwargs = {"icon": icon, "default_restore_mode": restore}
        if category:
            kwargs["entity_category"] = category
        schema[cv.Optional(key, default={})] = _entity(
            switch.switch_schema(OpenNeatoSwitch, block_inverted=True, **kwargs), name
        )
    for _, day, slot in _slot_keys():
        key = f"schedule_{day}_{slot + 1}"
        label = f"Schedule {day.capitalize()} {slot + 1}"
        schema[cv.Optional(key, default={})] = _entity(
            switch.switch_schema(
                OpenNeatoSwitch,
                block_inverted=True,
                icon="mdi:calendar-clock",
                entity_category=CONFIG,
                default_restore_mode="RESTORE_DEFAULT_OFF",
            ),
            label,
        )
        schema[cv.Optional(f"{key}_time", default={})] = _entity(
            datetime.time_schema(OpenNeatoTime),
            f"{label} time",
            icon="mdi:clock-outline",
            entity_category=CONFIG,
        )
    for key, _, name, unit, icon, *_rest in NUMBERS:
        schema[cv.Optional(key, default={})] = _entity(
            number.number_schema(OpenNeatoNumber, icon=icon, entity_category=CONFIG, unit_of_measurement=unit),
            name,
        )
    schema[cv.Optional("navigation_mode", default={})] = _entity(
        select.select_schema(OpenNeatoSelect, icon="mdi:map-marker-path", entity_category=CONFIG),
        "Navigation mode",
    )
    schema[cv.Optional("sound", default={})] = _entity(
        select.select_schema(OpenNeatoSelect, icon="mdi:music-note"), "Sound"
    )
    for key, _, name, icon, category, device_class in BUTTONS:
        kwargs = {}
        if icon:
            kwargs["icon"] = icon
        if category:
            kwargs["entity_category"] = category
        if device_class:
            kwargs["device_class"] = device_class
        schema[cv.Optional(key, default={})] = _entity(button.button_schema(OpenNeatoButton, **kwargs), name)
    schema[cv.Optional("robot_event", default={})] = _entity(
        event.event_schema(icon="mdi:robot-vacuum-alert"), "Robot event"
    )
    return cv.Schema(schema).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


CONFIG_SCHEMA = _build_schema()

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "openneato", baud_rate=115200, require_tx=True, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_TIME_ID in config:
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))
    cg.add(var.set_pose_buffer_size(config[CONF_POSE_BUFFER_SIZE]))
    cg.add(var.set_robot_clock_utc(config[CONF_ROBOT_CLOCK] == "utc"))
    cg.add(var.set_idle_poll_interval(config[CONF_IDLE_POLL_INTERVAL]))

    for key, enum, _, _ in TEXT_SENSORS:
        ent = await text_sensor.new_text_sensor(config[key])
        cg.add(var.set_text_sensor(getattr(openneato_ns, enum), ent))
    for key, enum, _, _, _ in SENSORS:
        ent = await sensor.new_sensor(config[key])
        cg.add(var.set_sensor(getattr(openneato_ns, enum), ent))
    for key, enum, _, _ in BINARY_SENSORS:
        ent = await binary_sensor.new_binary_sensor(config[key])
        cg.add(var.set_binary_sensor(getattr(openneato_ns, enum), ent))

    async def add_switch(conf, index):
        ent = await switch.new_switch(conf)
        await cg.register_parented(ent, var)
        cg.add(ent.set_index(index))
        cg.add(var.set_switch(index, ent))

    for key, enum, *_rest in SWITCHES:
        await add_switch(config[key], getattr(openneato_ns, enum))
    for slot_index, day, slot in _slot_keys():
        key = f"schedule_{day}_{slot + 1}"
        await add_switch(config[key], cg.RawExpression(f"esphome::openneato::SWITCH_SLOT_FIRST + {slot_index}"))
        ent = await datetime.new_datetime(config[f"{key}_time"])
        await cg.register_parented(ent, var)
        hour, minute = SLOT_DEFAULT_TIMES[slot]
        cg.add(ent.set_slot(slot_index))
        cg.add(ent.set_initial(hour, minute))
        cg.add(var.set_slot_time(slot_index, ent))

    for key, enum, _, _, _, min_value, max_value, step, initial in NUMBERS:
        ent = await number.new_number(config[key], min_value=min_value, max_value=max_value, step=step)
        await cg.register_parented(ent, var)
        cg.add(ent.set_index(getattr(openneato_ns, enum)))
        if initial is not None:
            cg.add(ent.set_restore(True, initial))
        cg.add(var.set_number(getattr(openneato_ns, enum), ent))

    for key, enum, options, restore in (
        ("navigation_mode", "SELECT_NAVIGATION_MODE", NAVIGATION_MODES, True),
        ("sound", "SELECT_SOUND", SOUND_NAMES, False),
    ):
        ent = await select.new_select(config[key], options=options)
        await cg.register_parented(ent, var)
        cg.add(ent.set_index(getattr(openneato_ns, enum)))
        cg.add(ent.set_restore(restore))
        cg.add(var.set_select(getattr(openneato_ns, enum), ent))

    for key, enum, *_rest in BUTTONS:
        ent = await button.new_button(config[key])
        await cg.register_parented(ent, var)
        cg.add(ent.set_index(getattr(openneato_ns, enum)))
        cg.add(var.set_button(getattr(openneato_ns, enum), ent))

    ent = await event.new_event(config["robot_event"], event_types=EVENT_TYPES)
    cg.add(var.set_event(ent))
