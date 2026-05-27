#include <stdio.h>

#include "FreeRTOS.h"
#include "Fusion.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "mpu6050.h"
#include "pico/stdlib.h"
#include "pins.h"
#include "semphr.h"
#include "task.h"
#define SAMPLE_PERIOD (0.01f)
#define UART_ID uart0

/*
Protocolo:
    0 - x
    1 - y
    3 - r_click
    4 - l_click
    5 - w
    6 - a
    7 - s
    8 - d
    9 - e
*/

typedef struct adc {
    int axis;
    int val;
} adc_t;

typedef struct data {
    FusionVector gyroscope;
    FusionVector accelerometer;
} data_t;

typedef struct rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

const int MPU_ADDRESS = 0x68;
const int I2C_SDA_GPIO = 4;
const int I2C_SCL_GPIO = 5;

/* Queues */
QueueHandle_t xQueueBtn;
QueueHandle_t xQueueMPU;
QueueHandle_t xQueuePos;

/* Semaphores */
SemaphoreHandle_t xSemaphoreBtn;

/* Functions */

static void uart_clear_rx_fifo(uart_inst_t* uart) {
    while (uart_is_readable(uart)) {
        (void)uart_getc(uart);
    }
}

static int angle_to_255(float angle_deg) {
    if (angle_deg > 180.0f) {
        angle_deg = 180.0f;
    } else if (angle_deg < -180.0f) {
        angle_deg = -180.0f;
    }

    return (int)(angle_deg * (255.0f / 180.0f));
}

static int tilt_to_255(float tilt_deg) {
    if (tilt_deg < 0.0f) {
        tilt_deg = 0.0f;
    } else if (tilt_deg > 90.0f) {
        tilt_deg = 90.0f;
    }

    return (int)(tilt_deg * (255.0f / 90.0f));
}

static void mpu6050_init() {
    i2c_init(i2c_default, 400 * 1000);
    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);

    // Two byte reset. First byte register, second byte data
    // There are a load more options to set up the device in different ways that
    // could be added here
    uint8_t buf[] = {0x6B, 0x00};
    i2c_write_blocking(i2c_default, MPU_ADDRESS, buf, 2, false);
}

static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3], int16_t* temp) {
    uint8_t buffer[14];

    // Read all data sequentially starting from acceleration registers (0x3B)
    // 0x3B-0x40: acceleration (6 bytes)
    // 0x41-0x42: temperature (2 bytes)
    // 0x43-0x48: gyro (6 bytes)
    uint8_t val = 0x3B;
    i2c_write_blocking(i2c_default, MPU_ADDRESS, &val, 1, true);
    i2c_read_blocking(i2c_default, MPU_ADDRESS, buffer, 14, false);

    // Parse acceleration
    for (int i = 0; i < 3; i++) {
        accel[i] = (buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
    }

    // Parse temperature
    *temp = buffer[6] << 8 | buffer[7];

    // Parse gyro
    for (int i = 0; i < 3; i++) {
        gyro[i] = (buffer[8 + i * 2] << 8 | buffer[8 + (i * 2) + 1]);
    }
}

/*Button Callback*/

void btn_callback(uint gpio, uint32_t events) {
    adc_t adc_btn;
    if (events == GPIO_IRQ_EDGE_FALL) {
        adc_btn.val = 1;
        if (gpio == BTN_PIN_B) {
            adc_btn.axis = 3;
        } else if (gpio == BTN_PIN_O) {
            adc_btn.axis = 4;
        } else if (gpio == BTN_PIN_GRAB) {
            adc_btn.axis = 5;
        }

        xQueueSendFromISR(xQueueBtn, &adc_btn, pdFALSE);
    } else if (events == GPIO_IRQ_EDGE_RISE) {
        adc_btn.val = 0;
        if (gpio == BTN_PIN_B) {
            adc_btn.axis = 3;
            xQueueSendFromISR(xQueueBtn, &adc_btn, pdFALSE);
        } else if (gpio == BTN_PIN_O) {
            adc_btn.axis = 4;
            xQueueSendFromISR(xQueueBtn, &adc_btn, pdFALSE);
        }
    }
}

/* Task function */

void mpu6050_task(void* p) {
    mpu6050_init();
    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);

    while (true) {
        int16_t acceleration[3], gyro[3], temp;

        mpu6050_read_raw(acceleration, gyro, &temp);

        FusionVector gyroscope;

        gyroscope.axis.x = gyro[0] / 131.0f;
        gyroscope.axis.y = gyro[1] / 131.0f;
        gyroscope.axis.z = gyro[2] / 131.0f;

        FusionVector accelerometer;

        accelerometer.axis.x = acceleration[0] / 16384.0f;
        accelerometer.axis.y = acceleration[1] / 16384.0f,
        accelerometer.axis.z = acceleration[2] / 16384.0f;


        data_t sensor_data;

        sensor_data.gyroscope = gyroscope;
        sensor_data.accelerometer = accelerometer;

        xQueueSend(xQueueMPU, &sensor_data, pdMS_TO_TICKS(10));

        // 100 Hz
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void fusion_task(void* p) {
    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);
    data_t sensor_data;
    adc_t adc_x, adc_y;
    adc_x.axis = 0;
    adc_y.axis = 1;
    int dead_zone = 10;
    float mouse_speed = 0.4;
    int send_counter = 0;

    while (true) {
        if (xQueueReceive(xQueueMPU, &sensor_data, pdMS_TO_TICKS(10))) {
            FusionVector gyroscope, accelerometer;

            gyroscope = sensor_data.gyroscope;
            accelerometer = sensor_data.accelerometer;

            FusionAhrsUpdateNoMagnetometer(&ahrs, gyroscope, accelerometer,
                                           SAMPLE_PERIOD);

            const FusionEuler euler =
                FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

            // printf("Roll %0.1f, Pitch %0.1f, Yaw %0.1f\n", euler.angle.roll,
            // euler.angle.pitch, euler.angle.yaw);
            if (accelerometer.axis.y > .8) {
                // printf("Clique\n");
                xSemaphoreGive(xSemaphoreBtn);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            adc_x.val = -angle_to_255(euler.angle.yaw) * mouse_speed;
            adc_y.val = -angle_to_255(euler.angle.roll) * mouse_speed;

            send_counter = (send_counter + 1) % 5;
            if (send_counter == 0) {
                if (adc_x.val > dead_zone || adc_x.val < -dead_zone) {
                    xQueueSend(xQueuePos, &adc_x, pdMS_TO_TICKS(10));
                }

                if (adc_y.val > dead_zone || adc_y.val < -dead_zone) {
                    xQueueSend(xQueuePos, &adc_y, pdMS_TO_TICKS(10));
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void analog_task(void* p) {
    adc_t adc_x;
    int x_filter_data[5] = {0};
    int x_filter_index = 0;

    adc_t adc_y;
    int y_filter_data[5] = {0};
    int y_filter_index = 0;

    while (1) {
        adc_select_input(0);
        int16_t x_output = (adc_read() - 2047) / 8;
        x_filter_data[x_filter_index] = x_output;
        x_filter_index = (x_filter_index + 1) % 5;

        int x_sum = 0;
        for (int i = 0; i < 5; i++) {
            x_sum += x_filter_data[i];
        }

        int x_media = x_sum / 5;
        if (x_media > 30) {
            adc_x.axis = 8;
            adc_x.val = x_media;
            xQueueSend(xQueueBtn, &adc_x, pdMS_TO_TICKS(10));
        } else if (x_media < -30) {
            adc_x.axis = 6;
            adc_x.val = x_media;
            xQueueSend(xQueueBtn, &adc_x, pdMS_TO_TICKS(10));
        }

        adc_select_input(1);
        int16_t y_output = (adc_read() - 2047) / 8;
        y_filter_data[y_filter_index] = y_output;
        y_filter_index = (y_filter_index + 1) % 5;

        int y_sum = 0;
        for (int i = 0; i < 5; i++) {
            y_sum += y_filter_data[i];
        }

        int y_media = y_sum / 5;
        if (y_media > 30) {
            adc_y.axis = 5;
            adc_y.val = y_media;
            xQueueSend(xQueueBtn, &adc_y, pdMS_TO_TICKS(10));
        } else if (y_media < -30) {
            adc_y.axis = 7;
            adc_y.val = y_media;
            xQueueSend(xQueueBtn, &adc_y, pdMS_TO_TICKS(10));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void uart_task(void *p) {
    adc_t adc_data;
    while (true) {
        if (xQueueReceive(xQueuePos, &adc_data, pdMS_TO_TICKS(100)) || xQueueReceive(xQueueBtn, &adc_data, pdMS_TO_TICKS(100))) {
            uart_putc(UART_ID, adc_data.axis);
            uart_putc(UART_ID, adc_data.val);
            uart_putc(UART_ID, adc_data.val >> 8);
            uart_putc(UART_ID, -1);
        }
    }
}

int main(void) {
    stdio_init_all();
    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);

    xQueueMPU = xQueueCreate(64, sizeof(data_t));
    xQueuePos = xQueueCreate(64, sizeof(adc_t));
    xQueueBtn = xQueueCreate(64, sizeof(adc_t));

    xTaskCreate(mpu6050_task, "mpu6050_Task", 8192, NULL, 1, NULL);
    xTaskCreate(fusion_task, "fusion_task", 1024, NULL, 1, NULL);
    xTaskCreate(analog_task, "analog_task", 1024, NULL, 1, NULL);
    xTaskCreate(uart_task, "uart_task", 256, NULL, 1, NULL);

    vTaskStartScheduler();

    // Should never reach here
    for (;;);
}
