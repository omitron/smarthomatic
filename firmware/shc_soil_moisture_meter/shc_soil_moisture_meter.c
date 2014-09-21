/*
* This file is part of smarthomatic, http://www.smarthomatic.org.
* Copyright (c) 2013 Uwe Freese
*
* smarthomatic is free software: you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation, either version 3 of the License, or (at your
* option) any later version.
*
* smarthomatic is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
* Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with smarthomatic. If not, see <http://www.gnu.org/licenses/>.
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <avr/sleep.h>

#include "rfm12.h"
#include "uart.h"

#include "../src_common/msggrp_generic.h"
#include "../src_common/msggrp_weather.h"

#include "../src_common/e2p_hardware.h"
#include "../src_common/e2p_generic.h"
#include "../src_common/e2p_soilmoisturemeter.h"

#include "aes256.h"
#include "util.h"
#include "version.h"

// Don't change this, because other switch count like 8 needs other status message.
// If support implemented, use EEPROM_SUPPORTEDSWITCHES_* E2P addresses.
#define SWITCH_COUNT 1

#define TRIGGER_DDR DDRC
#define TRIGGER_PIN 2
#define TRIGGER_PORT PORTC

#define BUTTON_DDR DDRD
#define BUTTON_PORT PORTD
#define BUTTON_PINPORT PIND
#define BUTTON_PIN 3

#define SEND_STATUS_EVERY_SEC 1800 // how often should a status be sent?
#define SEND_VERSION_STATUS_CYCLE 50 // send version status x times less than switch status (~once per day)

uint16_t device_id;
uint32_t station_packetcounter;
bool switch_state[SWITCH_COUNT];
uint16_t switch_timeout[SWITCH_COUNT];

uint16_t send_status_timeout = 5;
uint8_t version_status_cycle = SEND_VERSION_STATUS_CYCLE - 1; // send promptly after startup

uint32_t counter_thr = 35000; // configured by user
uint32_t counter_min = 100000; // min occurred value in current watering period
uint32_t counter_meas = 0;

uint16_t wupCnt = 0; // Amount of wake-up cycles counting from the last time a measurement was done.
uint16_t avgInt = 3; // The number of times a value is measured before an average is calculated and sent.

// TODO: Move to util
// calculate x^y
uint32_t power(uint32_t x, uint32_t y)
{
	uint32_t result = 1;

	while (y > 0)
	{
		result *= x;
		y--;
	}

	return result;
}

// TODO: Move to util?
// Read wakeup timer value from e2p, config rfm12 and
// return the value (in seconds).
uint16_t init_wakeup(void)
{
	uint16_t interval = e2p_soilmoisturemeter_get_wakeupinterval();
	
	if (interval == 0) // misconficuration in E2P
	{
		interval = WAKEUPINTERVAL_105S;
	}
	
	rfm12_set_wakeup_timer(interval);
	
	// Calculate wake-up time in seconds according RFM12B datasheet and round the value to seconds.
	uint16_t sec = (uint16_t)(((interval & 0xff) * power(2, (interval >> 8) & 0b11111) + 500) / 1000);
	UART_PUTF("Wake-up interval: %us\r\n", sec);
	
	return sec;
}

void switch_schmitt_trigger(bool b_on)
{
	if (b_on)
	{
		sbi(TRIGGER_PORT, TRIGGER_PIN);
	}
	else
	{
		cbi(TRIGGER_PORT, TRIGGER_PIN);
	}
}

void send_humidity_temperature_status(uint16_t hum)
{	
	UART_PUTS("Sending Humidity Status:\r\n");

	inc_packetcounter();

	// Set packet content
	pkg_header_init_weather_humiditytemperature_status();
	pkg_header_set_senderid(device_id);
	pkg_header_set_packetcounter(packetcounter);
	msg_weather_humiditytemperature_set_humidity(hum);
	msg_weather_humiditytemperature_set_temperature(0); // TODO: Read from ATMega
	
	UART_PUTF2("Send humidity: %u.%u%%, temperature: ", hum / 10, hum % 10);
	print_signed(0);
	UART_PUTS(" deg.C\r\n");

	pkg_header_calc_crc32();

	rfm12_send_bufx();
}

// Measure humidity, calculate relative value in permill and return it.
void measure_humidity(void)
{
	switch_schmitt_trigger(true);
	
	uint16_t result;

	// make PD5 an input and disable pull-ups
	DDRD &= ~(1 << 5);
	PORTD &= ~(1 << 5);

	// clear counter
	TCNT1H = 0x00;
	TCNT1L = 0x00;

	// configure counter and use external clock source, rising edge
	TCCR1A = 0x00;
	TCCR1B |= (1 << CS12) | (1 << CS11) | (1 << CS10);

	_delay_ms(100);

	//result = (TCNT1H << 8) | TCNT1L;
	result = TCNT1;

	TCCR1B = 0x00;  // turn counter off
	
	switch_schmitt_trigger(false);
	
	counter_meas += result;
	wupCnt++;
	
	UART_PUTF2("Measurement %u, Counter %u\r\n", wupCnt, result);
	
	if (wupCnt == avgInt)
	{
		uint32_t avg = counter_meas / avgInt;
		
		if (avg < counter_min)
		{
			counter_min = avg;
		}
		
		uint32_t result;
		
		if (avg > counter_thr)
		{
			result = 0;
		}
		else
		{
			result = (counter_thr - avg) * 1000 / (counter_thr - counter_min);
		}
		
		UART_PUTF("Avg: %u, ", avg);
		UART_PUTF("New min: %lu, ", counter_min);
		UART_PUTF("Result: %lu permill\r\n", result);
		
		send_humidity_temperature_status(result);
		wupCnt = 0;
		counter_meas = 0;
	}
	
	_delay_ms(100);
}

void send_version_status(void)
{
	inc_packetcounter();

	UART_PUTF4("Sending Version: v%u.%u.%u (%08lx)\r\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_HASH);
	
	// Set packet content
	pkg_header_init_generic_version_status();
	pkg_header_set_senderid(device_id);
	pkg_header_set_packetcounter(packetcounter);
	msg_generic_version_set_major(VERSION_MAJOR);
	msg_generic_version_set_minor(VERSION_MINOR);
	msg_generic_version_set_patch(VERSION_PATCH);
	msg_generic_version_set_hash(VERSION_HASH);
	pkg_header_calc_crc32();

	rfm12_send_bufx();
}

int main(void)
{
	uint8_t loop = 0;
	uint16_t wakeup_sec;

	// delay 1s to avoid further communication with uart or RFM12 when my programmer resets the MC after 500ms...
	_delay_ms(1000);

	util_init();
	
	check_eeprom_compatibility(DEVICETYPE_SOILMOISTUREMETER);

	// init button input
	cbi(BUTTON_DDR, BUTTON_PIN);
	sbi(BUTTON_PORT, BUTTON_PIN);
	
	// init power pin for 74HC14D
	sbi(TRIGGER_DDR, TRIGGER_PIN);

	// read packetcounter, increase by cycle and write back
	packetcounter = e2p_generic_get_packetcounter() + PACKET_COUNTER_WRITE_CYCLE;
	e2p_generic_set_packetcounter(packetcounter);

	// read device id
	device_id = e2p_generic_get_deviceid();

	osccal_init();

	uart_init();

	UART_PUTS ("\r\n");
	UART_PUTF4("smarthomatic Soil Moisture Meter v%u.%u.%u (%08lx)\r\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_HASH);
	UART_PUTS("(c) 2014 Uwe Freese, www.smarthomatic.org\r\n");
	osccal_info();
	UART_PUTF ("DeviceID: %u\r\n", device_id);
	UART_PUTF ("PacketCounter: %lu\r\n", packetcounter);
	
	// init AES key
	e2p_generic_get_aeskey(aes_key);

	led_blink(500, 500, 3);

	rfm12_init();
	wakeup_sec = init_wakeup();

	sei();

	while (42)
	{
		// send status from time to time
		send_status_timeout--;
	
		measure_humidity();
		
		if (version_status_cycle >= SEND_VERSION_STATUS_CYCLE)
		{
			version_status_cycle = 0;
			send_version_status();
			led_blink(200, 0, 1);
		}

		rfm12_tick();
		
		/*

		button = !(BUTTON_PINPORT & (1 << BUTTON_PIN));
		
		if (button_debounce > 0)
		{
			button_debounce--;
		}
		else if (button != button_old)
		{
			button_old = button;
			button_debounce = 10;
			
			if (button) // on button press
			{
				UART_PUTS("Button! ");
				switchRelais(0, !switch_state[0], 0);
				send_status_timeout = 15; // send status after 15s
			}
		}	
*/
		loop++;
		
		// Go to sleep. Wakeup by RFM12 wakeup-interrupt or pin change (if configured).
		set_sleep_mode(SLEEP_MODE_PWR_DOWN);
        sleep_mode();
	}
	
	// never called
	// aes256_done(&aes_ctx);
}