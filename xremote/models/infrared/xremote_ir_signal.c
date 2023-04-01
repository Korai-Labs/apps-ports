#include "xremote_ir_signal.h"

#include <stdlib.h>
#include <string.h>
#include <core/check.h>
#include <infrared_worker.h>
#include <infrared_transmit.h>
#include "../../xremote_i.h"

static void xremote_ir_signal_clear_timings(InfraredSignal* signal) {
    if(signal->is_raw) {
        free(signal->payload.raw.timings);
        signal->payload.raw.timings_size = 0;
        signal->payload.raw.timings = NULL;
    }
}

static bool xremote_ir_signal_is_message_valid(InfraredMessage* message) {
    if(!infrared_is_protocol_valid(message->protocol)) {
        FURI_LOG_E(TAG, "Unknown protocol");
        return false;
    }

    uint32_t address_length = infrared_get_protocol_address_length(message->protocol);
    uint32_t address_mask = (1UL << address_length) - 1;

    if(message->address != (message->address & address_mask)) {
        FURI_LOG_E(
            TAG,
            "Address is out of range (mask 0x%08lX): 0x%lX\r\n",
            address_mask,
            message->address);
        return false;
    }

    uint32_t command_length = infrared_get_protocol_command_length(message->protocol);
    uint32_t command_mask = (1UL << command_length) - 1;

    if(message->command != (message->command & command_mask)) {
        FURI_LOG_E(
            TAG,
            "Command is out of range (mask 0x%08lX): 0x%lX\r\n",
            command_mask,
            message->command);
        return false;
    }

    return true;
}

static bool xremote_ir_signal_is_raw_valid(InfraredRawSignal* raw) {
    if((raw->frequency > INFRARED_MAX_FREQUENCY) || (raw->frequency < INFRARED_MIN_FREQUENCY)) {
        FURI_LOG_E(
            TAG,
            "Frequency is out of range (%X - %X): %lX",
            INFRARED_MIN_FREQUENCY,
            INFRARED_MAX_FREQUENCY,
            raw->frequency);
        return false;

    } else if((raw->duty_cycle <= 0) || (raw->duty_cycle > 1)) {
        FURI_LOG_E(TAG, "Duty cycle is out of range (0 - 1): %f", (double)raw->duty_cycle);
        return false;

    } else if((raw->timings_size <= 0) || (raw->timings_size > MAX_TIMINGS_AMOUNT)) {
        FURI_LOG_E(
            TAG,
            "Timings amount is out of range (0 - %X): %zX",
            MAX_TIMINGS_AMOUNT,
            raw->timings_size);
        return false;
    }

    return true;
}

static inline bool xremote_ir_signal_read_message(InfraredSignal* signal, FlipperFormat* ff) {
    FuriString* buf;
    buf = furi_string_alloc();
    bool success = false;

    do {
        if(!flipper_format_read_string(ff, "protocol", buf)) break;

        InfraredMessage message;
        message.protocol = infrared_get_protocol_by_name(furi_string_get_cstr(buf));
        success = flipper_format_read_hex(ff, "address", (uint8_t*)&message.address, 4) &&
                  flipper_format_read_hex(ff, "command", (uint8_t*)&message.command, 4) &&
                  xremote_ir_signal_is_message_valid(&message);

        if(!success) break;

        xremote_ir_signal_set_message(signal, &message);
    } while(0);

    return success;
}

static inline bool xremote_ir_signal_read_raw(InfraredSignal* signal, FlipperFormat* ff) {
    uint32_t timings_size, frequency;
    float duty_cycle;

    bool success = flipper_format_read_uint32(ff, "frequency", &frequency, 1) &&
                   flipper_format_read_float(ff, "duty_cycle", &duty_cycle, 1) &&
                   flipper_format_get_value_count(ff, "data", &timings_size);

    if(!success || timings_size > MAX_TIMINGS_AMOUNT) {
        return false;
    }

    uint32_t* timings = malloc(sizeof(uint32_t) * timings_size);
    success = flipper_format_read_uint32(ff, "data", timings, timings_size);

    if(success) {
        xremote_ir_signal_set_raw_signal(signal, timings, timings_size, frequency, duty_cycle);
    }

    free(timings);
    return success;
}

InfraredSignal* xremote_ir_signal_alloc() {
    InfraredSignal* signal = malloc(sizeof(InfraredSignal));

    signal->is_raw = false;
    signal->payload.message.protocol = InfraredProtocolUnknown;

    return signal;
}

void xremote_ir_signal_free(InfraredSignal* signal) {
    xremote_ir_signal_clear_timings(signal);
    free(signal);
}

bool xremote_ir_signal_is_raw(InfraredSignal* signal) {
    return signal->is_raw;
}

bool xremote_ir_signal_is_valid(InfraredSignal* signal) {
    return signal->is_raw ? xremote_ir_signal_is_raw_valid(&signal->payload.raw) :
                            xremote_ir_signal_is_message_valid(&signal->payload.message);
}

void xremote_ir_signal_set_signal(InfraredSignal* signal, const InfraredSignal* other) {
    if(other->is_raw) {
        const InfraredRawSignal* raw = &other->payload.raw;
        xremote_ir_signal_set_raw_signal(
            signal, raw->timings, raw->timings_size, raw->frequency, raw->duty_cycle);
    } else {
        const InfraredMessage* message = &other->payload.message;
        xremote_ir_signal_set_message(signal, message);
    }
}

void xremote_ir_signal_set_raw_signal(
    InfraredSignal* signal,
    const uint32_t* timings,
    size_t timings_size,
    uint32_t frequency,
    float duty_cycle) {
    xremote_ir_signal_clear_timings(signal);

    signal->is_raw = true;

    signal->payload.raw.timings_size = timings_size;
    signal->payload.raw.frequency = frequency;
    signal->payload.raw.duty_cycle = duty_cycle;

    signal->payload.raw.timings = malloc(timings_size * sizeof(uint32_t));
    memcpy(signal->payload.raw.timings, timings, timings_size * sizeof(uint32_t));
}

static bool xremote_ir_signal_read_body(InfraredSignal* signal, FlipperFormat* ff) {
    FuriString* tmp = furi_string_alloc();

    bool success = false;

    do {
        if(!flipper_format_read_string(ff, "type", tmp)) break;
        if(furi_string_equal(tmp, "raw")) {
            success = xremote_ir_signal_read_raw(signal, ff);
        } else if(furi_string_equal(tmp, "parsed")) {
            success = xremote_ir_signal_read_message(signal, ff);
        } else {
            FURI_LOG_E(TAG, "Unknown signal type");
        }
    } while(false);

    furi_string_free(tmp);
    return success;
}

bool xremote_ir_signal_read(InfraredSignal* signal, FlipperFormat* ff, FuriString* name) {
    FuriString* tmp = furi_string_alloc();

    bool success = false;

    do {
        if(!flipper_format_read_string(ff, "name", tmp)) break;
        furi_string_set(name, tmp);
        if(!xremote_ir_signal_read_body(signal, ff)) break;
        success = true;
    } while(0);

    furi_string_free(tmp);
    return success;
}

void xremote_ir_signal_set_message(InfraredSignal* signal, const InfraredMessage* message) {
    xremote_ir_signal_clear_timings(signal);

    signal->is_raw = false;
    signal->payload.message = *message;
}
