#pragma once

#include <stdint.h>
#include "ModbusProtocol.hpp"
#include "cmsis_os.h"
#include "ModbusRegisters.hpp"
#include "dwarf_errors.hpp"

namespace dwarf::ModbusControl {

bool Init();
void ProcessModbusMessages();
void UpdateRegisters();
void TriggerMarlinKillFault(dwarf_shared::errors::FaultStatusMask fault_id, const char *component, const char *message);
bool isDwarfSelected();

} //namespace
