/*
Copyright 2010,2011,2012,2013,2019 Jun WAKO <wakojun@gmail.com>

This software is licensed with a Modified BSD License.
All of this is supposed to be Free Software, Open Source, DFSG-free,
GPL-compatible, and OK to use in both free and proprietary applications.
Additions and corrections to this file are welcome.


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in
  the documentation and/or other materials provided with the
  distribution.

* Neither the name of the copyright holders nor the names of
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * IBM PC keyboard protocol
 */

#include <stdbool.h>
#include <avr/interrupt.h>
#include <util/atomic.h>
#include "ibmpc.h"
#include "debug.h"
#include "timer.h"
#include "wait.h"


#define WAIT(stat, us, err) do { \
    if (!wait_##stat(us)) { \
        ibmpc_error = err; \
        goto ERROR; \
    } \
} while (0)


volatile uint8_t ibmpc_protocol = IBMPC_PROTOCOL_AT;
volatile uint8_t ibmpc_error = IBMPC_ERR_NONE;

/* 2-byte buffer for data received from keyhboard
 * buffer states:
 *      FFFF: empty
 *      FFss: one data
 *      sstt: two data(full)
 *  0xFF can not be stored as data in buffer because it means empty or no data.
 */
static volatile uint16_t recv_data = 0xFFFF;
/* internal state of receiving data */
static volatile uint16_t isr_state = 0x8000;
static uint8_t timer_start = 0;

void ibmpc_host_init(void)
{
    // initialize reset pin to HiZ
    IBMPC_RST_HIZ();
    inhibit();
    IBMPC_INT_INIT();
    IBMPC_INT_OFF();
}

void ibmpc_host_enable(void)
{
    IBMPC_INT_ON();
    idle();
}

void ibmpc_host_disable(void)
{
    IBMPC_INT_OFF();
    inhibit();
}

int16_t ibmpc_host_send(uint8_t data)
{
    bool parity = true;
    ibmpc_error = IBMPC_ERR_NONE;

    if (ibmpc_protocol == IBMPC_PROTOCOL_XT) return -1;

    dprintf("w%02X ", data);

    IBMPC_INT_OFF();

    /* terminate a transmission if we have */
    inhibit();
    wait_us(100); // 100us [4]p.13, [5]p.50

    /* 'Request to Send' and Start bit */
    data_lo();
    clock_hi();
    WAIT(clock_lo, 10000, 1);   // 10ms [5]p.50

    /* Data bit[2-9] */
    for (uint8_t i = 0; i < 8; i++) {
        wait_us(15);
        if (data&(1<<i)) {
            parity = !parity;
            data_hi();
        } else {
            data_lo();
        }
        WAIT(clock_hi, 50, 2);
        WAIT(clock_lo, 50, 3);
    }

    /* Parity bit */
    wait_us(15);
    if (parity) { data_hi(); } else { data_lo(); }
    WAIT(clock_hi, 50, 4);
    WAIT(clock_lo, 50, 5);

    /* Stop bit */
    wait_us(15);
    data_hi();

    /* Ack */
    WAIT(data_lo, 50, 6);
    WAIT(clock_lo, 50, 7);

    /* wait for idle state */
    WAIT(clock_hi, 50, 8);
    WAIT(data_hi, 50, 9);

    // clear buffer to get response correctly
    recv_data = 0xFFFF;
    ibmpc_host_isr_clear();

    idle();
    IBMPC_INT_ON();
    return ibmpc_host_recv_response();
ERROR:
    ibmpc_error |= IBMPC_ERR_SEND;
    idle();
    IBMPC_INT_ON();
    return -1;
}

/*
 * Receive data from keyboard
 */
int16_t ibmpc_host_recv(void)
{
    uint16_t data = 0;
    uint8_t ret = 0xFF;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        data = recv_data;
        if ((data&0xFF00) != 0xFF00) {      // recv_data:sstt -> recv_data:FFtt, ret:ss
            ret = (data>>8)&0x00FF;
            recv_data = data | 0xFF00;
        } else if (data != 0xFFFF) {        // recv_data:FFss -> recv_data:FFFF, ret:ss
            ret = data&0x00FF;
            recv_data = data | 0x00FF;
        }
    }

    if (ret != 0xFF) dprintf("r%02X ", ret);
    if (recv_data != 0xFFFF) dprintf("b%04X ", recv_data);
    return ((ret != 0xFF) ? ret : -1);

}

int16_t ibmpc_host_recv_response(void)
{
    // Command may take 25ms/20ms at most([5]p.46, [3]p.21)
    uint8_t retry = 25;
    int16_t data = -1;
    while (retry-- && (data = ibmpc_host_recv()) == -1) {
        wait_ms(1);
    }
    return data;
}

void ibmpc_host_isr_clear(void)
{
    isr_state = 0x8000;
    recv_data = 0xFFFF;
}

// NOTE: With this ISR data line can be read within 2us after clock falling edge.
// To read data line early as possible:
// write naked ISR with asembly code to read the line and call C func to do other job?
ISR(IBMPC_INT_VECT)
{
    uint8_t dbit;
    dbit = IBMPC_DATA_PIN&(1<<IBMPC_DATA_BIT);

    // Timeout check
    uint8_t t;
    // use only the least byte of millisecond timer
    asm("lds %0, %1" : "=r" (t) : "p" (&timer_count));
    //t = (uint8_t)timer_count;    // compiler uses four registers instead of one
    if (isr_state == 0x8000) {
        timer_start = t;
    } else {
        // should not take more than 1ms
        if (timer_start != t && (uint8_t)(timer_start + 1) != t) {
            ibmpc_error = IBMPC_ERR_TIMEOUT;
            //goto ERROR;
            // timeout error recovery by clearing isr_state?
            timer_start = t;
            isr_state = 0x8000;
        }
    }

    isr_state = isr_state>>1;
    if (dbit) isr_state |= 0x8000;

    // isr_state: state of receiving data from keyboard
    //      This is initialized with 0x8000 before start receiving data and  MSB '1' of
    //      the initial value is marker bit to discrimitate between protocols.
    //      It stores sampled bit at MSB after right shift on each clock falling edge.
    //
    //            15 14 13 12   11 10  9  8    7  6  5  4    3  2  1  0
    //            -----------------------------------------------------
    // Initial:   *1  0  0  0    0  0  0  0 |  0  0  0  0    0  0  0  0     with MSB marker *1
    // XT_Type-1: b7 b6 b5 b4   b3 b2 b1 b0 |  1  0 *1  0    0  0  0  0     start bit 0 and 1 **
    // XT_Type-2: b7 b6 b5 b4   b3 b2 b1 b0 |  1 *1  0  0    0  0  0  0     start bit 1
    // AT:        st pr b7 b6   b5 b4 b3 b2 | b1 b0  0 *1    0  0  0  0     start bit 0
    // AT**:      pr b7 b6 b5   b4 b3 b2 b1 | b0  0 *1  0    0  0  0  0     before stop bit **
    //
    //             x  x  x  x    x  x  x  x |  0  0  0  0    0  0  0  0     midway(0-7 bits received)
    //             x  x  x  x    x  x  x  x | *1  0  0  0    0  0  0  0     midway(8 bits received)
    //             x  x  x  x    x  x  x  x |  0 *1  0  0    0  0  0  0     XT_Type1-midway or AT-midway
    //             x  x  x  x    x  x  x  x |  1 *1  0  0    0  0  0  0     XT_Type2-done
    //             x  x  x  x    x  x  x  x |  0  0 *1  0    0  0  0  0     AT-midway[b0=0]
    //             x  x  x  x    x  x  x  x |  1  0 *1  0    0  0  0  0     XT_Type1-done or AT-midway[b0=1]
    //             x  x  x  x    x  x  x  x |  x  1  1  0    0  0  0  0     illegal
    //             x  x  x  x    x  x  x  x |  x  x  0  1    0  0  0  0     AT-done
    //             x  x  x  x    x  x  x  x |  x  x  1  1    0  0  0  0     illegal
    //                                          other states than above     illegal
    //
    // **: AT takes same state as XT_Type1-done(1010 000) in case that AT b0 is 1,
    // to discriminate between the two protocol we will have to wait a while for AT stop bit.
    switch (isr_state & 0xFF) {
        case 0b00000000:
        case 0b10000000:
        case 0b01000000:
        case 0b00100000:
            // midway
            goto NEXT;
            break;
        case 0b11000000:
            // XT_Type2-done
            recv_data = recv_data<<8;
            recv_data |= (isr_state>>8) & 0xFF;
            goto DONE;
            break;
        case 0b10100000:
            {
                uint8_t us = 100;
                // wait for rising and falling edge of AT stop bit
                while (!(IBMPC_CLOCK_PIN&(1<<IBMPC_CLOCK_BIT)) && us) { wait_us(1); us--; }
                while (  IBMPC_CLOCK_PIN&(1<<IBMPC_CLOCK_BIT)  && us) { wait_us(1); us--; }

                if (us) {
                    // found stop bit: AT-midway - process the stop bit in next ISR
                    goto NEXT;
                } else {
                    // no stop bit: XT_Type1-done
                    recv_data = recv_data<<8;
                    recv_data |= (isr_state>>8) & 0xFF;
                    goto DONE;
                }
             }
            break;
        case 0b00010000:
        case 0b10010000:
        case 0b01010000:
        case 0b11010000:
            // TODO: parity check?
            // AT-done
            recv_data = recv_data<<8;
            recv_data |= (isr_state>>6) & 0xFF;
            goto DONE;
            break;
        case 0b01100000:
        case 0b11100000:
        case 0b00110000:
        case 0b10110000:
        case 0b01110000:
        case 0b11110000:
        default:            // xxxx_oooo(any 1 in low nibble)
            // Illegal
            ibmpc_error = IBMPC_ERR_ILLEGAL;
            goto ERROR;
            break;
    }

ERROR:
    isr_state = 0x8000;
    recv_data = 0xFF00; // clear data and scancode of error 0x00
    return;
DONE:
    // TODO: process error code: 0x00(AT), 0xFF(XT) in particular
    isr_state = 0x8000;  // clear to next data
NEXT:
    return;
}

/* send LED state to keyboard */
void ibmpc_host_set_led(uint8_t led)
{
    ibmpc_host_send(0xED);
    ibmpc_host_send(led);
}
