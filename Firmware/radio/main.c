// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
// Copyright (c) 2011 Andrew Tridgell, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	_start.c
///
/// Early startup code.
/// This file *must* be linked first for interrupt vector generation and main() to work.
/// XXX this may no longer be the case - it may be sufficient for the interrupt vectors
/// to be located in the same file as main()
///

#include <stdarg.h>
#include "radio.h"
#include "tdm.h"
#include "freq_hopping.h"

////////////////////////////////////////////////////////////////////////////////
/// @name	Interrupt vector prototypes
///
/// @note these *must* be placed in this file for SDCC to generate the
/// interrupt vector table correctly.
//@{

/// Serial rx/tx interrupt handler.
///
extern void	serial_interrupt(void)	__interrupt(INTERRUPT_UART0) __using(1);

/// Radio event interrupt handler.
///
extern void	Receiver_ISR(void)	__interrupt(INTERRUPT_INT0);

/// Timer tick interrupt handler
///
/// @todo switch this and everything it calls to use another register bank?
///
static void	T3_ISR(void)		__interrupt(INTERRUPT_TIMER3);

//@}

__code const char g_banner_string[] = "SiK " stringify(APP_VERSION_HIGH) "." stringify(APP_VERSION_LOW) " on " BOARD_NAME;
__code const char g_version_string[] = stringify(APP_VERSION_HIGH) "." stringify(APP_VERSION_LOW);

__pdata enum BoardFrequency	g_board_frequency;	///< board info from the bootloader
__pdata uint8_t			g_board_bl_version;	///< from the bootloader

/// Counter used by delay_msec
///
static volatile uint8_t delay_counter;

/// Configure the Si1000 for operation.
///
static void hardware_init(void);

/// Initialise the radio and bring it online.
///
static void radio_init(void);

void
main(void)
{
	// Stash board info from the bootloader before we let anything touch
	// the SFRs.
	//
	g_board_frequency = BOARD_FREQUENCY_REG;
	g_board_bl_version = BOARD_BL_VERSION_REG;

	// try to load parameters; set them to defaults if that fails.
	// this is done before hardware_init() to get the serial speed
	// XXX default parameter selection should be based on board info
	//
	if (!param_load())
		param_default();

	// Do hardware initialisation.
	hardware_init();

	// do radio initialisation
	radio_init();

	// turn on the receiver
	if (!radio_receiver_on()) {
		panic("failed to enable receiver");
	}

	tdm_serial_loop();
}

void
panic(char *fmt, ...)
{
	va_list ap;

	puts("\n**PANIC**");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	puts("");
	for (;;)
		;
}

static void
hardware_init(void)
{
	__pdata uint16_t	i;

	// Disable the watchdog timer
	PCA0MD	&= ~0x40;

	// Select the internal oscillator, prescale by 1
	FLSCL	 =  0x40;
	OSCICN	 =  0x8F;
	CLKSEL	 =  0x00;

	// Configure the VDD brown out detector
	VDM0CN	 =  0x80;
	for (i = 0; i < 350; i++);	// Wait 100us for initialization
	RSTSRC	 =  0x06;		// enable brown out and missing clock reset sources

	// Configure crossbar for UART
	P0MDOUT	 =  0x10;		// UART Tx push-pull
	SFRPAGE	 =  CONFIG_PAGE;
	P0DRV	 =  0x10;		// UART TX
	SFRPAGE	 =  LEGACY_PAGE;
	XBR0	 =  0x01;		// UART enable

	// SPI1
	XBR1	|= 0x40;	// enable SPI in 3-wire mode
	P1MDOUT	|= 0x15;	// SCK1, MOSI1, MISO1 push-pull
	SFRPAGE	 = CONFIG_PAGE;
	P1DRV	|= 0x15;	// SPI signals use high-current mode
	SFRPAGE	 = LEGACY_PAGE;
	SPI1CFG	 = 0x40;	// master mode
	SPI1CN	 = 0x00;	// 3 wire master mode
	SPI1CKR	 = 0x00;	// Initialise SPI prescaler to divide-by-2 (12.25MHz, technically out of spec)
	SPI1CN	|= 0x01;	// enable SPI
	NSS1	 = 1;		// set NSS high

	// Clear the radio interrupt state
	IE0	 = 0;

	// 200Hz timer tick using timer3
	// Derive timer values from SYSCLK, just for laughs.
	TMR3RLL	 = (65536UL - ((SYSCLK / 12) / 200)) & 0xff;
	TMR3RLH	 = ((65536UL - ((SYSCLK / 12) / 200)) >> 8) & 0xff;
	TMR3CN	 = 0x04;	// count at SYSCLK / 12 and start
	EIE1	|= 0x80;

	// UART - set the configured speed
	serial_init(param_get(PARAM_SERIAL_SPEED));

	// global interrupt enable
	EA = 1;

	// Turn on the 'radio running' LED and turn off the bootloader LED
	LED_RADIO = LED_ON;
	LED_BOOTLOADER = LED_OFF;

	XBR2	 =  0x40;		// Crossbar (GPIO) enable
}

static void
radio_init(void)
{
	__pdata uint32_t	freq;

	// Do generic PHY initialisation
	if (!radio_initialise()) {
		panic("radio_initialise failed");
	}

	switch (g_board_frequency) {
	case FREQ_433:
		freq = 433000000UL;
		break;
	case FREQ_470:
		freq = 470000000UL;
		break;
	case FREQ_868:
		freq = 868000000UL;
		break;
	case FREQ_915:
		freq = 915000000UL;
		break;
	default:
		panic("bad board frequency %d", g_board_frequency);
		freq = 0;	// keep the compiler happy
		break;
	}

	// set the frequency and channel spacing
	radio_set_frequency(freq);

	// set channel spacing to use 12.5MHz total frequency width
	radio_set_channel_spacing(250000UL);

	// start on a channel chosen by network ID
	radio_set_channel(param_get(PARAM_NETID) % NUM_FREQ_CHANNELS);
	
	// And intilise the radio with them.
	if (!radio_configure(param_get(PARAM_AIR_SPEED)*1000UL)) {
		panic("radio_configure failed");
	}

	// setup network ID
	radio_set_network_id(param_get(PARAM_NETID));

	// initialise TDM system
	tdm_init();

	// initialise frequency hopping system
	fhop_init(param_get(PARAM_NETID));
}

static void
T3_ISR(void) __interrupt(INTERRUPT_TIMER3)
{

	// re-arm the interrupt
	TMR3CN = 0x04;

	// call the AT parser tick
	at_timer();

	// update the delay counter
	if (delay_counter > 0)
		delay_counter--;
	
	// tell the tdm system that another 5ms has passed
	tdm_tick();
}

void
delay_set(uint16_t msec)
{
	if (msec >= 1250) {
		delay_counter = 255;
	} else {
		delay_counter = (msec + 4) / 5;
	}
}

void delay_set_ticks(uint8_t ticks)
{
	delay_counter = ticks;
}

bool
delay_expired()
{
	return delay_counter == 0;
}

void
delay_msec(uint16_t msec)
{
	delay_set(msec);
	while (!delay_expired())
		;
}
