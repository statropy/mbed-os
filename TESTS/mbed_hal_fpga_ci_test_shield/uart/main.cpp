/*
 * Copyright (c) 2019, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !DEVICE_SERIAL
#error [NOT_SUPPORTED] SERIAL not supported for this target
#elif !COMPONENT_FPGA_CI_TEST_SHIELD
#error [NOT_SUPPORTED] FPGA CI Test Shield is needed to run this test
#elif !defined(TARGET_FF_ARDUINO) && !defined(MBED_CONF_TARGET_DEFAULT_FORM_FACTOR)
#error [NOT_SUPPORTED] Test not supported for this form factor
#else

#include "utest/utest.h"
#include "unity/unity.h"
#include "greentea-client/test_env.h"

using namespace utest::v1;

#include <stdlib.h>
#include "UARTTester.h"
#include "pinmap.h"
#include "test_utils.h"
#include "serial_api.h"
#include "us_ticker_api.h"

#define PUTC_REPS 16
#define GETC_REPS 16

// In the UART RX test, the request for the FPGA to start sending data is sent
// first. Then the execution is blocked at serial_getc() call. Since the DUT
// is not ready to receive UART data instantly after the request, the start of
// the actual transmission has to be dalyed.
// A measured delay for NUCLEO_F070RB is 193 us.
#define TX_START_DELAY_NS 250000

UARTTester tester(DefaultFormFactor::pins(), DefaultFormFactor::restricted_pins());

typedef struct {
    serial_t *ser;
    int *rx_buff;
    uint32_t rx_cnt;
    int *tx_buff;
    uint32_t tx_cnt;
} serial_test_data_t;

static void test_irq_handler(uint32_t id, SerialIrq event)
{
    serial_test_data_t *td = (serial_test_data_t *)id;
    int c = 0x01; // arbitrary, non-zero value
    if (event == RxIrq) {
        c = serial_getc(td->ser);
        core_util_critical_section_enter();
        if (td->rx_cnt < GETC_REPS) {
            td->rx_buff[td->rx_cnt] = c;
            td->rx_cnt++;
        }
        core_util_critical_section_exit();
    } else if (event == TxIrq) {
        core_util_critical_section_enter();
        if (td->tx_cnt < PUTC_REPS) {
            c = td->tx_buff[td->tx_cnt];
            td->tx_cnt++;
        }
        core_util_critical_section_exit();
        // Send either one of tx_buff[] values or 0x01.
        serial_putc(td->ser, c);
    }
}

static void uart_test_common(int baudrate, int data_bits, SerialParity parity, int stop_bits, PinName tx, PinName rx, PinName cts = NC, PinName rts = NC)
{
    // The FPGA CI shield only supports None, Odd & Even.
    // Forced parity is not supported on Atmel, Freescale, Nordic & STM targets.
    MBED_ASSERT(parity != ParityForced1 && parity != ParityForced0);

    // STM-specific constraints
    // Only 7, 8 & 9 data bits.
    MBED_ASSERT(data_bits >= 7 && data_bits <= 9);
    // Only Odd or Even parity for 7 data bits.
    if (data_bits == 7) {
        MBED_ASSERT(parity != ParityNone);
    }

    // Limit the actual TX & RX chars to 8 bits for this test.
    int test_buff_bits = data_bits < 8 ? data_bits : 8;

    // start_bit + data_bits + parity_bit + stop_bits
    int packet_bits = 1 + data_bits + stop_bits + (parity == ParityNone ? 0 : 1);
    us_timestamp_t packet_tx_time = 1000000 * packet_bits / baudrate;
    const ticker_data_t *const us_ticker = get_us_ticker_data();

    bool use_flow_control = (cts != NC && rts != NC) ? true : false;

    // Remap pins for test
    tester.reset();
    tester.pin_map_set(tx, MbedTester::LogicalPinUARTRx);
    tester.pin_map_set(rx, MbedTester::LogicalPinUARTTx);
    if (use_flow_control) {
        tester.pin_map_set(cts, MbedTester::LogicalPinUARTRts);
        tester.pin_map_set(rts, MbedTester::LogicalPinUARTCts);
    }

    // Initialize mbed UART pins
    serial_t serial;
    serial_init(&serial, tx, rx);
    serial_baud(&serial, baudrate);
    serial_format(&serial, data_bits, parity, stop_bits);
    if (use_flow_control) {
        serial_set_flow_control(&serial, FlowControlRTSCTS, rts, cts);
    } else {
        serial_set_flow_control(&serial, FlowControlNone, NC, NC);
    }

    // Reset tester stats and select UART
    tester.peripherals_reset();
    tester.select_peripheral(MbedTester::PeripheralUART);

    // Configure UART module
    tester.set_baud((uint32_t)baudrate);
    tester.set_bits((uint8_t)data_bits);
    tester.set_stops((uint8_t)stop_bits);
    switch (parity) {
        case ParityOdd:
            tester.set_parity(true, true);
            break;
        case ParityEven:
            tester.set_parity(true, false);
            break;
        case ParityNone:
        default:
            tester.set_parity(false, false);
            break;
    }
    if (use_flow_control) {
        tester.cts_deassert_delay(0);
    }

    int rx_buff[GETC_REPS] = {};
    int tx_buff[PUTC_REPS] = {};
    volatile serial_test_data_t td = {
        &serial,
        rx_buff,
        0,
        tx_buff,
        0
    };
    uint32_t checksum = 0;

    // DUT TX / FPGA RX
    int tx_val;
    tester.rx_start();
    for (uint32_t reps = 1; reps <= PUTC_REPS; reps++) {
        tx_val = rand() % (1 << test_buff_bits);
        checksum += tx_val;
        serial_putc(&serial, tx_val);
        us_timestamp_t end_ts = ticker_read_us(us_ticker) + 2 * packet_tx_time;
        while (tester.rx_get_count() != reps && ticker_read_us(us_ticker) <= end_ts) {
            // Wait (no longer than twice the time of one packet transfer) for
            // the FPGA to receive data and update the byte counter.
        }
        TEST_ASSERT_EQUAL_UINT32(reps, tester.rx_get_count());
        TEST_ASSERT_EQUAL(0, tester.rx_get_parity_errors());
        TEST_ASSERT_EQUAL(0, tester.rx_get_stop_errors());
        TEST_ASSERT_EQUAL(0, tester.rx_get_framing_errors());
        TEST_ASSERT_EQUAL_UINT32(checksum, tester.rx_get_checksum());
        TEST_ASSERT_EQUAL(tx_val, tester.rx_get_data());
    }
    tester.rx_stop();

    // DUT RX / FPGA TX
    // serial_getc() may return 16-bit as well as 8-bit value cast to an int.
    // Use a random initial value, but make sure it is low enouth,
    // so the FPGA will not overflow 8 bits when incrementing it.
    uint16_t tester_buff = rand() % ((1 << test_buff_bits) - GETC_REPS);
    tester.tx_set_next(tester_buff);
    tester.tx_set_count(GETC_REPS);
    if (!use_flow_control) {
        tester.tx_set_delay(TX_START_DELAY_NS);
    }
    tester.tx_start(use_flow_control);
    for (int i = 0; i < GETC_REPS; i++) {
        rx_buff[i] = serial_getc(&serial);
    }
    tester.tx_stop();
    for (int i = 0; i < GETC_REPS; tester_buff++, i++) {
        TEST_ASSERT_EQUAL(tester_buff, rx_buff[i]);
    }

    serial_irq_handler(&serial, test_irq_handler, (uint32_t) &td);

    // DUT TX (IRQ) / FPGA RX
    tx_val = rand() % ((1 << test_buff_bits) - PUTC_REPS);
    for (size_t i = 0; i < PUTC_REPS; tx_val++, i++) {
        td.tx_buff[i] = tx_val;
        checksum += tx_val;
    }

    tester.rx_start();
    core_util_critical_section_enter();
    td.tx_cnt = 0;
    // Enable only the TX IRQ.
    serial_irq_set(&serial, TxIrq, 1);
    core_util_critical_section_exit();
    while (core_util_atomic_load_u32(&td.tx_cnt) != PUTC_REPS) {
        // Wait until the last byte is written to UART TX reg.
    };
    core_util_critical_section_enter();
    serial_irq_set(&serial, TxIrq, 0);
    core_util_critical_section_exit();
    us_timestamp_t end_ts = ticker_read_us(us_ticker) + 2 * packet_tx_time;
    while (ticker_read_us(us_ticker) <= end_ts) {
        // Wait twice the time of one packet transfer for the FPGA
        // to receive and process data.
    };
    tester.rx_stop();
    TEST_ASSERT_EQUAL_UINT32(2 * PUTC_REPS, tester.rx_get_count());
    TEST_ASSERT_EQUAL(0, tester.rx_get_parity_errors());
    TEST_ASSERT_EQUAL(0, tester.rx_get_stop_errors());
    TEST_ASSERT_EQUAL(0, tester.rx_get_framing_errors());
    TEST_ASSERT_EQUAL_UINT32(checksum, tester.rx_get_checksum());
    TEST_ASSERT_EQUAL(tx_val - 1, tester.rx_get_data());

    // DUT RX (IRQ) / FPGA TX
    // serial_getc() may return 16-bit as well as 8-bit value cast to an int.
    // Use a random initial value, but make sure it is low enouth,
    // so the FPGA will not overflow 8 bits when incrementing it.
    tester_buff = rand() % ((1 << test_buff_bits) - GETC_REPS);
    tester.tx_set_next(tester_buff);
    tester.tx_set_count(GETC_REPS);
    if (!use_flow_control) {
        tester.tx_set_delay(TX_START_DELAY_NS);
    }
    core_util_critical_section_enter();
    // Enable only the RX IRQ.
    serial_irq_set(&serial, RxIrq, 1);
    core_util_critical_section_exit();
    tester.rx_start();
    tester.tx_start(use_flow_control);
    while (core_util_atomic_load_u32(&td.rx_cnt) != GETC_REPS) {
        // Wait until the last byte is received to UART RX reg.
    };
    core_util_critical_section_enter();
    serial_irq_set(&serial, RxIrq, 0);
    core_util_critical_section_exit();
    tester.tx_stop();
    tester.rx_stop();
    for (int i = 0; i < GETC_REPS; tester_buff++, i++) {
        TEST_ASSERT_EQUAL(tester_buff, td.rx_buff[i]);
    }

    // Make sure TX IRQ was disabled during the last RX test.
    TEST_ASSERT_EQUAL_UINT32(checksum, tester.rx_get_checksum());
    TEST_ASSERT_EQUAL_UINT32(2 * PUTC_REPS, tester.rx_get_count());

    // Cleanup
    serial_free(&serial);
    tester.reset();
}

void test_init_free(PinName tx, PinName rx, PinName cts = NC, PinName rts = NC)
{
    bool use_flow_control = (cts != NC && rts != NC) ? true : false;
    serial_t serial;
    serial_init(&serial, tx, rx);
    serial_baud(&serial, 9600);
    serial_format(&serial, 8, ParityNone, 1);
    if (use_flow_control) {
        serial_set_flow_control(&serial, FlowControlRTSCTS, rts, cts);
    }
    serial_free(&serial);
}

void test_init_free_no_fc(PinName tx, PinName rx)
{
    test_init_free(tx, rx);
}

template<int BAUDRATE, int DATA_BITS, SerialParity PARITY, int STOP_BITS>
void test_common(PinName tx, PinName rx, PinName cts, PinName rts)
{
    uart_test_common(BAUDRATE, DATA_BITS, PARITY, STOP_BITS, tx, rx, cts, rts);
}

template<int BAUDRATE, int DATA_BITS, SerialParity PARITY, int STOP_BITS>
void test_common_no_fc(PinName tx, PinName rx)
{
    uart_test_common(BAUDRATE, DATA_BITS, PARITY, STOP_BITS, tx, rx);
}

Case cases[] = {
    // Every set of pins from every peripheral.
    Case("init/free, FC on", all_ports<UARTPort, DefaultFormFactor, test_init_free>),
    Case("init/free, FC off", all_ports<UARTNoFCPort, DefaultFormFactor, test_init_free_no_fc>),

    // One set of pins from every peripheral.
    Case("basic, 9600, 8N1, FC on", all_peripherals<UARTPort, DefaultFormFactor, test_common<9600, 8, ParityNone, 1> >),
    Case("basic, 9600, 8N1, FC off", all_peripherals<UARTNoFCPort, DefaultFormFactor, test_common_no_fc<9600, 8, ParityNone, 1> >),

    // One set of pins from one peripheral.
    // baudrate
    Case("19200, 8N1, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<19200, 8, ParityNone, 1> >),
    Case("19200, 8N1, FC off", one_peripheral<UARTNoFCPort, DefaultFormFactor, test_common_no_fc<19200, 8, ParityNone, 1> >),
    Case("38400, 8N1, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<38400, 8, ParityNone, 1> >),
    Case("38400, 8N1, FC off", one_peripheral<UARTNoFCPort, DefaultFormFactor, test_common_no_fc<38400, 8, ParityNone, 1> >),
    Case("115200, 8N1, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<115200, 8, ParityNone, 1> >),
    Case("115200, 8N1, FC off", one_peripheral<UARTNoFCPort, DefaultFormFactor, test_common_no_fc<115200, 8, ParityNone, 1> >),
    // data bits: not tested (some platforms support 8 bits only)
    // parity
    Case("9600, 8O1, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<9600, 8, ParityOdd, 1> >),
    Case("9600, 8E1, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<9600, 8, ParityEven, 1> >),
    // stop bits
    Case("9600, 8N2, FC on", one_peripheral<UARTPort, DefaultFormFactor, test_common<9600, 8, ParityNone, 2> >),
    Case("9600, 8N2, FC off", one_peripheral<UARTNoFCPort, DefaultFormFactor, test_common_no_fc<9600, 8, ParityNone, 2> >),
};

utest::v1::status_t greentea_test_setup(const size_t number_of_cases)
{
    GREENTEA_SETUP(240, "default_auto");
    srand((unsigned) ticker_read_us(get_us_ticker_data()));
    return greentea_test_setup_handler(number_of_cases);
}

Specification specification(greentea_test_setup, cases, greentea_test_teardown_handler);

int main()
{
    Harness::run(specification);
}

#endif /* !DEVICE_SERIAL */
