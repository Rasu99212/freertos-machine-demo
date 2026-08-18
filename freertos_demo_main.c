/*
 * ============================================================================
 *  FreeRTOS Demo - Mini Machine Controller (ESP-IDF)
 * ============================================================================
 *  A single project demonstrating all major FreeRTOS concepts:
 *    1. Tasks (different priorities)        5. Mutex (protect shared data)
 *    2. Scheduler / priority                6. ISR-safe API (...FromISR)
 *    3. Queue (task -> task data passing)    7. Task Notification (lightweight)
 *    4. Binary + Counting Semaphore          8. vTaskDelayUntil (periodic)
 *                                            9. Critical Section
 *
 *  Idea: a "machine" that reads a sensor value, processes it in a control task,
 *  prints it in a display task, and stops via a button (ISR) emergency stop.
 *
 *  Build: place this file in the main/ folder of an ESP-IDF project (ESP32).
 * ============================================================================
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "MACHINE";

#define BUTTON_GPIO      0            // boot button (emergency stop)
#define RESOURCE_SLOTS   3            // counting semaphore - 3 "tool slots"

/* ---- Handles (global, shared by tasks) ---- */
static QueueHandle_t     sensor_queue;     // (3) Queue
static SemaphoreHandle_t emergency_sem;    // (4) Binary semaphore (ISR -> task)
static SemaphoreHandle_t tool_slots;       // (4) Counting semaphore
static SemaphoreHandle_t state_mutex;      // (5) Mutex
static TaskHandle_t      display_task_h;   // (7) Task notification target

/* ---- Shared machine state (must be protected by the mutex) ---- */
typedef struct {
    int   current_speed;
    int   parts_done;
    bool  running;
} machine_state_t;

static machine_state_t g_state = { .current_speed = 0, .parts_done = 0, .running = true };

/* ---- Global counter protected by a critical section ---- */
static volatile uint32_t g_isr_count = 0;
static portMUX_TYPE isr_mux = portMUX_INITIALIZER_UNLOCKED;


/* ============================================================================
 *  (6) ISR - Button press. Only ...FromISR APIs are allowed inside an ISR.
 * ============================================================================ */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;

    /* Critical section (ISR variant) - safely update the shared counter */
    portENTER_CRITICAL_ISR(&isr_mux);
    g_isr_count++;
    portEXIT_CRITICAL_ISR(&isr_mux);

    /* (4)(6) Give a binary semaphore to wake the emergency task (signaling) */
    xSemaphoreGiveFromISR(emergency_sem, &hp_task_woken);

    /* If a higher-priority task was woken, switch to it as the ISR exits */
    portYIELD_FROM_ISR(hp_task_woken);
}


/* ============================================================================
 *  (8) SENSOR TASK - periodic with vTaskDelayUntil (exact 500 ms period).
 *  Reads a sensor value and pushes it into the queue (producer).
 * ============================================================================ */
static void sensor_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    int reading = 0;

    while (1) {
        reading = (reading + 10) % 100;          // fake sensor value

        /* (3) Push into the queue - the control task will consume it */
        if (xQueueSend(sensor_queue, &reading, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Sensor queue full!");
        }

        /* (8) vTaskDelayUntil - exact 500 ms period with no drift.
         * (vTaskDelay would add the execution time and drift over time.) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(500));
    }
}


/* ============================================================================
 *  CONTROL TASK - consumes from the queue, updates the shared state, and
 *  notifies the display task.
 * ============================================================================ */
static void control_task(void *arg)
{
    int reading;

    while (1) {
        /* (3) Block until data is available (no busy-waiting / CPU waste) */
        if (xQueueReceive(sensor_queue, &reading, portMAX_DELAY) == pdTRUE) {

            /* (4) Counting semaphore - acquire a "tool slot".
             * Only 3 slots exist, which limits concurrent usage. */
            if (xSemaphoreTake(tool_slots, pdMS_TO_TICKS(100)) == pdTRUE) {

                /* (5) Mutex - protect the shared state while updating */
                if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                    g_state.current_speed = reading;
                    g_state.parts_done++;
                    xSemaphoreGive(state_mutex);       // release the mutex
                }

                vTaskDelay(pdMS_TO_TICKS(50));         // simulated processing time
                xSemaphoreGive(tool_slots);            // release the slot

                /* (7) Task Notification - wake the display task.
                 * Lighter and faster than a semaphore. */
                xTaskNotifyGive(display_task_h);
            }
        }
    }
}


/* ============================================================================
 *  (7) DISPLAY TASK - waits for a notification, then prints the state.
 * ============================================================================ */
static void display_task(void *arg)
{
    while (1) {
        /* (7) Block until notified (similar to refreshing an LVGL screen) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        machine_state_t snapshot;
        /* (5) Mutex - protect the state while reading (consistent snapshot) */
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            snapshot = g_state;
            xSemaphoreGive(state_mutex);
        }

        ESP_LOGI(TAG, "Speed=%d | Parts=%d | Running=%d",
                 snapshot.current_speed, snapshot.parts_done, snapshot.running);
    }
}


/* ============================================================================
 *  EMERGENCY TASK - waits for the button ISR semaphore, then stops the machine.
 * ============================================================================ */
static void emergency_task(void *arg)
{
    while (1) {
        /* (4) Binary semaphore - block until the ISR gives it */
        if (xSemaphoreTake(emergency_sem, portMAX_DELAY) == pdTRUE) {

            if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                g_state.running = false;
                g_state.current_speed = 0;
                xSemaphoreGive(state_mutex);
            }
            ESP_LOGE(TAG, ">>> EMERGENCY STOP! (button presses so far: %u)",
                     (unsigned) g_isr_count);
        }
    }
}


/* ============================================================================
 *  app_main - create everything and let the scheduler run.
 * ============================================================================ */
void app_main(void)
{
    /* ---- (3)(4)(5) Create kernel objects ---- */
    sensor_queue  = xQueueCreate(5, sizeof(int));      // holds up to 5 ints
    emergency_sem = xSemaphoreCreateBinary();          // binary
    tool_slots    = xSemaphoreCreateCounting(RESOURCE_SLOTS, RESOURCE_SLOTS);
    state_mutex   = xSemaphoreCreateMutex();           // mutex (priority inheritance)

    if (!sensor_queue || !emergency_sem || !tool_slots || !state_mutex) {
        ESP_LOGE(TAG, "Failed to create kernel objects!");
        return;
    }

    /* ---- (6) Button GPIO + ISR setup ---- */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,                // press = falling edge
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    /* ---- (1)(2) Create tasks with different PRIORITIES (higher number = higher) ----
     * emergency (4) > control (3) > display (2) = sensor (2)
     * The preemptive scheduler runs the highest-priority ready task. */
    xTaskCreate(sensor_task,    "sensor",    2048, NULL, 2, NULL);
    xTaskCreate(control_task,   "control",   2048, NULL, 3, NULL);
    xTaskCreate(display_task,   "display",   2048, NULL, 2, &display_task_h);
    xTaskCreate(emergency_task, "emergency", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "Machine started. Press BOOT button = emergency stop.");
    /* Tasks keep running even after app_main returns - the scheduler is active */
}
