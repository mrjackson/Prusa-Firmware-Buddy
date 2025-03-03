/**
 * @file filament_sensors_handler_toolchanger.cpp
 * @brief this file contains code for filament sensor api with multi tool support
 */

#include "filament_sensors_handler.hpp"
#include "filament_sensor_adc.hpp"
#include "filters/median_filter.hpp"
#include "eeprom.h"
#include "marlin_client.hpp"
#include "freertos_mutex.hpp"
#include <mutex>
#include <puppies/Dwarf.hpp>
#include "src/module/prusa/toolchanger.h"

bool FilamentSensors::has_mmu2_enabled() {
    return false;
}

// Store request_side off
void FilamentSensors::DisableSideSensor() {
    const std::lock_guard lock(GetSideMutex());
    request_side = filament_sensor::cmd_t::off;
}

/**
 * @brief store request to enable side sensor
 * there is chance request could not be done
 * there is currently no callback to notify success/failure
 * we could just wait, since request is handled in different thread .. TODO
 * @return filament_sensor::mmu_enable_result_t
 */
filament_sensor::mmu_enable_result_t FilamentSensors::EnableSide() {
    if ((!physical_sensors.current_extruder) || (!IsWorking(physical_sensors.current_extruder->Get()))) { // physical_sensors.current_extruder is not synchronized, but in this case it it OK
        return filament_sensor::mmu_enable_result_t::error_filament_sensor_disabled;
    }
    const std::lock_guard lock(GetSideMutex());
    request_side = filament_sensor::cmd_t::on;
    return filament_sensor::mmu_enable_result_t::ok;
}

// process side sensor request
// currently side sensor of XL is controlled by primary sensors commands
// we might want to change it
// see process_printer_request();
void FilamentSensors::process_side_request() {
    switch (request_side) {
    case filament_sensor::cmd_t::on:
        request_side = filament_sensor::cmd_t::processing;
        break;
    case filament_sensor::cmd_t::off:
        request_side = filament_sensor::cmd_t::processing;
        break;
    case filament_sensor::cmd_t::processing:
    case filament_sensor::cmd_t::null:
        break;
    }
}

// Meyer's singleton
FSensorAdcExtruder *getExtruderFSensor(uint8_t index) {
    static std::array<FSensorAdcExtruder, EXTRUDERS> printer_sensors = { {
        { EEVAR_FS_VALUE_SPAN_0, EEVAR_FS_REF_VALUE_0, 0 },
        { EEVAR_FS_VALUE_SPAN_1, EEVAR_FS_REF_VALUE_1, 1 },
        { EEVAR_FS_VALUE_SPAN_2, EEVAR_FS_REF_VALUE_2, 2 },
        { EEVAR_FS_VALUE_SPAN_3, EEVAR_FS_REF_VALUE_3, 3 },
        { EEVAR_FS_VALUE_SPAN_4, EEVAR_FS_REF_VALUE_4, 4 },
        { EEVAR_FS_VALUE_SPAN_5, EEVAR_FS_REF_VALUE_5, 5 },
    } };

    return (index < 5 && prusa_toolchanger.is_tool_enabled(index)) ? &printer_sensors[index] : nullptr; // 6th sensor is not calibrated and causing errors
}

// Meyer's singleton
FSensorAdcSide *getSideFSensor(uint8_t index) {
    static std::array<FSensorAdcSide, EXTRUDERS> side_sensors = { {
        { EEVAR_SIDE_FS_VALUE_SPAN_0, EEVAR_SIDE_FS_REF_VALUE_0, 0 },
        { EEVAR_SIDE_FS_VALUE_SPAN_1, EEVAR_SIDE_FS_REF_VALUE_1, 1 },
        { EEVAR_SIDE_FS_VALUE_SPAN_2, EEVAR_SIDE_FS_REF_VALUE_2, 2 },
        { EEVAR_SIDE_FS_VALUE_SPAN_3, EEVAR_SIDE_FS_REF_VALUE_3, 3 },
        { EEVAR_SIDE_FS_VALUE_SPAN_4, EEVAR_SIDE_FS_REF_VALUE_4, 4 },
        { EEVAR_SIDE_FS_VALUE_SPAN_5, EEVAR_SIDE_FS_REF_VALUE_5, 5 },
    } };
    return (index < 5 && prusa_toolchanger.is_tool_enabled(index)) ? &side_sensors[index] : nullptr; // 6th sensor is not calibrated and causing errors
}

// function returning abstract sensor - used in higher level api
IFSensor *GetExtruderFSensor(uint8_t index) {
    return getExtruderFSensor(index);
}

// function returning abstract sensor - used in higher level api
IFSensor *GetSideFSensor(uint8_t index) {
    return getSideFSensor(index);
}

void FilamentSensors::SetToolIndex() {
    tool_index = prusa_toolchanger.get_active_tool_nr();
}

void FilamentSensors::configure_sensors() {
    logical_sensors.primary_runout = GetSideFSensor(tool_index);
    logical_sensors.secondary_runout = GetExtruderFSensor(tool_index);
    logical_sensors.autoload = GetExtruderFSensor(tool_index);

    physical_sensors.current_extruder = GetExtruderFSensor(tool_index);
    physical_sensors.current_side = GetSideFSensor(tool_index);
}

void FilamentSensors::reconfigure_sensors_if_needed() {
    uint8_t current_tool = prusa_toolchanger.get_active_tool_nr();

    if (current_tool != tool_index) {
        tool_index = current_tool; // must be done before configure_sensors!!!
        // configure_sensors uses it
        configure_sensors();
    }
}

void FilamentSensors::AdcExtruder_FilteredIRQ(int32_t val, uint8_t tool_index) {
    FSensorADC *sensor = getExtruderFSensor(tool_index);
    if (sensor) {
        sensor->set_filtered_value_from_IRQ(val);
    } else {
        assert("wrong extruder index");
    }
}

void FilamentSensors::AdcSide_FilteredIRQ(int32_t val, uint8_t tool_index) {
    FSensorADC *sensor = getSideFSensor(tool_index);
    if (sensor) {
        sensor->set_filtered_value_from_IRQ(val);
    } else {
        assert("wrong extruder index");
    }
}

// IRQ - called from interruption
void fs_process_sample(int32_t fs_raw_value, uint8_t tool_index) {
    // does not need to be filtered (data from tool are already filtered)
    FSensors_instance().AdcExtruder_FilteredIRQ(fs_raw_value, tool_index);
}

void side_fs_process_sample(int32_t fs_raw_value, uint8_t tool_index) {
    static MedianFilter filter[HOTENDS];
    assert(tool_index < HOTENDS);

    FSensorADC *sensor = getSideFSensor(tool_index);
    if (sensor) {
        sensor->record_raw(fs_raw_value);
    }

    if (filter[tool_index].filter(fs_raw_value)) { //fs_raw_value is rewritten - passed by reference
        FSensors_instance().AdcSide_FilteredIRQ(fs_raw_value, tool_index);
    } else {
        FSensors_instance().AdcSide_FilteredIRQ(FSensorADC::fs_filtered_value_not_ready, tool_index);
    }
}
