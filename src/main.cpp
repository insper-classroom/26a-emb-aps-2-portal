// FreeRTOS MUST come first, before any other includes
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

// pico SDK
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

// std
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// project headers
#include "Fusion.h"
#include "hc06.h"
#include "mpu6050.h"

// Edge Impulse
#include "edge-impulse-sdk/classifier/ei_model_types.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "ei_classifier_porting.h"
#include "ei_run_impulse.h"
#include "model-parameters/model_metadata.h"

#define SAMPLE_PERIOD (0.01f)
#define I2C_PORT i2c0

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
    10 - spaço
    11 - ctrl
*/

typedef struct adc {
    int axis;
    int val;
} adc_t;

typedef struct data {
    FusionVector gyroscope;
    FusionVector accelerometer;
} data_t;

using namespace ei;

extern "C" EI_IMPULSE_ERROR
run_classifier(ei::signal_t *signal, ei_impulse_result_t *result, bool debug);

static bool debug_nn = false;

const int MPU_ADDRESS = 0x68;
const int I2C_SDA_GPIO = 4;
const int I2C_SCL_GPIO = 5;
const int RESET_LED_PIN = 14;

/* Botões */
const int BTN_PIN_B = 15;
const int BTN_PIN_O = 16;
const int BTN_PIN_RESET = 17;
const int BTN_PIN_CROUCH = 18;
const int BTN_PIN_JUMP = 13;
const int LED_PIN_R = 10;
const int LED_PIN_G = 11;
const int LED_PIN_B = 12;
const int Feadback_Pin = 14;

/* Queues */
QueueHandle_t xQueueBtn;
QueueHandle_t xQueueMPU;
QueueHandle_t xQueuePos;
QueueHandle_t xQueueAnalog;

/* Semaphores */
SemaphoreHandle_t xSemaphoreReset;
SemaphoreHandle_t xSemaphoreO;
SemaphoreHandle_t xSemaphoreB;
SemaphoreHandle_t xSemaphoreAHRS;

/* Functions */
void reset_mpu6050() {
    // Para as tasks que usam o MPU antes de resetar
    // Acorda o sensor caso esteja em sleep
    uint8_t wake_cmd[] = {0x6B, 0x00};
    i2c_write_blocking(i2c_default, MPU_ADDRESS, wake_cmd, 2, false);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Reset completo
    uint8_t reset_cmd[] = {0x6B, 0x80};
    i2c_write_blocking(i2c_default, MPU_ADDRESS, reset_cmd, 2, false);
    vTaskDelay(pdMS_TO_TICKS(100));  // MPU precisa de ~100ms pós-reset

    // Tira do sleep novamente
    i2c_write_blocking(i2c_default, MPU_ADDRESS, wake_cmd, 2, false);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Reconfigura gyro (±250°/s) e accel (±2g) explicitamente
    uint8_t gyro_cfg[] = {0x1B, 0x00};
    i2c_write_blocking(i2c_default, MPU_ADDRESS, gyro_cfg, 2, false);

    uint8_t accel_cfg[] = {0x1C, 0x00};
    i2c_write_blocking(i2c_default, MPU_ADDRESS, accel_cfg, 2, false);
}

void init_uart_irq() {
    // Turn off FIFO's - we want to do this character by character
    uart_set_fifo_enabled(HC06_UART_ID, false);

    // Set up a RX interrupt
    // We need to set up the handler first
    // Select correct interrupt for the UART we are using
    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    // And set up and enable the interrupt handlers
    // irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);

    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

void init_uart_hc06() {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));

    int __unused actual = uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);

    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(HC06_UART_ID, false, false);

    // Set our data format
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

static void uart_clear_rx_fifo(uart_inst_t *uart) {
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

static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3], int16_t *temp) {
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
        } else if (gpio == BTN_PIN_RESET) {
            xSemaphoreGiveFromISR(xSemaphoreReset, 0);
        } else if (gpio == BTN_PIN_JUMP) {
            adc_btn.axis = 10;
        } else if (gpio == BTN_PIN_CROUCH) {
            adc_btn.axis = 11;
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
        } else if (gpio == BTN_PIN_JUMP) {
            adc_btn.axis = 10;
            xQueueSendFromISR(xQueueBtn, &adc_btn, pdFALSE);
        } else if (gpio == BTN_PIN_CROUCH) {
            adc_btn.axis = 11;
            xQueueSendFromISR(xQueueBtn, &adc_btn, pdFALSE);
        }
    }
}

/* Tasks */
static void reset_task(void *p) {
    while (true) {
        if (xSemaphoreTake(xSemaphoreReset, pdMS_TO_TICKS(10))) {
            gpio_put(RESET_LED_PIN, 1);
            reset_mpu6050();
            xSemaphoreGive(xSemaphoreAHRS);  // sinaliza fusion_task
            gpio_put(RESET_LED_PIN, 0);
        }
    }
}

static void gesture_recognize_task(void *p) {
    mpu6050_init();
    int16_t accelerometer[3], gyro[3], temp;

    while (true) {
        //        ei_printf("\nStarting inferencing in 2 seconds...\n");
        //        vTaskDelay(pdMS_TO_TICKS(2000));
        //        ei_printf("Sampling...\n");

        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = {0};

        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 6) {
            mpu6050_read_raw(accelerometer, gyro, &temp);
            buffer[ix + 0] = accelerometer[0];
            buffer[ix + 1] = accelerometer[1];
            buffer[ix + 2] = accelerometer[2];
            buffer[ix + 3] = gyro[0];
            buffer[ix + 4] = gyro[1];
            buffer[ix + 5] = gyro[2];

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Prepara sinal
        ei::signal_t signal;
        int err = numpy::signal_from_buffer(
            buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (err != 0) {
            // ei_printf("Failed to create signal from buffer (%d)\n", err);
            break;
        }

        // Run the classifier
        ei_impulse_result_t result = {0};
        err = run_classifier(&signal, &result, debug_nn);
        if (err != EI_IMPULSE_OK) {
            // ei_printf("ERR: Failed to run classifier (%d)\n", err);
            break;
        }

        // print the predictions
        // ei_printf("Predictions ");
        // ei_printf("(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
        //           result.timing.dsp, result.timing.classification,
        //           result.timing.anomaly);
        // ei_printf(": \n");
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            // ei_printf("teste    %s: %.5f\n", result.classification[ix].label,
            //           result.classification[ix].value);

            if (strcmp(result.classification[ix].label, "click") == 0 &&
                result.classification[ix].value > 0.9f) {
                // ei_printf(">>> CLICK DETECTADO! (%.5f)\n",
                //           result.classification[ix].value);
                // coloca sua lógica aqui
                adc_t adc_btn;
                adc_btn.axis = 9;
                adc_btn.val = 1;

                xQueueSend(xQueueBtn, &adc_btn, pdFALSE);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

#if EI_CLASSIFIER_HAS_ANOMALY == 1
        // ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif
    }
}

void mpu6050_task(void *p) {
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

void fusion_task(void *p) {
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
        if (xSemaphoreTake(xSemaphoreAHRS, 0)) {  // 0 = não bloqueia
            FusionAhrsInitialise(&ahrs);
        }

        if (xQueueReceive(xQueueMPU, &sensor_data, pdMS_TO_TICKS(10))) {
            FusionVector gyroscope, accelerometer;

            gyroscope = sensor_data.gyroscope;
            accelerometer = sensor_data.accelerometer;

            FusionAhrsUpdateNoMagnetometer(&ahrs, gyroscope, accelerometer,
                                           SAMPLE_PERIOD);

            const FusionEuler euler =
                FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

            adc_x.val = -angle_to_255(euler.angle.yaw) * mouse_speed;
            adc_y.val = -angle_to_255(euler.angle.roll) * mouse_speed;

            send_counter = (send_counter + 1) % 5;
            if (send_counter == 0) {
                if (adc_x.val > dead_zone || adc_x.val < -dead_zone)
                    xQueueSend(xQueuePos, &adc_x, pdMS_TO_TICKS(10));
                if (adc_y.val > dead_zone || adc_y.val < -dead_zone)
                    xQueueSend(xQueuePos, &adc_y, pdMS_TO_TICKS(10));
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void analog_task(void *p) {
    adc_t adc_x;
    int x_filter_data[5] = {0};
    int x_filter_index = 0;

    adc_t adc_y;
    int y_filter_data[5] = {0};
    int y_filter_index = 0;

    int deadzone = 100;
    bool x_pressed = false;  // rastreia se alguma tecla X está pressionada
    bool y_pressed = false;

    while (1) {
        adc_select_input(0);
        int16_t x_output = (adc_read() - 2047) / 8;
        x_filter_data[x_filter_index] = x_output;
        x_filter_index = (x_filter_index + 1) % 5;

        int x_sum = 0;
        for (int i = 0; i < 5; i++) x_sum += x_filter_data[i];
        int x_media = x_sum / 5;

        if (x_media > deadzone) {
            adc_x.axis = 8;
            adc_x.val = 1;
            xQueueSend(xQueueBtn, &adc_x, pdMS_TO_TICKS(10));
            x_pressed = true;
        } else if (x_media < -deadzone) {
            adc_x.axis = 6;
            adc_x.val = 1;
            xQueueSend(xQueueBtn, &adc_x, pdMS_TO_TICKS(10));
            x_pressed = true;
        } else if (x_pressed) {
            // solta a ultima tecla X pressionada mandando val=0
            adc_x.val = 0;
            xQueueSend(xQueueBtn, &adc_x, pdMS_TO_TICKS(10));
            x_pressed = false;
        }

        adc_select_input(1);
        int16_t y_output = (adc_read() - 2047) / 8;
        y_filter_data[y_filter_index] = y_output;
        y_filter_index = (y_filter_index + 1) % 5;

        int y_sum = 0;
        for (int i = 0; i < 5; i++) y_sum += y_filter_data[i];
        int y_media = y_sum / 5;

        if (y_media > deadzone) {
            adc_y.axis = 7;
            adc_y.val = 1;
            xQueueSend(xQueueAnalog, &adc_y, pdMS_TO_TICKS(10));
            y_pressed = true;
        } else if (y_media < -deadzone) {
            adc_y.axis = 5;
            adc_y.val = 1;
            xQueueSend(xQueueAnalog, &adc_y, pdMS_TO_TICKS(10));
            y_pressed = true;
        } else if (y_pressed) {
            adc_y.val = 0;
            xQueueSend(xQueueAnalog, &adc_y, pdMS_TO_TICKS(10));
            y_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void com_task(void *p) {
    adc_t adc_data;
    while (true) {
        if (xQueueReceive(xQueuePos, &adc_data, pdMS_TO_TICKS(5))) {
            uart_putc_raw(HC06_UART_ID, adc_data.axis);
            uart_putc_raw(HC06_UART_ID, adc_data.val);
            uart_putc_raw(HC06_UART_ID, adc_data.val >> 8);
            uart_putc_raw(HC06_UART_ID, -1);
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (xQueueReceive(xQueueBtn, &adc_data, pdMS_TO_TICKS(5))) {
            uart_putc_raw(HC06_UART_ID, adc_data.axis);
            uart_putc_raw(HC06_UART_ID, adc_data.val);
            uart_putc_raw(HC06_UART_ID, adc_data.val >> 8);
            uart_putc_raw(HC06_UART_ID, -1);
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (xQueueReceive(xQueueAnalog, &adc_data, pdMS_TO_TICKS(5))) {
            uart_putc_raw(HC06_UART_ID, adc_data.axis);
            uart_putc_raw(HC06_UART_ID, adc_data.val);
            uart_putc_raw(HC06_UART_ID, adc_data.val >> 8);
            uart_putc_raw(HC06_UART_ID, -1);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void rgb_task(void *p) { // task que controla os leds rgb e a vibração
    gpio_set_function(LED_PIN_R, GPIO_FUNC_PWM);
    const uint slice_num_r = pwm_gpio_to_slice_num(LED_PIN_R);
    const uint chan_r = pwm_gpio_to_channel(LED_PIN_R);

    gpio_set_function(LED_PIN_G, GPIO_FUNC_PWM);
    const uint slice_num_g = pwm_gpio_to_slice_num(LED_PIN_G);
    const uint chan_g = pwm_gpio_to_channel(LED_PIN_G);

    gpio_set_function(LED_PIN_B, GPIO_FUNC_PWM);
    const uint slice_num_b = pwm_gpio_to_slice_num(LED_PIN_B);
    const uint chan_b = pwm_gpio_to_channel(LED_PIN_B);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 4.0f);
    pwm_config_set_wrap(&config, 255);

    pwm_init(slice_num_r, &config, true);
    pwm_init(slice_num_g, &config, true);
    pwm_init(slice_num_b, &config, true);

    pwm_set_chan_level(slice_num_r, chan_r, 0);
    pwm_set_chan_level(slice_num_g, chan_g, 0);
    pwm_set_chan_level(slice_num_b, chan_b, 0);

    while (1) {
        if (xSemaphoreTake(xSemaphoreB, pdMS_TO_TICKS(5))) {
            pwm_set_chan_level(slice_num_b, chan_b, 255);
            gpio_put(Feadback_Pin, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_put(Feadback_Pin, 0);
            pwm_set_chan_level(slice_num_b, chan_b, 0);
        }
        if (xSemaphoreTake(xSemaphoreO, pdMS_TO_TICKS(5))) {
            pwm_set_chan_level(slice_num_r, chan_r, 255);
            pwm_set_chan_level(slice_num_g, chan_g, 90);
            gpio_put(Feadback_Pin, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_put(Feadback_Pin, 0);
            pwm_set_chan_level(slice_num_r, chan_r, 0);
            pwm_set_chan_level(slice_num_g, chan_g, 0);
        }
    }
}

int main(void) {
    stdio_init_all();

    adc_init();
    adc_gpio_init(26);
    adc_gpio_init(27);
    init_uart_hc06();
    init_uart_irq();

    gpio_init(RESET_LED_PIN);
    gpio_set_dir(RESET_LED_PIN, GPIO_OUT);

    gpio_init(Feadback_Pin);
    gpio_set_dir(Feadback_Pin, GPIO_OUT);

    gpio_init(BTN_PIN_B);
    gpio_set_dir(BTN_PIN_B, GPIO_IN);
    gpio_pull_up(BTN_PIN_B);

    gpio_init(BTN_PIN_O);
    gpio_set_dir(BTN_PIN_O, GPIO_IN);
    gpio_pull_up(BTN_PIN_O);

    gpio_init(BTN_PIN_RESET);
    gpio_set_dir(BTN_PIN_RESET, GPIO_IN);
    gpio_pull_up(BTN_PIN_RESET);

    gpio_init(BTN_PIN_JUMP);
    gpio_set_dir(BTN_PIN_JUMP, GPIO_IN);
    gpio_pull_up(BTN_PIN_JUMP);

    gpio_init(BTN_PIN_CROUCH);
    gpio_set_dir(BTN_PIN_CROUCH, GPIO_IN);
    gpio_pull_up(BTN_PIN_CROUCH);

    gpio_set_irq_enabled_with_callback(BTN_PIN_B,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, &btn_callback);

    gpio_set_irq_enabled(BTN_PIN_O, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                         true);

    gpio_set_irq_enabled(BTN_PIN_RESET, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                         true);

    gpio_set_irq_enabled(BTN_PIN_CROUCH,
                         GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);

    gpio_set_irq_enabled(BTN_PIN_JUMP, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                         true);

    xQueueMPU = xQueueCreate(64, sizeof(data_t));
    xQueuePos = xQueueCreate(64, sizeof(adc_t));
    xQueueBtn = xQueueCreate(64, sizeof(adc_t));
    xQueueAnalog = xQueueCreate(64, sizeof(adc_t));

    xSemaphoreReset = xSemaphoreCreateBinary();
    xSemaphoreB = xSemaphoreCreateBinary();
    xSemaphoreO = xSemaphoreCreateBinary();
    xSemaphoreAHRS = xSemaphoreCreateBinary();


    xTaskCreate(gesture_recognize_task, "gesture_task 1", 8192, NULL, 1, NULL);
    xTaskCreate(mpu6050_task, "mpu_task", 8192, NULL, 1, NULL);
    xTaskCreate(fusion_task, "fusion_task", 1024, NULL, 1, NULL);
    xTaskCreate(analog_task, "analog_task", 1024, NULL, 1, NULL);
    xTaskCreate(com_task, "com_task", 1024, NULL, 1, NULL);
    xTaskCreate(reset_task, "com_task", 256, NULL, 1, NULL);
    xTaskCreate(rgb_task, "RGB_task", 128, NULL, 1, NULL);
    vTaskStartScheduler();

    while (true)
        ;
}
