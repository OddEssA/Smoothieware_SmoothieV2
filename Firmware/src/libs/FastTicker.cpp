#include "FastTicker.h"
#include "tmr-setup.h"

#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>

// timers are specified in Hz and periods in microseconds
#define BASE_FREQUENCY 1000000
#define MAX_FREQUENCY 10000

FastTicker *FastTicker::instance;
bool FastTicker::started= false;

// This module uses a Timer to periodically call registered callbacks
// Modules register with a function ( callback ) and a frequency, and we then call that function at the given frequency.
// We use TMR1 for this

FastTicker *FastTicker::getInstance()
{
    if(instance == nullptr) {
        instance= new FastTicker;
    }

    return instance;
}

void FastTicker::deleteInstance()
{
    if(started) {
        instance->stop();
    }

    delete instance;
    instance= nullptr;
}

FastTicker::FastTicker()
{}

FastTicker::~FastTicker()
{}

#define _ramfunc_

_ramfunc_ static void timer_handler()
{
    FastTicker::getInstance()->tick();
}

// called once to start the timer
bool FastTicker::start()
{
    if(max_frequency == 0) {
        printf("WARNING: FastTicker not started as nothing has attached to it\n");
        return false;
    }

    if(!started) {
        if(max_frequency > MAX_FREQUENCY) {
            printf("ERROR: FastTicker cannot be set > %dHz\n", MAX_FREQUENCY);
            return false;
        }
        fasttick_setup(max_frequency, (void *)timer_handler);

    }else{
        printf("WARNING: FastTicker already started\n");
    }

    started= true;
    return true;
}

// called to stop the timer usually only called in TESTs
bool FastTicker::stop()
{
    if(started) {
        fasttick_stop();
        started= false;
    }
    return true;
}

int FastTicker::attach(uint32_t frequency, std::function<void(void)> cb)
{
    uint32_t period = BASE_FREQUENCY / frequency;
    int countdown = period;

    if(frequency > max_frequency) {
        // reset frequency to a higher value
        if(!set_frequency(frequency)) {
            printf("ERROR: FastTicker cannot be set > %dHz\n", MAX_FREQUENCY);
            return -1;
        }
        max_frequency = frequency;
    }

    taskENTER_CRITICAL();
    callbacks.push_back(std::make_tuple(countdown, period, cb));
    taskEXIT_CRITICAL();

    // return the index it is in
    return callbacks.size()-1;
}

void FastTicker::detach(int n)
{
    // TODO need to remove it but that would change all the indexes
    // For now we just zero the callback
    taskENTER_CRITICAL();
    std::get<2>(callbacks[n])= nullptr;
    taskEXIT_CRITICAL();
}

// Set the base frequency we use for all sub-frequencies
bool FastTicker::set_frequency( int frequency )
{
    if(frequency > MAX_FREQUENCY) return false;
    this->interval = BASE_FREQUENCY / frequency; // microsecond period

    if(started) {
        // change frequency of timer callback
        if(fasttick_set_frequency(frequency) != 1) {
            printf("ERROR: FastTicker failed to set frequency\n");
            return false;
        }
    }

    return true;
}

// This is an ISR
_ramfunc_ void FastTicker::tick()
{
    // Call all callbacks that need to be called
    for(auto& i : callbacks) {
        int& countdown= std::get<0>(i);
        countdown -= this->interval;
        if (countdown <= 0) {
            countdown += std::get<1>(i);
            auto& fnc= std::get<2>(i); // get the callback
            if(fnc) {
                fnc();
            }
        }
    }
}
