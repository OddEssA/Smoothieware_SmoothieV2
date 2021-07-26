#pragma once

#include <functional>
#include <cstdint>

class OutputStream;

using StartupFunc_t = std::function<void()>;
void register_startup(StartupFunc_t sf);
const char *get_config_error_msg();

// sleep for given ms, but don't block things like ?
void safe_sleep(uint32_t ms);

// get the vmotor and vfet voltages
float get_voltage_monitor(const char* name);
int get_voltage_monitor_names(const char *names[]);

extern "C" {
    void Board_LED_Toggle(int);
    void Board_LED_Set(int, bool);
    bool Board_LED_Test(int);
}

#define _ramfunc_ __attribute__ ((section(".ramfunctions"),long_call,noinline))

// the communications task priority (lower number is lower priority)
#define COMMS_PRI 3UL
// The command thread priority
#define CMDTHRD_PRI 2UL
