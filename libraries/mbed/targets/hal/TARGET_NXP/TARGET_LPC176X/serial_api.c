/* mbed Microcontroller Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// math.h required for floating point operations for baud rate calculation
#include <math.h>
#include <string.h>

#include "serial_api.h"
#include "cmsis.h"
#include "pinmap.h"
#include "error.h"
#include "gpio_api.h"

/******************************************************************************
 * INITIALIZATION
 ******************************************************************************/
#define UART_NUM    4

static const PinMap PinMap_UART_TX[] = {
    {P0_0,  UART_3, 2},
    {P0_2,  UART_0, 1},
    {P0_10, UART_2, 1},
    {P0_15, UART_1, 1},
    {P0_25, UART_3, 3},
    {P2_0 , UART_1, 2},
    {P2_8 , UART_2, 2},
    {P4_28, UART_3, 3},
    {NC   , NC    , 0}
};

static const PinMap PinMap_UART_RX[] = {
    {P0_1 , UART_3, 2},
    {P0_3 , UART_0, 1},
    {P0_11, UART_2, 1},
    {P0_16, UART_1, 1},
    {P0_26, UART_3, 3},
    {P2_1 , UART_1, 2},
    {P2_9 , UART_2, 2},
    {P4_29, UART_3, 3},
    {NC   , NC    , 0}
};

static const PinMap PinMap_UART_RTS[] = {
    {P0_22, UART_1, 1},
    {P2_7,  UART_1, 2},
    {NC,    NC,     0}
};

static const PinMap PinMap_UART_CTS[] = {
    {P0_17, UART_1, 1},
    {P2_2,  UART_1, 2},
    {NC,    NC,     0}
};

#define UART_MCR_RTSEN_MASK     (1 << 6)
#define UART_MCR_CTSEN_MASK     (1 << 7)
#define UART_MCR_FLOWCTRL_MASK  (UART_MCR_RTSEN_MASK | UART_MCR_CTSEN_MASK)

static uint32_t serial_irq_ids[UART_NUM] = {0};
static uart_irq_handler irq_handler;

int stdio_uart_inited = 0;
serial_t stdio_uart;

struct serial_global_data_s {
    gpio_t sw_rts, sw_cts;
    uint8_t count, initialized, irq_set_flow, irq_set_api;
};

static struct serial_global_data_s uart_data[UART_NUM];

void serial_init(serial_t *obj, PinName tx, PinName rx) {
    int is_stdio_uart = 0;
    
    // determine the UART to use
    UARTName uart_tx = (UARTName)pinmap_peripheral(tx, PinMap_UART_TX);
    UARTName uart_rx = (UARTName)pinmap_peripheral(rx, PinMap_UART_RX);
    UARTName uart = (UARTName)pinmap_merge(uart_tx, uart_rx);
    if ((int)uart == NC) {
        error("Serial pinout mapping failed");
    }
    
    obj->uart = (LPC_UART_TypeDef *)uart;
    // enable power
    switch (uart) {
        case UART_0: LPC_SC->PCONP |= 1 <<  3; break;
        case UART_1: LPC_SC->PCONP |= 1 <<  4; break;
        case UART_2: LPC_SC->PCONP |= 1 << 24; break;
        case UART_3: LPC_SC->PCONP |= 1 << 25; break;
    }
    
    // enable fifos and default rx trigger level
    obj->uart->FCR = 1 << 0  // FIFO Enable - 0 = Disables, 1 = Enabled
                   | 0 << 1  // Rx Fifo Reset
                   | 0 << 2  // Tx Fifo Reset
                   | 0 << 6; // Rx irq trigger level - 0 = 1 char, 1 = 4 chars, 2 = 8 chars, 3 = 14 chars

    // disable irqs
    obj->uart->IER = 0 << 0  // Rx Data available irq enable
                   | 0 << 1  // Tx Fifo empty irq enable
                   | 0 << 2; // Rx Line Status irq enable
    
    // set default baud rate and format
    serial_baud  (obj, 9600);
    serial_format(obj, 8, ParityNone, 1);
    
    // pinout the chosen uart
    pinmap_pinout(tx, PinMap_UART_TX);
    pinmap_pinout(rx, PinMap_UART_RX);
    
    // set rx/tx pins in PullUp mode
    pin_mode(tx, PullUp);
    pin_mode(rx, PullUp);
    
    switch (uart) {
        case UART_0: obj->index = 0; break;
        case UART_1: obj->index = 1; break;
        case UART_2: obj->index = 2; break;
        case UART_3: obj->index = 3; break;
    }
    if (!uart_data[obj->index].initialized) {
        uart_data[obj->index].sw_rts.pin = NC;
        uart_data[obj->index].sw_cts.pin = NC;
        uart_data[obj->index].initialized = 1;
    }
    
    is_stdio_uart = (uart == STDIO_UART) ? (1) : (0);
    
    if (is_stdio_uart) {
        stdio_uart_inited = 1;
        memcpy(&stdio_uart, obj, sizeof(serial_t));
    }
}

void serial_free(serial_t *obj) {
    serial_irq_ids[obj->index] = 0;
}

// serial_baud
// set the baud rate, taking in to account the current SystemFrequency
void serial_baud(serial_t *obj, int baudrate) {
    // The LPC2300 and LPC1700 have a divider and a fractional divider to control the
    // baud rate. The formula is:
    //
    // Baudrate = (1 / PCLK) * 16 * DL * (1 + DivAddVal / MulVal)
    //   where:
    //     1 < MulVal <= 15
    //     0 <= DivAddVal < 14
    //     DivAddVal < MulVal
    //
    // set pclk to /1
    switch ((int)obj->uart) {
        case UART_0: LPC_SC->PCLKSEL0 &= ~(0x3 <<  6); LPC_SC->PCLKSEL0 |= (0x1 <<  6); break;
        case UART_1: LPC_SC->PCLKSEL0 &= ~(0x3 <<  8); LPC_SC->PCLKSEL0 |= (0x1 <<  8); break;
        case UART_2: LPC_SC->PCLKSEL1 &= ~(0x3 << 16); LPC_SC->PCLKSEL1 |= (0x1 << 16); break;
        case UART_3: LPC_SC->PCLKSEL1 &= ~(0x3 << 18); LPC_SC->PCLKSEL1 |= (0x1 << 18); break;
        default: error("serial_baud"); break;
    }
    
    uint32_t PCLK = SystemCoreClock;
    
    // First we check to see if the basic divide with no DivAddVal/MulVal
    // ratio gives us an integer result. If it does, we set DivAddVal = 0,
    // MulVal = 1. Otherwise, we search the valid ratio value range to find
    // the closest match. This could be more elegant, using search methods
    // and/or lookup tables, but the brute force method is not that much
    // slower, and is more maintainable.
    uint16_t DL = PCLK / (16 * baudrate);

    uint8_t DivAddVal = 0;
    uint8_t MulVal = 1;
    int hit = 0;
    uint16_t dlv;
    uint8_t mv, dav;
    if ((PCLK % (16 * baudrate)) != 0) {     // Checking for zero remainder
        float err_best = (float) baudrate;
        uint16_t dlmax = DL;
        for ( dlv = (dlmax/2); (dlv <= dlmax) && !hit; dlv++) {
            for ( mv = 1; mv <= 15; mv++) {
                for ( dav = 1; dav < mv; dav++) {
                    float ratio = 1.0f + ((float) dav / (float) mv);
                    float calcbaud = (float)PCLK / (16.0f * (float) dlv * ratio);
                    float err = fabs(((float) baudrate - calcbaud) / (float) baudrate);
                    if (err < err_best) {
                        DL = dlv;
                        DivAddVal = dav;
                        MulVal = mv;
                        err_best = err;
                        if (err < 0.001f) {
                            hit = 1;
                        }
                    }
                }
            }
        }
    }
    
    // set LCR[DLAB] to enable writing to divider registers
    obj->uart->LCR |= (1 << 7);
    
    // set divider values
    obj->uart->DLM = (DL >> 8) & 0xFF;
    obj->uart->DLL = (DL >> 0) & 0xFF;
    obj->uart->FDR = (uint32_t) DivAddVal << 0
                   | (uint32_t) MulVal    << 4;
    
    // clear LCR[DLAB]
    obj->uart->LCR &= ~(1 << 7);
}

void serial_format(serial_t *obj, int data_bits, SerialParity parity, int stop_bits) {
    // 0: 1 stop bits, 1: 2 stop bits
    if (stop_bits != 1 && stop_bits != 2) {
        error("Invalid stop bits specified");
    }
    stop_bits -= 1;
    
    // 0: 5 data bits ... 3: 8 data bits
    if (data_bits < 5 || data_bits > 8) {
        error("Invalid number of bits (%d) in serial format, should be 5..8", data_bits);
    }
    data_bits -= 5;

    int parity_enable, parity_select;
    switch (parity) {
        case ParityNone: parity_enable = 0; parity_select = 0; break;
        case ParityOdd : parity_enable = 1; parity_select = 0; break;
        case ParityEven: parity_enable = 1; parity_select = 1; break;
        case ParityForced1: parity_enable = 1; parity_select = 2; break;
        case ParityForced0: parity_enable = 1; parity_select = 3; break;
        default:
            error("Invalid serial parity setting");
            return;
    }
    
    obj->uart->LCR = data_bits            << 0
                   | stop_bits            << 2
                   | parity_enable        << 3
                   | parity_select        << 4;
}

/******************************************************************************
 * INTERRUPTS HANDLING
 ******************************************************************************/
static inline void uart_irq(uint32_t iir, uint32_t index) {
    // [Chapter 14] LPC17xx UART0/2/3: UARTn Interrupt Handling
    SerialIrq irq_type;
    switch (iir) {
        case 1: irq_type = TxIrq; break;
        case 2: irq_type = RxIrq; break;
        default: return;
    }

    if ((RxIrq == irq_type) && (uart_data[index].sw_rts.pin != NC))
        gpio_write(&uart_data[index].sw_rts, 1);
    if (serial_irq_ids[index] != 0)
        irq_handler(serial_irq_ids[index], irq_type);
}

void uart0_irq() {uart_irq((LPC_UART0->IIR >> 1) & 0x7, 0);}
void uart1_irq() {uart_irq((LPC_UART1->IIR >> 1) & 0x7, 1);}
void uart2_irq() {uart_irq((LPC_UART2->IIR >> 1) & 0x7, 2);}
void uart3_irq() {uart_irq((LPC_UART3->IIR >> 1) & 0x7, 3);}

void serial_irq_handler(serial_t *obj, uart_irq_handler handler, uint32_t id) {
    irq_handler = handler;
    serial_irq_ids[obj->index] = id;
}

static void serial_irq_set_internal(serial_t *obj, SerialIrq irq, uint32_t enable) {
    IRQn_Type irq_n = (IRQn_Type)0;
    uint32_t vector = 0;
    switch ((int)obj->uart) {
        case UART_0: irq_n=UART0_IRQn; vector = (uint32_t)&uart0_irq; break;
        case UART_1: irq_n=UART1_IRQn; vector = (uint32_t)&uart1_irq; break;
        case UART_2: irq_n=UART2_IRQn; vector = (uint32_t)&uart2_irq; break;
        case UART_3: irq_n=UART3_IRQn; vector = (uint32_t)&uart3_irq; break;
    }
    
    if (enable) {
        obj->uart->IER |= 1 << irq;
        NVIC_SetVector(irq_n, vector);
        NVIC_EnableIRQ(irq_n);
    } else if (uart_data[obj->index].irq_set_api + uart_data[obj->index].irq_set_flow == 0) { // disable
        int all_disabled = 0;
        SerialIrq other_irq = (irq == RxIrq) ? (TxIrq) : (RxIrq);
        obj->uart->IER &= ~(1 << irq);
        all_disabled = (obj->uart->IER & (1 << other_irq)) == 0;
        if (all_disabled)
            NVIC_DisableIRQ(irq_n);
    }
}

void serial_irq_set(serial_t *obj, SerialIrq irq, uint32_t enable) {
    uart_data[obj->index].irq_set_api = enable;
    serial_irq_set_internal(obj, irq, enable);
}

static void serial_flow_irq_set(serial_t *obj, SerialIrq irq, uint32_t enable) {
    uart_data[obj->index].irq_set_flow = enable;
    serial_irq_set_internal(obj, irq, enable);
}

/******************************************************************************
 * READ/WRITE
 ******************************************************************************/
int serial_getc(serial_t *obj) {
    while (!serial_readable(obj));
    if (NC != uart_data[obj->index].sw_rts.pin)
        gpio_write(&uart_data[obj->index].sw_rts, 0);
    return obj->uart->RBR;
}

void serial_putc(serial_t *obj, int c) {
    while (!serial_writable(obj));
    obj->uart->THR = c;
    uart_data[obj->index].count++;
}

int serial_readable(serial_t *obj) {
    return obj->uart->LSR & 0x01;
}

int serial_writable(serial_t *obj) {
    int isWritable = 1;
    if (NC != uart_data[obj->index].sw_cts.pin)
        isWritable = gpio_read(&uart_data[obj->index].sw_cts) == 0;
    if (isWritable) {
        if (obj->uart->LSR & 0x20)
            uart_data[obj->index].count = 0;
        else if (uart_data[obj->index].count >= 16)
            isWritable = 0;
    }
    return isWritable;
}

void serial_clear(serial_t *obj) {
    obj->uart->FCR = 1 << 0  // FIFO Enable - 0 = Disables, 1 = Enabled
                   | 1 << 1  // rx FIFO reset
                   | 1 << 2  // tx FIFO reset
                   | 0 << 6; // interrupt depth
}

void serial_pinout_tx(PinName tx) {
    pinmap_pinout(tx, PinMap_UART_TX);
}

void serial_break_set(serial_t *obj) {
    obj->uart->LCR |= (1 << 6);
}

void serial_break_clear(serial_t *obj) {
    obj->uart->LCR &= ~(1 << 6);
}

void serial_set_flow_control(serial_t *obj, FlowControl type, PinName rxflow, PinName txflow) {
    // Only UART1 has hardware flow control on LPC176x
    LPC_UART1_TypeDef *uart1 = (uint32_t)obj->uart == (uint32_t)LPC_UART1 ? LPC_UART1 : NULL;
    int index = obj->index;

    // First, disable flow control completely
    if (uart1)
        uart1->MCR = uart1->MCR & ~UART_MCR_FLOWCTRL_MASK;
    serial_flow_irq_set(obj, RxIrq, 0);
    uart_data[index].sw_rts.pin = uart_data[index].sw_cts.pin = NC;
    if (FlowControlNone == type)
        return;
    // Check type(s) of flow control to use
    UARTName uart_rts = (UARTName)pinmap_find_peripheral(rxflow, PinMap_UART_RTS);
    UARTName uart_cts = (UARTName)pinmap_find_peripheral(txflow, PinMap_UART_CTS);
    if (((FlowControlCTS == type) || (FlowControlRTSCTS == type)) && (NC != txflow)) {
        // Can this be enabled in hardware?
        if ((UART_1 == uart_cts) && (NULL != uart1)) {
            // Enable auto-CTS mode
            uart1->MCR |= UART_MCR_CTSEN_MASK;
        } else {
            // Can't enable in hardware, use software emulation
            gpio_init(&uart_data[index].sw_cts, txflow, PIN_INPUT);
        }
    }
    if (((FlowControlRTS == type) || (FlowControlRTSCTS == type)) && (NC != rxflow)) {
        // Enable FIFOs, trigger level of 1 char on RX FIFO
        obj->uart->FCR = 1 << 0  // FIFO Enable - 0 = Disables, 1 = Enabled
                       | 1 << 1  // Rx Fifo Reset
                       | 1 << 2  // Tx Fifo Reset
                       | 0 << 6; // Rx irq trigger level - 0 = 1 char, 1 = 4 chars, 2 = 8 chars, 3 = 14 chars
         // Can this be enabled in hardware?
        if ((UART_1 == uart_rts) && (NULL != uart1)) {
            // Enable auto-RTS mode
            uart1->MCR |= UART_MCR_RTSEN_MASK;
        } else { // can't enable in hardware, use software emulation
            gpio_init(&uart_data[index].sw_rts, rxflow, PIN_OUTPUT);
            gpio_write(&uart_data[index].sw_rts, 0);
            // Enable RX interrupt
            serial_flow_irq_set(obj, RxIrq, 1);
        }
    }
}

