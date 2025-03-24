#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "ssd1306.h"
#include "gfx.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#define TRIGGER_PIN 16
#define ECHO_PIN    17

#define STACK_SIZE_TRIGGER 512
#define STACK_SIZE_ECHO    512
#define STACK_SIZE_OLED    1024

QueueHandle_t xQueueTime = NULL;         
QueueHandle_t xQueueDistance = NULL;     
SemaphoreHandle_t xSemaphoreTrigger = NULL; 

void pin_callback(uint gpio, uint32_t events) {
    static uint64_t local_start = 0;

    if (gpio == ECHO_PIN) {
        if (events & GPIO_IRQ_EDGE_RISE) {
            local_start = time_us_64();
        } else if (events & GPIO_IRQ_EDGE_FALL) {
            uint64_t pulse_duration = time_us_64() - local_start;
            BaseType_t xHigherPriorityWoken = pdFALSE;

            xQueueSendFromISR(xQueueTime, &pulse_duration, &xHigherPriorityWoken);
            portYIELD_FROM_ISR(xHigherPriorityWoken);
        }
    }
}

static inline void trigger_sensor(void) {
    gpio_put(TRIGGER_PIN, 1);

    absolute_time_t t0 = get_absolute_time();
    while (absolute_time_diff_us(t0, get_absolute_time()) < 10) {
        tight_loop_contents();
    }

    gpio_put(TRIGGER_PIN, 0);
}

void trigger_task(void *pvParameters) {
    (void) pvParameters;
    while (1) {
        xSemaphoreGive(xSemaphoreTrigger);

        trigger_sensor();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void echo_task(void *pvParameters) {
    (void) pvParameters;
    const float conversion_factor = 58.0f; 

    while (1) {
        if (xSemaphoreTake(xSemaphoreTrigger, portMAX_DELAY) == pdTRUE) {
            uint64_t pulse_time;
            if (xQueueReceive(xQueueTime, &pulse_time, pdMS_TO_TICKS(100)) == pdTRUE) {
                float distance = (float)pulse_time / conversion_factor;
                xQueueSend(xQueueDistance, &distance, 0);
            } else {
                float distance = -1.0f;
                xQueueSend(xQueueDistance, &distance, 0);
            }
        }
    }
}

void oled_task(void *pvParameters) {
    (void) pvParameters;
    ssd1306_init();
    ssd1306_t disp;
    gfx_init(&disp, 128, 32);

    while (1) {
        float distance;
        if (xQueueReceive(xQueueDistance, &distance, portMAX_DELAY) == pdTRUE) {
            gfx_clear_buffer(&disp);

            if (distance < 0) {
                gfx_draw_string(&disp, 0, 0, 1, "Sensor Falhou!");
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "Dist: %.2f cm", distance);
                gfx_draw_string(&disp, 0, 0, 1, buf);

                int max_bar_length = 112;
                if (distance > 100.0f) {
                    distance = 100.0f;
                }
                int bar_length = (int)((distance / 100.0f) * max_bar_length);
                gfx_draw_line(&disp, 8, 27, 8 + bar_length, 27);
            }
            gfx_show(&disp);
        }
    }
}

int main(void) {
    stdio_init_all();

    gpio_init(TRIGGER_PIN);
    gpio_set_dir(TRIGGER_PIN, GPIO_OUT);
    gpio_put(TRIGGER_PIN, 0);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);

    gpio_set_irq_enabled_with_callback(
        ECHO_PIN,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &pin_callback
    );

    xQueueTime = xQueueCreate(5, sizeof(uint64_t));
    xQueueDistance = xQueueCreate(5, sizeof(float));
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger Task", STACK_SIZE_TRIGGER, NULL, 1, NULL);
    xTaskCreate(echo_task,    "Echo Task",    STACK_SIZE_ECHO,    NULL, 1, NULL);
    xTaskCreate(oled_task,    "OLED Task",    STACK_SIZE_OLED,    NULL, 1, NULL);

    vTaskStartScheduler();

    while (1) {}

    return 0;
}
