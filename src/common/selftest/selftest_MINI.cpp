// selftest.cpp

#include "printer_selftest.hpp"
#include <fcntl.h>
#include <unistd.h>
#include "selftest_fan.h"
#include "selftest_axis.h"
#include "selftest_heater.h"
#include "selftest_firstlayer.hpp"
#include "stdarg.h"
#include "app.h"
#include "otp.h"
#include "hwio.h"
#include "marlin_server.hpp"
#include "wizard_config.hpp"
#include "../../Marlin/src/module/stepper.h"
#include "../../Marlin/src/module/temperature.h"
#include "eeprom.h"
#include "selftest_fans_type.hpp"
#include "selftest_axis_type.hpp"
#include "selftest_heaters_type.hpp"
#include "selftest_heaters_interface.hpp"
#include "selftest_fans_interface.hpp"
#include "selftest_axis_interface.hpp"
#include "selftest_netstatus_interface.hpp"
#include "selftest_firstlayer_interface.hpp"
#include "selftest_axis_config.hpp"
#include "selftest_heater_config.hpp"
#include "fanctl.hpp"
#include "timing.h"
#include "selftest_result_type.hpp"

using namespace selftest;

#define HOMING_TIME 15000 // ~15s when X and Y axes are at opposite side to home position
static constexpr feedRate_t maxFeedrates[] = DEFAULT_MAX_FEEDRATE;

static const char *_suffix[] = { "_fan", "_xyz", "_heaters" };
/// These speeds create major chord
/// https://en.wikipedia.org/wiki/Just_intonation

static const float XYfr_table[] = { 50, 62.5f, 75, 100 };
static const float Zfr_table_fw[] = { 10 };
static const float Zfr_table_bw[] = { 10 };

static constexpr size_t xy_fr_table_size = sizeof(XYfr_table) / sizeof(XYfr_table[0]);

#ifdef Z_AXIS_DO_NOT_TEST_MOVE_DOWN
static constexpr size_t z_fr_tables_size = sizeof(Zfr_table_fw) / sizeof(Zfr_table_fw[0]);
#else
static constexpr size_t z_fr_tables_size = sizeof(Zfr_table_fw) / sizeof(Zfr_table_fw[0]) + sizeof(Zfr_table_bw) / sizeof(Zfr_table_bw[0]);
#endif

static const uint16_t printFanMin_rpm_table[] = { 10, 10, 10, 10, 10 };

static const uint16_t printFanMax_rpm_table[] = { 10000, 10000, 10000, 10000, 10000 };

static const uint16_t heatBreakFanMin_rpm_table[] = { 10, 10, 10, 10, 10 };

static const uint16_t heatBreakFanMax_rpm_table[] = { 10000, 10000, 10000, 10000, 10000 };

// use this?
/*
static const uint16_t Fan0min_rpm_table[] = { 150, 1250, 3250, 3250, 3850 };

static const uint16_t Fan0max_rpm_table[] = { 1950, 3950, 5050, 5950, 6650 };

static const uint16_t Fan1min_rpm_table[] = { 2350, 4750, 5950, 6850, 7650 };

static const uint16_t Fan1max_rpm_table[] = { 3750, 5850, 7050, 8050, 8950 };
*/

static const FanConfig_t Config_Fans[2] = {
    { .type = fan_type_t::Print,
        .tool_nr = 0,
        .fanctl = fanCtlPrint[0],
        .pwm_start = 51,
        .pwm_step = 51,
        .rpm_min_table = printFanMin_rpm_table,
        .rpm_max_table = printFanMax_rpm_table,
        .steps = 5 },
    { .type = fan_type_t::Heatbreak,
        .tool_nr = 0,
        .fanctl = fanCtlHeatBreak[0],
        .pwm_start = 51,
        .pwm_step = 51,
        .rpm_min_table = heatBreakFanMin_rpm_table,
        .rpm_max_table = heatBreakFanMax_rpm_table,
        .steps = 5 }
};

const AxisConfig_t selftest::Config_XAxis = { .partname = "X-Axis", .length = 186, .fr_table_fw = XYfr_table, .fr_table_bw = XYfr_table, .length_min = 178, .length_max = 188, .axis = X_AXIS, .steps = xy_fr_table_size * 2, .movement_dir = -1 };

const AxisConfig_t selftest::Config_YAxis = { .partname = "Y-Axis", .length = 185, .fr_table_fw = XYfr_table, .fr_table_bw = XYfr_table, .length_min = 179, .length_max = 189, .axis = Y_AXIS, .steps = xy_fr_table_size * 2, .movement_dir = 1 };

static const AxisConfig_t Config_ZAxis = { .partname = "Z-Axis", .length = get_z_max_pos_mm(), .fr_table_fw = Zfr_table_fw, .fr_table_bw = Zfr_table_bw, .length_min = get_z_max_pos_mm() - 4, .length_max = get_z_max_pos_mm() + 6, .axis = Z_AXIS, .steps = z_fr_tables_size, .movement_dir = 1 };

static const HeaterConfig_t Config_HeaterNozzle[] = {
    {
        .partname = "Nozzle",
        .type = heater_type_t::Nozzle,
        .getTemp = []() { return thermalManager.temp_hotend[0].celsius; },
        .setTargetTemp = [](int target_temp) { thermalManager.setTargetHotend(target_temp, 0); },
        .refKp = Temperature::temp_hotend[0].pid.Kp,
        .refKi = Temperature::temp_hotend[0].pid.Ki,
        .refKd = Temperature::temp_hotend[0].pid.Kd,
        .heatbreak_fan = fanCtlHeatBreak[0],
        .print_fan = fanCtlPrint[0],
        .heat_time_ms = 42000,
        .start_temp = 40,
        .undercool_temp = 37,
        .target_temp = 290,
        .heat_min_temp = 130,
        .heat_max_temp = 190,
    }
};

static const HeaterConfig_t Config_HeaterBed = {
    .partname = "Bed",
    .type = heater_type_t::Bed,
    .getTemp = []() { return thermalManager.temp_bed.celsius; },
    .setTargetTemp = [](int target_temp) { thermalManager.setTargetBed(target_temp); },
    .refKp = Temperature::temp_bed.pid.Kp,
    .refKi = Temperature::temp_bed.pid.Ki,
    .refKd = Temperature::temp_bed.pid.Kd,
    .heatbreak_fan = fanCtlHeatBreak[0],
    .print_fan = fanCtlPrint[0],
    .heat_time_ms = 60000,
    .start_temp = 40,
    .undercool_temp = 39,
    .target_temp = 110,
    .heat_min_temp = 50,
    .heat_max_temp = 65,
};

static const FanConfig_t Config_fans_fine[2] = {
    { .type = fan_type_t::Print,
        .tool_nr = 0,
        .fanctl = fanCtlPrint[0],
        .pwm_start = 20,
        .pwm_step = 10,
        .rpm_min_table = nullptr,
        .rpm_max_table = nullptr,
        .steps = 24 },
    { .type = fan_type_t::Heatbreak,
        .tool_nr = 0,
        .fanctl = fanCtlHeatBreak[0],
        .pwm_start = 20,
        .pwm_step = 10,
        .rpm_min_table = nullptr,
        .rpm_max_table = nullptr,
        .steps = 24 }
};

static const FirstLayerConfig_t Config_FirstLayer = { .partname = "First Layer" };

CSelftest::CSelftest()
    : m_State(stsIdle)
    , m_Mask(stmNone)
    , pXAxis(nullptr)
    , pYAxis(nullptr)
    , pZAxis(nullptr)
    , pBed(nullptr)
    , pFirstLayer(nullptr) {
}

bool CSelftest::IsInProgress() const {
    return ((m_State != stsIdle) && (m_State != stsFinished) && (m_State != stsAborted));
}

bool CSelftest::Start(const uint64_t test_mask, [[maybe_unused]] const uint8_t tool_mask) {
    m_Mask = SelftestMask_t(test_mask);
    if (m_Mask & stmFans)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmWait_fans));
    if (m_Mask & stmXYZAxis)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmWait_axes));
    if (m_Mask & stmHeaters)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmWait_heaters));
    if (m_Mask & stmZAxis)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmMoveZup)); // if Z is calibrated, move it up
    if (m_Mask & stmFullSelftest)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmSelftestStart)); //any selftest state will trigger selftest additional init
    if (m_Mask & stmFullSelftest)
        m_Mask = (SelftestMask_t)(m_Mask | uint64_t(stmSelftestStop)); //any selftest state will trigger selftest additional deinit

    //dont show message about footer and do not wait response
    m_Mask = (SelftestMask_t)(m_Mask & (~(uint64_t(1) << stsPrologueInfo)));
    m_Mask = (SelftestMask_t)(m_Mask & (~(uint64_t(1) << stsPrologueInfo_wait_user)));

    m_State = stsStart;
    return true;
}

void CSelftest::Loop() {
    uint32_t time = ticks_ms();
    if ((time - m_Time) < SELFTEST_LOOP_PERIODE)
        return;
    m_Time = time;
    switch (m_State) {
    case stsIdle:
        return;
    case stsStart:
        phaseStart();
        break;
    case stsPrologueAskRun:
        FSM_CHANGE__LOGGING(Selftest, GuiDefaults::ShowDevelopmentTools ? PhasesSelftest::WizardPrologue_ask_run_dev : PhasesSelftest::WizardPrologue_ask_run);
        break;
    case stsPrologueAskRun_wait_user:
        if (phaseWaitUser(GuiDefaults::ShowDevelopmentTools ? PhasesSelftest::WizardPrologue_ask_run_dev : PhasesSelftest::WizardPrologue_ask_run))
            return;
        break;
    case stsSelftestStart:
        phaseSelftestStart();
        break;
    case stsPrologueInfo:
        FSM_CHANGE__LOGGING(Selftest, PhasesSelftest::WizardPrologue_info);
        break;
    case stsPrologueInfo_wait_user:
        if (phaseWaitUser(PhasesSelftest::WizardPrologue_info))
            return;
        break;
    case stsPrologueInfoDetailed:
        FSM_CHANGE__LOGGING(Selftest, PhasesSelftest::WizardPrologue_info_detailed);
        break;
    case stsPrologueInfoDetailed_wait_user:
        if (phaseWaitUser(PhasesSelftest::WizardPrologue_info_detailed))
            return;
        break;
    case stsFans:
        if (selftest::phaseFans(pFans, Config_Fans))
            return;
        break;
    case stsWait_fans:
        if (phaseWait())
            return;
        break;
    case stsXAxis: {
        if (selftest::phaseAxis(pXAxis, Config_XAxis))
            return;
        // Y is not skipped even if X fails
        break;
    }
    case stsYAxis: {
        if (selftest::phaseAxis(pYAxis, Config_YAxis))
            return;
        break;
    }
    case stsZAxis: {
        if (selftest::phaseAxis(pZAxis, Config_ZAxis))
            return;
        break;
    }
    case stsMoveZup:
#ifndef Z_AXIS_DO_NOT_TEST_MOVE_DOWN
        queue.enqueue_one_now("G0 Z100"); // move to 100 mm
#endif
        break;
    case stsWait_axes:
        if (phaseWait())
            return;
        break;
    case stsHeaters_noz_ena:
        selftest::phaseHeaters_noz_ena(pNozzles, Config_HeaterNozzle);
        break;
    case stsHeaters_bed_ena:
        selftest::phaseHeaters_bed_ena(pBed, Config_HeaterBed);
        break;
    case stsHeaters:
        if (selftest::phaseHeaters(pNozzles, pBed))
            return;
        break;
    case stsWait_heaters:
        if (phaseWait())
            return;
        break;
    case stsFans_fine:
        if (selftest::phaseFans(pFans, Config_fans_fine))
            return;
        break;
    case stsSelftestStop:
        restoreAfterSelftest();
        break;
    case stsNet_status:
        selftest::phaseNetStatus();
        break;
    case stsDidSelftestPass:
        phaseDidSelftestPass();
        break;
    case stsEpilogue_nok:
        if (SelftestResult_Failed(m_result)) {
            FSM_CHANGE__LOGGING(Selftest, PhasesSelftest::WizardEpilogue_nok);
        }
        break;
    case stsEpilogue_nok_wait_user:
        if (SelftestResult_Failed(m_result)) {
            if (phaseWaitUser(PhasesSelftest::WizardEpilogue_nok))
                return;
        }
        break;
    case stsShow_result:
        phaseShowResult();
        break;
    case stsFirstLayer:
        if (selftest::phaseFirstLayer(pFirstLayer, Config_FirstLayer))
            return;
        break;
    case stsResult_wait_user:
        if (phaseWaitUser(PhasesSelftest::Result))
            return;
        break;
    case stsEpilogue_ok:
        if (SelftestResult_Passed(m_result)) {
            FSM_CHANGE__LOGGING(Selftest, PhasesSelftest::WizardEpilogue_ok);
        }
        break;
    case stsEpilogue_ok_wait_user:
        if (SelftestResult_Passed(m_result)) {
            if (phaseWaitUser(PhasesSelftest::WizardEpilogue_ok))
                return;
        }
        break;
    case stsFinish:
        phaseFinish();
        break;
    case stsFinished:
    case stsAborted:
        return;
    }
    next();
}

void CSelftest::phaseShowResult() {
    eeprom_get_selftest_results(&m_result);
    FSM_CHANGE_WITH_DATA__LOGGING(Selftest, PhasesSelftest::Result, FsmSelftestResult().Serialize());
}

void CSelftest::phaseDidSelftestPass() {
    eeprom_get_selftest_results(&m_result);
    SelftestResult_Log(m_result);

    //dont run wizard again
    if (SelftestResult_Passed(m_result)) {
        eeprom_set_bool(EEVAR_RUN_SELFTEST, false); // clear selftest flag
        eeprom_set_bool(EEVAR_RUN_XYZCALIB, false); // clear XYZ calib flag
        eeprom_set_bool(EEVAR_RUN_FIRSTLAY, false); // clear first layer flag
    }
}

bool CSelftest::phaseWaitUser(PhasesSelftest phase) {
    const Response response = marlin_server::ClientResponseHandler::GetResponseFromPhase(phase);
    if (response == Response::Abort || response == Response::Cancel)
        Abort();
    if (response == Response::Ignore) {
        eeprom_set_bool(EEVAR_RUN_SELFTEST, false); // clear selftest flag
        eeprom_set_bool(EEVAR_RUN_XYZCALIB, false); // clear XYZ calib flag
        eeprom_set_bool(EEVAR_RUN_FIRSTLAY, false); // clear first layer flag
        Abort();
    }
    return response == Response::_none;
}

bool CSelftest::Abort() {
    if (!IsInProgress())
        return false;
    for (auto &pFan : pFans)
        abort_part(&pFan);
    abort_part((selftest::IPartHandler **)&pXAxis);
    abort_part((selftest::IPartHandler **)&pYAxis);
    abort_part((selftest::IPartHandler **)&pZAxis);
    for (auto &pNozzle : pNozzles)
        abort_part(&pNozzle);
    abort_part((selftest::IPartHandler **)&pBed);
    abort_part((selftest::IPartHandler **)&pFirstLayer);

    m_State = stsAborted;

    phaseFinish();
    return true;
}

void CSelftest::phaseSelftestStart() {
    if (m_Mask & stmHeaters) {
        // set bed to 35°C
        // heater test will start after temperature pass tru 40°C (we dont want to entire bed and sheet to be tempered at it)
        // so don't set 40°C, it could also trigger cooldown in case temperature is or similar 40.1°C
        thermalManager.setTargetBed(35);
        // no need to preheat nozzle, it heats up much faster than bed
        thermalManager.setTargetHotend(0, 0);
        marlin_server::set_temp_to_display(0, 0);
    }

    eeprom_get_selftest_results(&m_result); // read previous result
    if (m_Mask & stmFans) {
        m_result.tools[0].printFan = TestResult_Unknown;
        m_result.tools[0].heatBreakFan = TestResult_Unknown;
    }
    if (m_Mask & stmXAxis)
        m_result.xaxis = TestResult_Unknown;
    if (m_Mask & stmYAxis)
        m_result.yaxis = TestResult_Unknown;
    if (m_Mask & stmZAxis)
        m_result.zaxis = TestResult_Unknown;
    if (m_Mask & stmHeaters) {
        m_result.tools[0].nozzle = TestResult_Unknown;
        m_result.bed = TestResult_Unknown;
    }
    eeprom_set_selftest_results(&m_result); // reset status for all selftest parts in eeprom
}

void CSelftest::restoreAfterSelftest() {
    // disable heater target values - thermalManager.disable_all_heaters does not do that
    thermalManager.setTargetBed(0);
    thermalManager.setTargetHotend(0, 0);
    marlin_server::set_temp_to_display(0, 0);

    //restore fan behavior
    fanCtlPrint[0].ExitSelftestMode();
    fanCtlHeatBreak[0].ExitSelftestMode();

    thermalManager.disable_all_heaters();
    disable_all_steppers();
}

void CSelftest::next() {
    if ((m_State == stsFinished) || (m_State == stsAborted))
        return;
    int state = m_State + 1;
    while ((((uint64_t(1) << state) & m_Mask) == 0) && (state < stsFinish))
        state++;
    m_State = (SelftestState_t)state;

    // check, if state can run
    // this must be done after mask check
    eeprom_get_selftest_results(&m_result);
    switch (m_State) {
    case stsZAxis: // both X and Y must be OK to test Z
        if (m_result.xaxis == TestResult_Passed && m_result.yaxis == TestResult_Passed)
            return;  // current state can be run
        break;       // current state cannot be run
    case stsMoveZup: // Z must be OK, if axis are not homed, it could be stacked at the top and generate noise, but the way states are generated from mask should prevent it
        if (m_result.zaxis == TestResult_Passed)
            return; // current state can be run
        break;      // current state cannot be run
    default:
        return; // current state can be run
    }

    // current state cannot be run
    // call recursively: it is fine, this function is tiny and there will be few iterations
    next();
}

const char *CSelftest::get_log_suffix() {
    const char *suffix = "";
    if (m_Mask & stmFans)
        suffix = _suffix[0];
    else if (m_Mask & stmXYAxis)
        suffix = _suffix[1];
    else if (m_Mask & stmXYZAxis)
        suffix = _suffix[1];
    else if (m_Mask & stmHeaters)
        suffix = _suffix[2];
    return suffix;
}

//declared in parent source file
ISelftest &SelftestInstance() {
    static CSelftest ret = CSelftest();
    return ret;
}
