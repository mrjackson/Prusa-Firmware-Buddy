// marlin_server.hpp
#pragma once

#include <optional>
#include <atomic>
#include "marlin_vars.hpp"

#include "client_response.hpp"
#include "fsm_types.hpp"

#include "../../lib/Marlin/Marlin/src/inc/MarlinConfig.h"
#include "marlin_events.h"
#include "marlin_errors.h"
#include "client_fsm_types.h"
#include "marlin_server_extended_fsm_data.hpp"

#include <stddef.h>

namespace marlin_server {

// server flags
// FIXME define the same type for these and marlin_server.flags
constexpr uint16_t MARLIN_SFLG_STARTED = 0x0001; // server started (set in marlin_server::init)
constexpr uint16_t MARLIN_SFLG_PROCESS = 0x0002; // loop processing in main thread is enabled
constexpr uint16_t MARLIN_SFLG_BUSY = 0x0004;    // loop is busy
constexpr uint16_t MARLIN_SFLG_PENDREQ = 0x0008; // pending request
constexpr uint16_t MARLIN_SFLG_EXCMODE = 0x0010; // exclusive mode enabled (currently used for selftest/wizard)

// server variable update interval [ms]
constexpr uint8_t MARLIN_UPDATE_PERIOD = 100;

typedef void(idle_t)();

// callback for idle operation inside marlin (called from ExtUI handler onIdle)
extern idle_t *idle_cb;

extern osMessageQId server_queue;      // input queue (uint8_t)
extern osSemaphoreId server_semaphore; // semaphore handle

//-----------------------------------------------------------------------------
// server side functions (can be called from server thread only)

// initialize server side - must be called at beginning in server thread
void init();

// server loop - must be called periodically in server thread
int loop();

// returns enabled status of loop processing
bool processing();

// direct start loop processing
void start_processing();

// direct stop loop processing + disable heaters and safe state
void stop_processing();

// direct call of babystep.add_steps(Z_AXIS, ...)
void do_babystep_Z(float offs);

void move_axis(float pos, float feedrate, size_t axis);

// direct call of 'enqueue_and_echo_command'
// @retval true command enqueued
// @retval false otherwise
bool enqueue_gcode(const char *gcode);

// direct call of 'enqueue_and_echo_command' with formatting
// @retval true command enqueued
// @retval false otherwise
bool enqueue_gcode_printf(const char *gcode, ...);

// direct call of 'inject_P'
// @retval true command enqueued
// @retval false otherwise
bool inject_gcode(const char *gcode);

// direct call of settings.save()
void settings_save();

// direct call of settings.load()
void settings_load();

// direct call of settings.reset()
void settings_reset();

// direct print file with SFM format
void print_start(const char *filename, bool skip_preview);

//
uint32_t get_command();

//
void set_command(uint32_t command);

//
void test_start(const uint64_t test_mask, const uint8_t tool_mask);

//
void test_abort();

//
void print_abort();

//
void print_resume();

//
bool print_reheat_ready();

// return true if the printer is not moving (idle, paused, aborted or finished)
bool printer_idle();

typedef struct
{
    xyze_pos_t pos;               // resume position for unpark_head
    float nozzle_temp[EXTRUDERS]; // resume nozzle temperature
    uint8_t fan_speed;            // resume fan speed
    uint8_t print_speed;          // resume printing speed
} resume_state_t;

//
void print_pause();

void unpause_nozzle(const uint8_t extruder);

// return true if the printer is currently aborting or already aborted the print
bool aborting_or_aborted();

// return true if the printer is in the paused and not moving state
bool printer_paused();

// return the resume state during a paused print
resume_state_t *get_resume_data();

// set the resume state for unpausing a print
void set_resume_data(const resume_state_t *data);

/// Plans retract and returns E stepper position in mm
void retract();

/// Lifts printing head
void lift_head();

/// Parks head at print pause or crash
/// If Z lift or retraction wasn't performed
/// you can rerun them.
void park_head();

//
void unpark_head_XY();
void unpark_head_ZE();

//
bool all_axes_homed();

//
bool all_axes_known();

// returns state of exclusive mode (1/0)
int get_exclusive_mode();

// set state of exclusive mode (1/0)
void set_exclusive_mode(int exclusive);

// display different value than target, used in preheat
void set_temp_to_display(float value, uint8_t extruder);

bool get_media_inserted();

//
void resuming_begin();

uint32_t get_user_click_count();

uint32_t get_user_move_count();

void nozzle_timeout_on();
void nozzle_timeout_off();

// user can stop waiting for heating/cooling by pressing a button
bool can_stop_wait_for_heatup();
void can_stop_wait_for_heatup(bool val);

//inherited class for server side to be able to work with server_side_encoded_response
class ClientResponseHandler : public ClientResponses {
    ClientResponseHandler() = delete;
    ClientResponseHandler(ClientResponseHandler &) = delete;
    static std::atomic<uint32_t> server_side_encoded_response;

public:
    //call inside marlin server on received response from client
    static void SetResponse(uint32_t encoded_bt) {
        server_side_encoded_response = encoded_bt;
    }
    /// @returns currently recorded response and erases it
    /// @returns UINT32_MAX if phase does not match
    /// Can be used from a sub thread, as long as only one thread at the time reads it.
    /// Beware: calling this function erases the previous response (if any). That means calling this function from multiple
    /// dialogs/threads/places just for checking if there has been some input renders the whole printer unresponsive in all of the dialogs.
    template <class T>
    static Response GetResponseFromPhase(T phase) {
        const uint32_t value = server_side_encoded_response.exchange(UINT32_MAX); //read and erase response

        uint32_t _phase = value >> RESPONSE_BITS;
        if ((static_cast<uint32_t>(phase)) != _phase)
            return Response::_none;
        uint32_t index = value & uint32_t(MAX_RESPONSES - 1); //get response index
        return GetResponse(phase, index);
    }
};

//FSM_notifier
class FSM_notifier {
    struct data { //used floats - no need to retype
        ClientFSM type;
        uint8_t phase;
        float scale = 1;  //scale from value to progress
        float offset = 0; //offset from lowest value
        uint8_t progress_min = 0;
        uint8_t progress_max = 100;
        const MarlinVariable<float> *var_id;
        std::optional<uint8_t> last_progress_sent;
        data()
            : type(ClientFSM::_none)
            , phase(0)
            , var_id(nullptr) {}
    };
    //static members
    //there can be only one active instance of FSM_notifier, which use this data
    static data s_data;
    static FSM_notifier *activeInstance;

    //temporary members
    //constructor stores previous state of FSM_notifier (its static data), destructor restores it
    data temp_data;

protected:
    FSM_notifier(const FSM_notifier &) = delete;

public:
    FSM_notifier(ClientFSM type, uint8_t phase, float min, float max, uint8_t progress_min, uint8_t progress_max, const MarlinVariable<float> &var_id);
    virtual ~FSM_notifier();
    static void SendNotification();

    virtual fsm::PhaseData serialize(uint8_t progress) = 0;
};

// macros to call automatically fsm_create/change/destroy the way it logs __PRETTY_FUNCTION__, __FILE__, __LINE__
#define FSM_CREATE_WITH_DATA__LOGGING(fsm_type, phase, data)          marlin_server::fsm_create(ClientFSM::fsm_type, phase, data, __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_CREATE__LOGGING(fsm_type)                                 marlin_server::_fsm_create(ClientFSM::fsm_type, fsm::BaseData(), __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_DESTROY_AND_CREATE__LOGGING(fsm_old, fsm_new)             marlin_server::_fsm_destroy_and_create(ClientFSM::fsm_old, ClientFSM::fsm_new, fsm::BaseData(), __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_DESTROY__LOGGING(fsm_type)                                marlin_server::fsm_destroy(ClientFSM::fsm_type, __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_CHANGE_WITH_DATA__LOGGING(fsm_type, phase, data)          marlin_server::fsm_change(ClientFSM::fsm_type, phase, data, __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_CHANGE_WITH_EXTENDED_DATA__LOGGING(fsm_type, phase, data) marlin_server::fsm_change_extended(ClientFSM::fsm_type, phase, data, __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_CHANGE__LOGGING(fsm_type, phase)                          marlin_server::fsm_change(ClientFSM::fsm_type, phase, fsm::PhaseData({ 0, 0, 0, 0 }), __PRETTY_FUNCTION__, __FILE__, __LINE__)
//notify all clients to create finite statemachine
void _fsm_create(ClientFSM type, fsm::BaseData data, const char *fnc, const char *file, int line);
//notify all clients to destroy finite statemachine, must match fsm_destroy_t signature
void fsm_destroy(ClientFSM type, const char *fnc, const char *file, int line);
//notify all clients to change state of finite statemachine, must match fsm_change_t signature
//can be called inside while, notification is send only when is different from previous one
void _fsm_change(ClientFSM type, fsm::BaseData data, const char *fnc, const char *file, int line);
//notify all clients to atomically destroy and create finite statemachine
void _fsm_destroy_and_create(ClientFSM old_type, ClientFSM new_type, fsm::BaseData data, const char *fnc, const char *file, int line);

template <class T>
void fsm_create(ClientFSM type, T phase, fsm::PhaseData data, const char *fnc, const char *file, int line) {
    _fsm_create(type, fsm::BaseData(GetPhaseIndex(phase), data), fnc, file, line);
}

template <class T>
void fsm_change(ClientFSM type, T phase, fsm::PhaseData data, const char *fnc, const char *file, int line) {
    _fsm_change(type, fsm::BaseData(GetPhaseIndex(phase), data), fnc, file, line);
}

template <class T, FSMExtendedDataSubclass DATA_TYPE>
void fsm_change_extended(ClientFSM type, T phase, DATA_TYPE data, const char *fnc, const char *file, int line) {
    bool changed = FSMExtendedDataManager::store(data);
    if (changed) {
        // Only send FSM change if data actually changed, We also use this ugly hack that we increment fsm_change_data[0] every time data changed, to force redraw of GUI
        static std::array<uint8_t, 4> fsm_change_data = { 0 };
        fsm_change_data[0]++;
        _fsm_change(type, fsm::BaseData(GetPhaseIndex(phase), fsm_change_data), fnc, file, line);
    }
}

template <class T>
void fsm_destroy_and_create(ClientFSM old_type, ClientFSM new_type, T phase, fsm::PhaseData data, const char *fnc, const char *file, int line) {
    _fsm_destroy_and_create(old_type, new_type, fsm::BaseData(GetPhaseIndex(phase), data), fnc, file, line);
}

/**
 * @brief create finite state machine and automatically destroy it at the end of scope
 * do not create it directly, use FSM_HOLDER__LOGGING / FSM_HOLDER_WITH_DATA__LOGGING instead
 */
class FSM_Holder {
    ClientFSM dialog;
    const char *fnc;
    const char *file;
    int line;

public:
    FSM_Holder(ClientFSM type, const char *fnc, const char *file, int line)
        : dialog(type)
        , fnc(fnc)
        , file(file)
        , line(line) {
        _fsm_create(type, fsm::BaseData(), fnc, file, line);
    }

    template <class T>
    FSM_Holder(fsm::PhaseData data, T phase, ClientFSM type, const char *fnc, const char *file, int line) // had to put data first, so Template parameter does not collide with other ctor
        : dialog(type)
        , fnc(fnc)
        , file(file)
        , line(line) {
        fsm_create(type, phase, data, fnc, file, line);
    }

    template <class T>
    void Change(T phase) const {
        fsm_change(dialog, phase);
    }

    template <class T>
    void Change(T phase, fsm::PhaseData data, const char *fnc, const char *file, int line) const {
        fsm_change(dialog, phase, data, fnc, file, line);
    }

    template <class T, class U>
    void Change(T phase, const U &serializer, const char *fnc, const char *file, int line) const {
        fsm_change(dialog, phase, serializer.Serialize(), fnc, file, line);
    }

    ~FSM_Holder() {
        fsm_destroy(dialog, fnc, file, line);
    }
};
// macro to create automatically FSM_Holder instance the way it logs __PRETTY_FUNCTION__, __FILE__, __LINE__
// instance name is fsm_type##_from_macro - for example Selftest_from_macro in case of ClientFSM::Selftest
#define FSM_HOLDER__LOGGING(fsm_type)                        marlin_server::FSM_Holder fsm_type##_from_macro(ClientFSM::fsm_type, __PRETTY_FUNCTION__, __FILE__, __LINE__)
#define FSM_HOLDER_WITH_DATA__LOGGING(fsm_type, phase, data) marlin_server::FSM_Holder fsm_type##_from_macro(data, phase, ClientFSM::fsm_type, __PRETTY_FUNCTION__, __FILE__, __LINE__)
// macro to call change over FSM_Holder instance the way it logs __PRETTY_FUNCTION__, __FILE__, __LINE__
// first parameter is instance name
#define FSM_HOLDER_CHANGE_METHOD__LOGGING(fsm, phase, data) fsm.Change(phase, data, __PRETTY_FUNCTION__, __FILE__, __LINE__)

uint8_t get_var_sd_percent_done();
void set_var_sd_percent_done(uint8_t value);
void set_warning(WarningType type);

#if ENABLED(AXIS_MEASURE)
// Sets length of X and Y axes for crash recovery
void set_axes_length(xy_float_t xy);
#endif

void powerpanic_resume_loop(const char *media_SFN_path, uint32_t pos, bool auto_recover);
void powerpanic_finish(bool paused);

} // marlin_server namespace
