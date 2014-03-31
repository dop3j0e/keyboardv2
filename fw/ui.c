
#include <stdio.h>
#include "hw.h"
#include "lcd_drv.h"
#include "common.h"
#include "panel.h"
#include "ui.h"
#include "key.h"
#include "key_timer.h"
#include "config.h"

uint8_t ui_state = UIS_IDLE;
uint8_t ui_flags = 0;
uint8_t selected_key = 0;
uint8_t selected_time;
uint8_t max_selectable_time;
uint8_t ui_timer = 0;
uint8_t expired_timer;
uint8_t error_slot;

static int16_t getMinimumTimer(uint8_t limit)
{
	uint8_t i;
	int16_t min = INT16_MAX;

	for (i = 0; i < limit; i++)
		if (keyTimers[i] >= 0 && keyTimers[i] < min)
			min = keyTimers[i];

	return (min == INT16_MAX) ? -1 : min;
}

static void print_time(int16_t timeInSeconds)
{
	if (timeInSeconds < 0) {
		lcd_print_update_P(1, PSTR("--- "));
	} else if (timeInSeconds < 60) {
		// print <timeInSeconds>s
		lcd_print_update_P(1, PSTR("%2ds "), timeInSeconds);
	} else {
		// print <timeInSeconds / 60>m
		lcd_print_update_P(1, PSTR("%2dm "), timeInSeconds / 60);
	}
}

/*
 * The pulsation frequency of the Smaul button LED depends on how soon the Keyboard will
 * start to throw a fit. As frequency is not perceived linearly, but rather logarithmically
 * (see audio frequencies), we want to double the frequency roughly every time unit, but
 * preferably we want to go slower in the beginning and go faster the closer to the alarm we
 * get. So let's choose
 *   freq = 2^y  with  y = a*(x^2) + b  and  x = t0 - t
 *   and a, b chosen such that  freq(0) = 200  and  freq(t0) = 6
 *
 * 2^b = 6 <=> b = log2(6)
 * 2^(a*t0^2 + b) = 200 <=> a = (log2(200) - log(6)) / (t0^2)
 *
 * We avoid floating point math in the uC by generating a small table for these values
 * and interpolating between them.
 */

#if 0
/* Generate a table of pulsation frequencies */
#include <stdio.h>
#include <math.h>

const double t0 = 180.0;
const double f0 = 6.0;
const double f1 = 200.0;

const int INTERP_FACTOR = 8;

double log2(double x)
{
	return log(x) / log(2);
}

int main(void)
{
	int i;
	double x, y, a, b;
	int TABLE_SIZE = (int)t0 / INTERP_FACTOR;

	b = log2(f0);
	a = (log2(f1) - log2(f0)) / (t0 * t0);

	for (i = 0; i < TABLE_SIZE; i++) {
		x = (1.0 - (double)i / (double)(TABLE_SIZE - 1)) * t0;
		y = pow(2, a * x * x + b);
		printf("%i, ", (int)floor(y + 0.5));
	}
	return 0;
}
#endif

#define INTERP_LOG    3
#define INTERP_FACTOR (1 << INTERP_LOG)

static const PROGMEM uint8_t smaul_freq[] = {
		200, 144, 106, 79, 60, 46, 36, 29, 23, 19, 16, 13, 11, 10, 9, 8, 7, 7,
};

void smaul_pulse_update(void)
{
	int16_t min;

	/* Don't do anything if we have an expired timer or key error */
	if (ui_flags)
		return;

	min = getMinimumTimer(MAX_KEYS + NUM_PIZZA_TIMERS);
	if (min < 0) {
		smaul_off();
	} else if (min >= ((ARRAY_SIZE(smaul_freq) - 1) * INTERP_FACTOR)) {
		smaul_pulse(6);
	} else {
		uint8_t part1 = min & (INTERP_FACTOR - 1);
		uint8_t part0 = INTERP_FACTOR - part1;
		uint8_t idx = min >> INTERP_LOG;
		smaul_pulse(((uint16_t)pgm_read_byte(smaul_freq + idx) * part0 +
				     (uint16_t)pgm_read_byte(smaul_freq + idx + 1) * part1) >> INTERP_LOG);
	}
}

void keytimer_display_update(void)
{
	lcd_print_start(1);
	print_time(keyTimers[MAX_KEYS + 0]);
	print_time(keyTimers[MAX_KEYS + 1]);
	print_time(keyTimers[MAX_KEYS + 2]);
	print_time(getMinimumTimer(MAX_KEYS));
	lcd_print_end(1);
}

static void ui_repaint(void) {
	uint8_t n;

	switch (ui_state) {
	case UIS_IDLE:
	case UIS_MESSAGE_TIMEOUT:
	case UIS_KEY_ERROR:
		keytimer_display_update();
		break;

	case UIS_MENU_PIZZA1:
	case UIS_MENU_PIZZA2:
	case UIS_MENU_PIZZA3:
		n = ui_state - UIS_MENU_PIZZA1;
		if (pizzatimer_running(n) != 0) {
			lcd_printfP(0, PSTR("Pizzatimer %d Off"), n + 1);
		} else {
			lcd_printfP(0, PSTR("Pizzatimer %d"), n + 1);
		}
		break;

	case UIS_MENU_FIND_KEY:
		lcd_printfP(0, PSTR("Locate key"));
		break;

	case UIS_MENU_BOOTLOADER:
		lcd_printfP(0, PSTR("Enter bootloader"));
		break;

	case UIS_SELECT_TIME:
		lcd_printfP(1, PSTR("%02i minutes"), selected_time);
		break;

	case UIS_FIND_KEY:
		if (keys[selected_key].state == KS_VALID)
			lcd_printfP(1, PSTR("%s"), keys[selected_key].eep.key.name);
		else
			lcd_printfP(1, (keys[selected_key].state == KS_EMPTY) ? PSTR("No key plugged") : PSTR("Read error"));
		keyled_on(selected_key);
		break;
	}
}

/**
 * this method is used internally to reset the timer for ending the menu mode.
 */
static void reset_ui_timer(void) {
	switch (ui_state) {
	case UIS_MENU_PIZZA1:
	case UIS_MENU_PIZZA2:
	case UIS_MENU_PIZZA3:
	case UIS_MENU_FIND_KEY:
	case UIS_MENU_BOOTLOADER:
		ui_timer = MENU_TIMEOUT_SECONDS;
		break;

	case UIS_SELECT_TIME:
	case UIS_FIND_KEY:
		ui_timer = MENU_TIMEOUT_SELECT_SECONDS;
		break;

	case UIS_MESSAGE_TIMEOUT:
		ui_timer = UI_MESSAGE_TIMEOUT_SECONDS;
		break;

	default:
		ui_timer = 0;
		break;
	}
}

static void ui_default_state(void) {
	keyleds_off();
	beeper_stop();
	smaul_off();
	enable_lcd_backlight();

	if (ui_flags & UIF_KEY_ERROR) {
		ui_state = UIS_KEY_ERROR;
		keyled_blink(error_slot);
		beeper_start(BEEP_ERROR);

		switch (ui_flags & UIF_KEY_ERROR) {
		case UIF_KEY_ERROR_READ_ERR:
			lcd_printfP(0, PSTR("Read error in slot %d"), error_slot + 1);
			break;
		case UIF_KEY_ERROR_UNKNOWN:
			lcd_printfP(0, PSTR("Unknown key %d (\"%s\")"), keys[error_slot].eep.key.id, keys[error_slot].eep.key.name);
			break;
		case UIF_KEY_ERROR_OTHER_KB:
			lcd_printfP(0, PSTR("Invalid key; belongs to %s"), keys[error_slot].eep.kb.name);
			break;
		}
	} else {
		ui_state = UIS_IDLE;

		if (!(ui_flags & UIF_TIMER_EXPIRED)) {
			lcd_printfP(0, PSTR(""));
		} else {
			smaul_blink(220);
			if (expired_timer < MAX_KEYS) {
				beeper_start(BEEP_KEYMISSING);
				lcd_printfP(0, PSTR("Key %s missing"), config.keys[expired_timer].name);
			} else {
				uint8_t n = expired_timer - MAX_KEYS;
				beeper_start(BEEP_PIZZA1 + n);
				lcd_printfP(0, PSTR("Pizza %d done"), n + 1);
			}
		}
	}
}

void ui_message(uint8_t dest_state)
{
	if (ui_state == UIS_FIND_KEY)
		keyleds_off();

	ui_state = dest_state;
	enable_lcd_backlight();
	reset_ui_timer();
}

void ui_set_timer_expired(uint8_t timer_idx)
{
	if ((ui_flags & UIF_TIMER_EXPIRED) && expired_timer == timer_idx)
		return;

	expired_timer = timer_idx;
	ui_flags |= UIF_TIMER_EXPIRED;
	ui_default_state();
}

void ui_clear_timer_expired(void)
{
	ui_flags &= ~UIF_TIMER_EXPIRED;
	ui_default_state();
}

void ui_set_key_error(uint8_t error_type, uint8_t slot_idx)
{
	if ((ui_flags & UIF_KEY_ERROR) == error_type && error_slot == slot_idx)
		return;

	ui_flags = (ui_flags & ~UIF_KEY_ERROR) | error_type;
	error_slot = slot_idx;
	ui_default_state();
}

void ui_clear_key_error(void)
{
	ui_flags &= ~UIF_KEY_ERROR;
	ui_default_state();
}

static void apply_timer(void) {
	setKeyTimeout(selected_key, selected_time);
	ui_default_state();
}

static void count_ui_timer(void) {
	if (ui_timer && !(--ui_timer)) {
		if (ui_state == UIS_SELECT_TIME) {
			apply_timer();
		} else {
			ui_default_state();
		}
	}
}

static void menu_activate(void) {
	uint8_t n;

	switch (ui_state) {
	// enable the menu;
	case UIS_IDLE:
	case UIS_MESSAGE_TIMEOUT:
		ui_state = UIS_MENU_FIND_KEY;
		reset_ui_timer();
		lcd_printfP(1, PSTR(""));
		break;

	case UIS_MENU_PIZZA1:
	case UIS_MENU_PIZZA2:
	case UIS_MENU_PIZZA3:
		n = ui_state - UIS_MENU_PIZZA1;
		if (pizzatimer_running(n) != 0) {
			pizzatimer_clear(n);
			ui_default_state();
		} else {
			ui_select_time(MAX_KEYS + n, PIZZA_TIMER_DEFAULT_TIME, PIZZA_TIMER_MAX_TIME);
		}
		break;

	case UIS_MENU_FIND_KEY:
		ui_state = UIS_FIND_KEY;
		selected_key = 0;
		break;

	case UIS_MENU_BOOTLOADER:
		call_bootloader();
		break;

	case UIS_SELECT_TIME:
		apply_timer();
		break;

	case UIS_FIND_KEY:
		ui_default_state();
		break;
	}
}

static void menu_button_forward(void) {
	switch (ui_state) {
	case UIS_MENU_FIND_KEY:
	case UIS_MENU_PIZZA1:
	case UIS_MENU_PIZZA2:
	case UIS_MENU_PIZZA3:
		ui_state++;
		break;

	case UIS_MENU_BOOTLOADER:
		ui_state = UIS_MENU_FIND_KEY;
		break;

	case UIS_SELECT_TIME:
		selected_time = min(max_selectable_time, selected_time + 1);
		break;

	case UIS_FIND_KEY:
		if (selected_key == MAX_KEYS - 1)
			selected_key = 0;
		else
			selected_key++;
		break;
	}
}

static void menu_button_back(void) {
	switch (ui_state) {
	case UIS_MENU_FIND_KEY:
		ui_state = UIS_MENU_BOOTLOADER;
		break;

	case UIS_MENU_PIZZA1:
	case UIS_MENU_PIZZA2:
	case UIS_MENU_PIZZA3:
	case UIS_MENU_BOOTLOADER:
		ui_state--;
		break;

	case UIS_SELECT_TIME:
		selected_time = max(1, selected_time - 1);
		break;

	case UIS_FIND_KEY:
		if (selected_key == 0)
			selected_key = MAX_KEYS - 1;
		else
			selected_key--;
		break;
	}
}

static void menu_button_smaul(void) {
	switch (ui_state) {
	case UIS_IDLE:
		key_smaul();
		break;
	case UIS_KEY_ERROR:
		// ignore
		break;
	default:
		ui_default_state();
		break;
	}
}

#ifndef __NO_INCLUDE_AVR

void ui_poll(void)
{
	uint8_t event = get_event();

	if (event == EV_NONE)
		return;

	switch (event) {
	case EV_ENCODER_CW:
		menu_button_forward();
		break;
	case EV_ENCODER_CCW:
		menu_button_back();
		break;
	case EV_ENCODER_PUSH:
		menu_activate();
		break;
	case EV_SMAUL_PUSH:
		menu_button_smaul();
		break;
	case EV_TICK:
		key_timer();
		count_ui_timer();
		smaul_pulse_update();
		break;
	case EV_KEY_CHANGE:
		key_change();
		break;
	}

	if (event != EV_TICK && event != EV_KEY_CHANGE) {
		enable_lcd_backlight();
		reset_ui_timer();
	}

	ui_repaint();
}

void ui_select_time(uint8_t timer_id, uint8_t default_time, uint8_t max_time)
{
	if (ui_state == UIS_FIND_KEY)
		keyleds_off();

	selected_time = default_time;
	max_selectable_time = max_time;
	selected_key = timer_id;
	ui_state = UIS_SELECT_TIME;
	enable_lcd_backlight();
	reset_ui_timer();
	ui_repaint();
}

void ui_init(void)
{
	initTimers();
}

#endif // __NO_INCLUDE_AVR
