#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

#include "hw.h"
#include "panel.h"
#include "common.h"
#include "lcd_drv.h"

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

static void shiftreg_reset(void)
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
	cli();
	SPCR = 0;
	SPSR = 0;
	shiftreg_state = 0;
	SPCR = (1 << SPIE) | SPI_SETTINGS;
	SPDR = shiftreg_bytes[0];
	sei();
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

static uint8_t sync_smaul_to_beeper = 0, beeper_counter, beeper_tick;
static volatile uint8_t beeper_state;

static void beeper_set(uint8_t on)
{
	if (sync_smaul_to_beeper)
		set_smaul_led(on ? 255 : 0);
	cli();
	shiftregs.beeper = on;
	sei();
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
	case BEEP_DISABLED:
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
	if (beeper_state == BEEP_DISABLED)
		return;

	beeper_state = pattern;
	beeper_counter = BEEPER_TICK_LENGTH;
	beeper_tick = 0;

	beeper_set(pattern != BEEP_OFF);
}

void beeper_enable(uint8_t enable)
{
	if (enable) {
		beeper_state = BEEP_OFF;
	} else {
		beeper_stop();
		beeper_state = BEEP_DISABLED;
	}
}

#define ROTLIGHT_ON_SECS     30
#define ROTLIGHT_OFF_SECS 15*60

static uint8_t rotlight_active = 0;
static uint16_t rotlight_timer;

static void rotlight_update(void)
{
	if (rotlight_active) {
		rotlight_timer++;
		if (rotlight_timer == ROTLIGHT_ON_SECS) {
			cli();
			shiftregs.rotlight = 0;
			sei();
			shiftreg_update();
		} else if (rotlight_timer == ROTLIGHT_ON_SECS + ROTLIGHT_OFF_SECS) {
			rotlight_timer = 0;
			cli();
			shiftregs.rotlight = 1;
			sei();
			shiftreg_update();
		}
	}
}

void rotlight_on(void)
{
	cli();
	shiftregs.rotlight = 1;
	sei();
	shiftreg_update();
	rotlight_timer = 0;
	rotlight_active = 1;
}

void rotlight_off(void)
{
	rotlight_active = 0;
	cli();
	shiftregs.rotlight = 0;
	sei();
	shiftreg_update();
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
static uint16_t smaul_led_osc = 0;
static volatile uint8_t lcd_led_state = LCD_NONE;
static volatile uint8_t lcd_led_timer = 0;
static volatile uint8_t smaul_led_state = SMAUL_OFF;
static volatile uint8_t smaul_led_frequency;

/* generated by gen_tables.c */
static const PROGMEM uint8_t gamma[64] = {
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
			set_smaul_led((smaul_led_osc & 2048) ? 0 : 255);
		} else {
			uint8_t brightness = (smaul_led_osc >> 5) & 63;
			set_smaul_led((smaul_led_osc & 2048) ? pgm_read_byte(gamma + 63 - brightness) :
					pgm_read_byte(gamma + brightness));
		}
	}
}

void enable_lcd_backlight(void)
{
	if (!lcd_led_timer)
		lcd_led_state = LCD_BRIGHT;
	lcd_led_timer = LCD_BACKLIGHT_TIMEOUT_SECS;
}

void smaul_pulse(uint8_t frequency)
{
	if (smaul_led_state != SMAUL_PULSE)
		smaul_led_osc = 0;
	sync_smaul_to_beeper = 0;
	smaul_led_frequency = frequency;
	smaul_led_state = SMAUL_PULSE;
}

void smaul_blink(uint8_t frequency)
{
	if (smaul_led_state != SMAUL_BLINK)
		smaul_led_osc = 0;
	sync_smaul_to_beeper = 0;
	smaul_led_frequency = frequency;
	smaul_led_state = SMAUL_BLINK;
}

void smaul_sync_to_beeper(void)
{
	smaul_led_state = SMAUL_OFF;
	sync_smaul_to_beeper = 1;
}

void smaul_off(void)
{
	sync_smaul_to_beeper = 0;
	smaul_led_state = SMAUL_OFF;
	set_smaul_led(0);
}

static volatile uint8_t led_blink_mask = 0;

static void keyleds_update(void)
{
	uint8_t led_blink_mask_copy = led_blink_mask;
	if (led_blink_mask_copy && (global_qs_timer & 1)) {
		cli();
		shiftregs.leds ^= led_blink_mask_copy;
		sei();
		shiftreg_update();
	}
}

void keyled_on(uint8_t which)
{
	led_blink_mask = 0;
	cli();
	shiftregs.leds = 1 << which;
	sei();
	shiftreg_update();
}

void keyled_blink(uint8_t which)
{
	cli();
	shiftregs.leds = 0;
	sei();
	led_blink_mask = 1 << which;
}

void keyleds_off(void)
{
	led_blink_mask = 0;
	cli();
	shiftregs.leds = 0;
	sei();
	shiftreg_update();
}

#define LCD_WIDTH 16
#define MAX_LCD_LINE1 (MAX_KEYS + 1) * (NAME_LENGTH + 2)
#define MAX_LCD_LINE2 LCD_WIDTH + 1
#define SCROLL_NUM_SPACES 3
#define SCROLL_SPEED 3
#define SCROLL_DELAY 2

static volatile uint8_t max_line_length(uint8_t line)
{
	return (line == 0) ? MAX_LCD_LINE1 : MAX_LCD_LINE2;
}

static volatile uint8_t lcd_writing = 0;
static volatile uint8_t lcd_needs_update = 0;
struct lcd_line {
	char *text;
	uint8_t len, pos, delay;
};
static char lcd_line1[MAX_LCD_LINE1 + SCROLL_NUM_SPACES + LCD_WIDTH];
static char lcd_line2[MAX_LCD_LINE2];
static struct lcd_line lcd_lines[2] = { { lcd_line1 }, { lcd_line2 } };

void lcd_print_start(uint8_t line)
{
	lcd_writing = 1;
	lcd_lines[line].len = 0;
}

void lcd_print_update_P(uint8_t line, const char *fmt, ...)
{
	va_list varargs;
	struct lcd_line *l = lcd_lines + line;

	if (l->len >= max_line_length(line) - 1)
		return;

	va_start(varargs, fmt);
	l->len += vsnprintf_P(l->text + l->len, max_line_length(line) - l->len, fmt, varargs);
	va_end(varargs);
}

void lcd_print_end(uint8_t line)
{
	struct lcd_line *l = lcd_lines + line;

	l->pos = 0;
	l->delay = SCROLL_DELAY;
	if (l->len > LCD_WIDTH) {
		memset(l->text + l->len, ' ', SCROLL_NUM_SPACES);
		l->len += SCROLL_NUM_SPACES;
		memcpy(l->text + l->len, l->text, LCD_WIDTH);
		l->text[l->len + LCD_WIDTH] = 0;
	} else if (l->len < LCD_WIDTH) {
		memset(l->text + l->len, ' ', LCD_WIDTH - l->len);
		l->text[LCD_WIDTH] = 0;
	}

	lcd_writing = 0;
	lcd_needs_update = 1;
}

void lcd_printfP(uint8_t line, const char *fmt, ...)
{
	va_list varargs;

	lcd_print_start(line);
	va_start(varargs, fmt);
	lcd_lines[line].len = vsnprintf_P(lcd_lines[line].text, max_line_length(line), fmt, varargs);
	va_end(varargs);
	lcd_print_end(line);
}

static void lcd_scroll(void)
{
	struct lcd_line *line;

	for (line = lcd_lines; line < (lcd_lines + ARRAY_SIZE(lcd_lines)); line++) {
		if (line->len > LCD_WIDTH) {
			if (line->delay) {
				line->delay--;
				continue;
			}

			line->pos += SCROLL_SPEED;
			if (line->pos >= line->len)
				line->pos -= line->len;
			lcd_needs_update = 1;
		}
	}
}

void lcd_poll(void)
{
	struct lcd_line *line;
	uint8_t i;

	if (!lcd_needs_update)
		return;

	lcd_xy(0, 0);
	for (line = lcd_lines; line < (lcd_lines + ARRAY_SIZE(lcd_lines)); line++) {
		for (i = 0; i < LCD_WIDTH; i++)
			lcd_putchar(line->text[line->pos + i]);
	}

	lcd_needs_update = 0;
}

/* Use timer/counter 3 as system tick source because
 *  a) it has lower interrupt priority than T/C0 which is used for one-wire communication
 *  b) it has only one PWM pin connected to package pins
 */
ISR(TIMER3_OVF_vect)
{
	sei(); /* Allow other ints, like onewire int, to interrupt this. */
	poll_inputs();
	beeper_update();
	pwmled_update();
	global_ms_timer++;
	if (!global_ms_timer) {
		global_qs_timer++;
		keyleds_update();
		if ((global_qs_timer & 1) == 0 && !lcd_writing)
			lcd_scroll();
		if ((global_qs_timer & 3) == 0) {
			rotlight_update();
			push_event(EV_TICK);
			if (lcd_led_timer && !(--lcd_led_timer))
				lcd_led_state = LCD_DARK;
		}
	}
}

void panel_init(void)
{
	lcd_printfP(0, PSTR(""));
	lcd_printfP(1, PSTR(""));

	shiftreg_reset();

	// Set up T/C 1 for 8-bit fast PWM running at F_CPU/256 (64kHz), resulting in a PWM period of 250 Hz
	// Also, use inverted PWM so it's possible to turn the pin off completely
	set_lcd_led(LCD_LED_DIM);
	set_smaul_led(0);
	TCCR1A = (1 << WGM10) | (3 << COM1A0) | (3 << COM1B0) | (0 << COM1C0);
	TCCR1B = (1 << WGM12) | (4 << CS10);

	/* Set up T/C3 to run at CLK/64 and do 8-bit PWM, leading to an OCR int at F_CPU / 16k, i.e. roughly 1kHz */
	TCNT3 = 0;
	TIMSK3 = 1 << TOIE3;
	TIFR3  = 1 << TOV3;
	TCCR3A = 1 << WGM30;
	TCCR3B = 1 << WGM32 | 3 << CS30;
}
