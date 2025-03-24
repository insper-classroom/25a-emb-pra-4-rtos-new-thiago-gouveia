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

volatile uint64_t t_start = 0;
volatile uint64_t t_end = 0;

void pin_callback(uint gpio, uint32_t events) {
    if (gpio == ECHO_PIN) {
        if (events & GPIO_IRQ_EDGE_RISE) {
            t_start = time_us_64();
        } else if (events & GPIO_IRQ_EDGE_FALL) {
            t_end = time_us_64();
            uint64_t pulse_duration = t_end - t_start;
            BaseType_t xHigherPriorityWoken = pdFALSE;
            xQueueSendFromISR(xQueueTime, &pulse_duration, &xHigherPriorityWoken);
            portYIELD_FROM_ISR(xHigherPriorityWoken);
        }
    }
}

void trigger_task(void *pvParameters) {
    (void) pvParameters;
    while (1) {
        xSemaphoreGive(xSemaphoreTrigger);

        gpio_put(TRIGGER_PIN, 1);
        sleep_us(10);
        gpio_put(TRIGGER_PIN, 0);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void echo_task(void *pvParameters) {
    (void) pvParameters;
    const float conversion_factor = 58.0;
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
                if (distance > 100.0f)
                    distance = 100.0f;
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
