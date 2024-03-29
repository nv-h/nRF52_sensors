/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <kernel.h>
#include <zephyr.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(sensor);

#include "battery.h"
#include "qmp6988.h"
#include "sgp30.h"
#include "sht3x.h"

#include "app_bt.h"

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 5000

// NOTE: SGP30 is very high comsumption (about 50 mA), so disable it
#define CONFIG_SGP30

struct sensors {
    const struct device *i2c_dev;
    sht3x_sensor_t      *sht3x;
    qmp6988_sensor_t    *qmp6988;
#ifdef CONFIG_SGP30
    sgp30_sensor_t *sgp30;
#endif
    float temperature;
    float humidity;
    float pressure;
    float temperature_p;
};

typedef struct {
    int   battery_mV;
    float temperature;
    float humidity;
    float pressure;
#ifdef CONFIG_SGP30
    uint16_t co2;
    uint16_t tvoc;
#endif
} send_data_t;

static struct sensors env_sensors = {
    .i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0)),
    .sht3x   = NULL,
    .qmp6988 = NULL,
#ifdef CONFIG_SGP30
    .sgp30 = NULL,
#endif
};

static struct gpio_dt_spec led0_spec = GPIO_DT_SPEC_GET(DT_NODELABEL(led0), gpios);

static send_data_t send_data = {
    .battery_mV  = 0,
    .temperature = 0,
    .humidity    = 0,
    .pressure    = 0,
#ifdef CONFIG_SGP30
    .co2  = 0,
    .tvoc = 0,
#endif
};

static bool init_i2c_sensors(struct sensors *env_sensors);
static void get_i2c_sensors_values(struct sensors *env_sensors);
static void app_work_handler(struct k_work *work);
static void app_timer_handler(struct k_timer *dummy);

K_WORK_DEFINE(app_work, app_work_handler);
K_TIMER_DEFINE(app_timer, app_timer_handler, NULL);

static void app_work_handler(struct k_work *work)
{
    gpio_pin_set_dt(&led0_spec, 0);

    battery_measure_enable(true);
    int batt_mV = battery_sample();
    if (batt_mV < 0) {
        LOG_WRN("Failed to read battery voltage: %d\n", batt_mV);
    } else {
        send_data.battery_mV = batt_mV;
    }
    battery_measure_enable(false);

    get_i2c_sensors_values(&env_sensors);
#ifdef CONFIG_SGP30
    LOG_INF(
        "%d C %d %% %4d hPa(t=%2d) %d ppm CO2 %d ppm TVOC %d mV", (int)env_sensors.temperature,
        (int)env_sensors.humidity, (int)env_sensors.pressure, (int)env_sensors.temperature_p, env_sensors.sgp30->CO2,
        env_sensors.sgp30->TVOC, batt_mV
    );
#else
    LOG_INF(
        "%d C %d %% %4d hPa(t=%2d) %d mV", (int)env_sensors.temperature, (int)env_sensors.humidity,
        (int)env_sensors.pressure, (int)env_sensors.temperature_p, batt_mV
    );
#endif

    send_data.temperature = env_sensors.temperature;
    send_data.humidity    = env_sensors.humidity;
    send_data.pressure    = env_sensors.pressure;
#ifdef CONFIG_SGP30
    send_data.co2 = env_sensors.sgp30->CO2;
    send_data.tvoc = env_sensors.sgp30->TVOC;
#endif
    bt_app_send_data(&send_data, sizeof(send_data_t));

    gpio_pin_set_dt(&led0_spec, 1);
}

static void app_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&app_work);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    printk("Connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %u)\n", reason);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected,
    .disconnected = disconnected,
};

static int app_bt_cb(void *data)
{
    // Note: The maximum data size is defined by APP_BT_MAX_ATTR_LEN.
    if (APP_BT_MAX_ATTR_LEN < sizeof(send_data_t)) {
        LOG_ERR("app_bt_cb(): Too large data size");
        return -EINVAL;
    }
    memcpy(data, &send_data, sizeof(send_data_t));
    return sizeof(send_data_t);
}

static struct bt_app_cb app_callbacks = {
    .app_bt_cb = app_bt_cb,
};

static bool init_i2c_sensors(struct sensors *env_sensors)
{
    env_sensors->sht3x = sht3x_init_sensor(env_sensors->i2c_dev, SHT3x_ADDR_1);
    if (env_sensors->sht3x == NULL) {
        LOG_ERR("Cannot init sht3x sensor");
        return false;
    }

    // Wait until first measurement is ready (constant time of at least 30 ms
    // or the duration returned from *sht3x_get_measurement_duration*).
    k_msleep(sht3x_get_measurement_duration(sht3x_high));

    env_sensors->qmp6988 = qmp6988_init_sensor(env_sensors->i2c_dev, QMP6988_SLAVE_ADDRESS_L);
    if (env_sensors->qmp6988 == NULL) {
        LOG_ERR("Cannot init qmp6988 sensor");
        return false;
    }

#ifdef CONFIG_SGP30
    env_sensors->sgp30 = sgp30_init_sensor(env_sensors->i2c_dev, SGP30_I2C_DEFAULT_ADDRESS);
    if (env_sensors->sgp30 == NULL) {
        LOG_ERR("Cannot init sgp30 sensor");
        return false;
    }
    sgp30_initAirQuality(env_sensors->sgp30);
    k_msleep(INIT_AIR_QUALITY_DURATION_MS);
#endif

    return true;
}

static void get_i2c_sensors_values(struct sensors *env_sensors)
{
    int ret;
    ret = sht3x_measure(env_sensors->sht3x, &env_sensors->temperature, &env_sensors->humidity);
    qmp6988_calcPressure(env_sensors->qmp6988, &env_sensors->pressure, &env_sensors->temperature_p);
#ifdef CONFIG_SGP30
    sgp30_measureAirQuality(env_sensors->sgp30);
    if (ret) {
        sgp30_setCompensation(env_sensors->sgp30, env_sensors->humidity, env_sensors->temperature);
    }
#endif
}

void main(void)
{
    LOG_MODULE_DECLARE(sensor);
    int ret;

    ret = gpio_pin_configure_dt(&led0_spec, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Cannot configure LED pin [%d]", ret);
        return;
    }

    ret = init_i2c_sensors(&env_sensors);
    if (!ret) {
        LOG_ERR("Cannot init i2c sensors [%d]", ret);
        return;
    }

    ret = bt_app_init(&app_callbacks);
    if (ret) {
        LOG_ERR("Failed to init bt [%d]", ret);
        return;
    }

    ret = bt_app_advertise_start();
    if (ret) {
        LOG_ERR("Failed to start advertising [%d]", ret);
        return;
    }

    /* start periodic timer that expires once every 10 seconds */
    k_timer_start(&app_timer, K_SECONDS(1), K_MSEC(SLEEP_TIME_MS));

    // do nothing
}
