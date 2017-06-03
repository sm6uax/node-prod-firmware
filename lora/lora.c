/* LoRa task */

#include <stdint.h>
#include <stdio.h>

#include "ble.h"
#include "hw/led.h"
#include "lmic/lmic.h"
#include "lora/ad_lora.h"
#include "lora/lora.h"
#include "lora/param.h"
#include "lora/proto.h"
#include "lora/upgrade.h"
#include "sensor/sensor.h"

#define DEBUG
//#define HELLO
//#define BLE_ALWAYS_ON

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof(*x))

#define STATE_JOINING		0
#define STATE_JOINED		1
#define STATE_SAMPLING_SENSOR	2
#define STATE_WAITING_TO_SEND	3
#define STATE_SENDING		4
PRIVILEGED_DATA static uint8_t	state;

#define MAX_SENSOR_SAMPLE_TIME	sec2osticks(2)
PRIVILEGED_DATA static ostime_t	sampling_since;

#define JOIN_TIMEOUT		sec2osticks(2 * 60 * 60)
#define TX_TIMEOUT		sec2osticks(8)
#define TX_PERIOD_TIMEOUT	sec2osticks(10 * 60)

#define MAX_RESETS		8

#ifdef DEBUG
static void
debug_event(int ev)
{
	static const char	*evnames[] = {
		[EV_SCAN_TIMEOUT]	= "SCAN_TIMEOUT",
		[EV_BEACON_FOUND]	= "BEACON_FOUND",
		[EV_BEACON_MISSED]	= "BEACON_MISSED",
		[EV_BEACON_TRACKED]	= "BEACON_TRACKED",
		[EV_JOINING]		= "JOINING",
		[EV_JOINED]		= "JOINED",
		[EV_RFU1]		= "RFU1",
		[EV_JOIN_FAILED]	= "JOIN_FAILED",
		[EV_REJOIN_FAILED]	= "REJOIN_FAILED",
		[EV_TXCOMPLETE]		= "TXCOMPLETE",
		[EV_LOST_TSYNC]		= "LOST_TSYNC",
		[EV_RESET]		= "RESET",
		[EV_RXCOMPLETE]		= "RXCOMPLETE",
		[EV_LINK_DEAD]		= "LINK_DEAD",
		[EV_LINK_ALIVE]		= "LINK_ALIVE",
		[EV_SCAN_FOUND]		= "SCAN_FOUND",
		[EV_TXSTART]		= "TXSTART",
	};

	printf("%s\r\n", ev < ARRAY_SIZE(evnames) ? evnames[ev] : "UNKNOWN");
}
#else
#define debug_event(ev)
#endif

static void
lora_start_joining(osjob_t *job)
{
	if (!cm_lp_clk_is_avail()) {
		os_setTimedCallback(job, os_getTime() + sec2osticks(1),
		    lora_start_joining);
		return;
	}
	LMIC_startJoining();
}

static void
lora_reset(osjob_t *job)
{
	PRIVILEGED_DATA static uint8_t	reset_count;

#ifdef DEBUG
	printf("lora reset #%d\r\n", reset_count);
#endif
	if (++reset_count > MAX_RESETS)
		hal_failed();
	LMIC_reset();
	os_setCallback(job, lora_start_joining);
}

static void
lora_reset_after(ostime_t delay)
{
	PRIVILEGED_DATA static osjob_t	reset_job;

	os_setTimedCallback(&reset_job, os_getTime() + delay, lora_reset);
}

#define lora_init()	lora_reset_after(0)

static void	lora_prepare_sensor_data(osjob_t *job);

static void
lora_wait_for_sensor_data(osjob_t *job)
{
	ostime_t	delay;

	if (os_getTime() < sampling_since + MAX_SENSOR_SAMPLE_TIME &&
	    (delay = sensor_data_ready()) != 0) {
		os_setTimedCallback(job, os_getTime() + delay,
		    lora_wait_for_sensor_data);
		ad_lora_suspend_sleep(LORA_SUSPEND_LORA, delay + 64);
	} else {
		state = STATE_WAITING_TO_SEND;
		led_notify(LED_STATE_IDLE);
		proto_send_data();
		os_setTimedCallback(job, os_getTime() + sensor_period(),
		    lora_prepare_sensor_data);
	}
}

static void
lora_prepare_sensor_data(osjob_t *job)
{
	if (state != STATE_JOINED)
		return;
	state = STATE_SAMPLING_SENSOR;
	sampling_since = os_getTime();
	led_notify(LED_STATE_SAMPLING_SENSOR);
	sensor_prepare();
	lora_wait_for_sensor_data(job);
}

void
lora_send_data(void)
{
	PRIVILEGED_DATA static osjob_t	sensor_job;

	lora_prepare_sensor_data(&sensor_job);
}

void
onEvent(ev_t ev)
{
	debug_event(ev);
	switch(ev) {
	case EV_JOINING:
	case EV_LINK_DEAD:
		state = STATE_JOINING;
		led_notify(LED_STATE_JOINING);
		lora_reset_after(JOIN_TIMEOUT);
		break;
	case EV_JOINED:
#ifdef DEBUG
		printf("netid = %lu\r\n", LMIC.netid);
#endif
		/* FALLTHROUGH */
	case EV_LINK_ALIVE:
		state = STATE_JOINED;
		lora_reset_after(TX_PERIOD_TIMEOUT);
		lora_send_data();
		break;
	case EV_TXSTART:
		ad_lora_suspend_sleep(LORA_SUSPEND_LORA, TX_TIMEOUT);
		proto_txstart();
		if (state == STATE_JOINING) {
			led_notify(LED_STATE_JOINING);
		} else {
			state = STATE_SENDING;
			led_notify(LED_STATE_SENDING);
			lora_reset_after(TX_TIMEOUT);
		}
		break;
	case EV_TXCOMPLETE:
		{
			ostime_t	delay;

			delay = sensor_period() + sec2osticks(5);
			if (delay < TX_PERIOD_TIMEOUT)
				delay = TX_PERIOD_TIMEOUT;
			lora_reset_after(delay);
		}
		if (LMIC.dataLen != 0) {
			proto_handle(LMIC.frame[LMIC.dataBeg - 1],
			    LMIC.frame + LMIC.dataBeg, LMIC.dataLen);
		}
		state = STATE_JOINED;
		led_notify(LED_STATE_IDLE);
		ad_lora_allow_sleep(LORA_SUSPEND_LORA);
		break;
	default:
		break;
	}
}

#ifdef HELLO
static void
say_hi(osjob_t *job)
{
	PRIVILEGED_DATA static int	n;
	char				buf[64];

	os_setTimedCallback(job, os_getTime() + sec2osticks(1), say_hi);
	printf("Hello #%u @ %u (%ld)\r\n", n++, os_getTimeSecs(), os_getTime());
}
#endif

void
lora_task_func(void *param)
{
#ifdef HELLO
	osjob_t	hello_job;
#endif

	param_init();
	ad_lora_init();
	os_init();
	led_notify(LED_STATE_BOOTING);
	upgrade_init();
#ifdef BLE_ALWAYS_ON
	ble_on();
#endif
#ifdef HELLO
	os_setCallback(&hello_job, say_hi);
#endif
	lora_init();
	os_runloop();
}
