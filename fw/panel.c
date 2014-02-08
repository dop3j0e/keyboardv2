#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>

#include "hw.h"
#include "panel.h"
#include "common.h"

struct shiftregs shiftregs = {
	0, 1, 0, 0, 0, 0
};

#define shiftreg_bytes ((uint8_t *)&shiftregs)

uint8_t shiftreg_state;

ISR(SPI_STC_vect)
{
	shiftreg_state++;
	if (shiftreg_state < 2)
		SPDR = shiftreg_bytes[shiftreg_state];
	else {
		SHIFTREG_LATCH = 1;
		SPCR = 0;
		SHIFTREG_LATCH = 0;
	}
}

#define SPI_SETTINGS (1 << SPE) | (0 << DORD) | (1 << MSTR) | (0 << CPOL) | (0 << CPHA) | (2 << SPR0)

void shiftreg_reset(void)
{
	uint8_t dummy __attribute__((unused));

	SPSR = 0;
	SPCR = SPI_SETTINGS;
	SPDR = 0;
	while (!(SPSR & (1 << SPIF)));
	SPDR = 0;
	while (!(SPSR & (1 << SPIF)));
	dummy = SPDR; // read SPDR to clear SPIF
	SHIFTREG_LATCH = 1;
	SPCR = 0;
	SHIFTREG_LATCH = 0;
}

void shiftreg_update(void)
{
	SPSR = 0;
	SPCR = (1 << SPIE) | SPI_SETTINGS;
	shiftreg_state = 0;
	SPDR = shiftreg_bytes[0];
}

uint8_t inputs_prev = 0, inputs_debounced = 0, inputs_debounced_prev = 0;

#define IN_MASKB (IN_ROTA | IN_ROTB | IN_PUSH)
#define IN_MASKE IN_SMAUL

static void poll_inputs(void)
{
	uint8_t inputs = (PINB & IN_MASKB) | (PINE & IN_MASKE);
	uint8_t debounce_low = inputs | inputs_prev;
	uint8_t debounce_high = inputs & inputs_prev;
	inputs_prev = inputs;

	inputs_debounced |= debounce_high;
	inputs_debounced &= debounce_low;

	/* Here's the trick for the rotary encoder:
	 *
	 * Due to the detents, the first half of the quadrature cycle (where you have to invest force to overcome the current detent)
	 * takes way longer than the second half (where the knob snaps into the next detent without outside help).
	 *
	 * The result is that depending on direction, the signal edges on either A or B are further apart than on the other signal.
	 * So when we look for an edge, we're better off checking for edges on both signals instead of just looking for the edge on
	 * one signal and deriving the direction from the other signal.
	 */
	if ((inputs_debounced_prev & IN_ROTA) && !(inputs_debounced & IN_ROTA) && (inputs_debounced_prev & IN_ROTB))
		push_event(EV_ENCODER_CW);
	else if ((inputs_debounced_prev & IN_ROTB) && !(inputs_debounced & IN_ROTB) && (inputs_debounced_prev & IN_ROTA))
		push_event(EV_ENCODER_CCW);

	if ((inputs_debounced_prev & IN_PUSH) && !(inputs_debounced & IN_PUSH))
		push_event(EV_ENCODER_PUSH);

	if ((inputs_debounced_prev & IN_SMAUL) && !(inputs_debounced & IN_SMAUL))
		push_event(EV_SMAUL_PUSH);

	inputs_debounced_prev = inputs_debounced;
}

#define BEEPER_TICK_LENGTH 30

static uint8_t beeper_counter, beeper_tick;
static volatile uint8_t beeper_state;

static void beeper_set(uint8_t on)
{
	//set_smaul_led(on ? 255 : 0);
	shiftregs.beeper = on;
	shiftreg_update();
}

static void beeper_update(void)
{
	uint8_t local_state;

	beeper_counter--;
	if (beeper_counter)
		return;

	beeper_counter = BEEPER_TICK_LENGTH;
	beeper_tick++;

	local_state = beeper_state;
	switch (local_state) {
	case BEEP_OFF:
		break;
	case BEEP_SINGLE:
		if (beeper_tick == 5) {
			beeper_set(0);
			beeper_state = BEEP_OFF;
		}
		break;
	case BEEP_KEYMISSING:
		if ((beeper_tick & 15) == 0)
			beeper_set(!(beeper_tick & 16));
		break;
	case BEEP_PIZZA1:
	case BEEP_PIZZA2:
	case BEEP_PIZZA3:
		if (beeper_tick < 12) {
			if ((beeper_tick & 1) == 0)
				beeper_set(!(beeper_tick & 2));
		} else if (beeper_tick < (12 + (local_state - BEEP_PIZZA1 + 1) * 16)) {
			if (((beeper_tick - 12) & 7) == 0)
				beeper_set(!((beeper_tick - 12) & 8));
		} else if (beeper_tick == (80 + (local_state - BEEP_PIZZA1 + 1) * 16)) {
			beeper_set(1);
			beeper_tick = 0;
		}
		break;
	case BEEP_ERROR:
		if (((beeper_tick & 48) == 0) && ((beeper_tick & 1) == 0))
				beeper_set(!(beeper_tick & 2));
		break;
	}
}

void beeper_start(enum beep_patterns pattern)
{
	beeper_state = pattern;
	beeper_counter = BEEPER_TICK_LENGTH;
	beeper_tick = 0;

	beeper_set(pattern != BEEP_OFF);
}

enum lcd_led_state {
	LCD_NONE = 0,
	LCD_BRIGHT,
	LCD_DARK,
};

enum smaul_led_state {
	SMAUL_OFF = 0,
	SMAUL_PULSE,
	SMAUL_BLINK,
};

#define LCD_LED_DIM  13
#define LCD_LED_ON   255
#define LCD_LED_UP   42
#define LCD_LED_DOWN 3

volatile uint8_t global_ms_timer;
volatile uint8_t global_qs_timer;

static uint8_t lcd_led_brightness = LCD_LED_DIM;
static uint8_t smaul_led_osc = 0;
static volatile uint8_t lcd_led_state = LCD_NONE;
static volatile uint8_t smaul_led_state = SMAUL_OFF;
static volatile uint8_t smaul_led_frequency;

#if 0
/* Generate gamma table for smaul LED driver */
#include <stdio.h>
#include <math.h>

int main(void)
{
	int i;
	double x, y;
	for (i = 0; i < 64; i++) {
		x = (double)i / 63.0;
		y = pow(x, 2.2);
		printf("%i, ", (int)floor((y * 255.0) + 0.5));
	}
	return 0;
}
#endif

static const uint8_t gamma[64] = {
		0, 0, 0, 0, 1, 1, 1, 2, 3, 4, 4, 5, 7, 8, 9, 11, 13, 14, 16, 18, 20, 23, 25, 28, 31, 33, 36, 40, 43, 46, 50, 54, 57, 61,
		66, 70, 74, 79, 84, 89, 94, 99, 105, 110, 116, 122, 128, 134, 140, 147, 153, 160, 167, 174, 182, 189, 197, 205, 213, 221,
		229, 238, 246, 255,
};

static void pwmled_update(void)
{
	uint8_t smaul_led_state_copy = smaul_led_state;

	if (lcd_led_state == LCD_NONE && smaul_led_state_copy == SMAUL_OFF)
		return;

	if (global_ms_timer & 15)
		return;

	switch (lcd_led_state) {
	case LCD_BRIGHT:
		if (lcd_led_brightness < LCD_LED_ON - LCD_LED_UP) {
			lcd_led_brightness += LCD_LED_UP;
		} else {
			lcd_led_brightness = LCD_LED_ON;
			lcd_led_state = LCD_NONE;
		}
		set_lcd_led(lcd_led_brightness);
		break;
	case LCD_DARK:
		if (lcd_led_brightness > LCD_LED_DIM + LCD_LED_DOWN) {
			lcd_led_brightness -= LCD_LED_DOWN;
		} else {
			lcd_led_brightness = LCD_LED_DIM;
			lcd_led_state = LCD_NONE;
		}
		set_lcd_led(lcd_led_brightness);
		break;
	}

	if (smaul_led_state_copy != SMAUL_OFF) {
		smaul_led_osc += smaul_led_frequency;
		if (smaul_led_state_copy == SMAUL_BLINK) {
			set_smaul_led((smaul_led_osc & 128) ? 255 : 0);
		} else {
			uint8_t brightness = (smaul_led_osc >> 1) & 63;
			set_smaul_led((smaul_led_osc & 128) ? gamma[brightness] : gamma[63 - brightness]);
		}
	}
}

void set_lcd_backlight(uint8_t on)
{
	lcd_led_state = on ? LCD_BRIGHT : LCD_DARK;
}

void smaul_pulse(uint8_t frequency)
{
	smaul_led_frequency = frequency;
	smaul_led_state = SMAUL_PULSE;
}

void smaul_blink(uint8_t frequency)
{
	smaul_led_frequency = frequency;
	smaul_led_state = SMAUL_BLINK;
}

void smaul_off(void)
{
	smaul_led_state = SMAUL_OFF;
	set_smaul_led(0);
}

static volatile uint8_t led_blink_mask = 0;

static void keyleds_update(void)
{
	uint8_t led_blink_mask_copy = led_blink_mask;
	if (led_blink_mask_copy && (global_qs_timer & 1)) {
		shiftregs.leds ^= led_blink_mask_copy;
		shiftreg_update();
	}
}

void keyled_on(uint8_t which)
{
	led_blink_mask = 0;
	shiftregs.leds = 1 << which;
	shiftreg_update();
}

void keyled_blink(uint8_t which)
{
	shiftregs.leds = 0;
	led_blink_mask = 1 << which;
}

void keyleds_off(void)
{
	led_blink_mask = 0;
	shiftregs.leds = 0;
	shiftreg_update();
}

/* Use timer/counter 3 as system tick source because
 *  a) it has lower interrupt priority than T/C0 which is used for one-wire communication
 *  b) it has only one PWM pin connected to package pins
 */
ISR(TIMER3_OVF_vect)
{
	poll_inputs();
	beeper_update();
	pwmled_update();
	global_ms_timer++;
	if (!global_ms_timer) {
		global_qs_timer++;
		keyleds_update();
		if ((global_qs_timer & 3) == 0)
			push_event(EV_TICK);
	}
}