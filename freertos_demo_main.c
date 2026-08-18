/*
 * ============================================================================
 *  FreeRTOS Demo — Mini Machine Controller (ESP-IDF)
 * ============================================================================
 *  Oru chinna project-la ELLA major FreeRTOS concept-um:
 *    1. Tasks (different priorities)        5. Mutex (shared data protect)
 *    2. Scheduler / priority                6. ISR-safe API (...FromISR)
 *    3. Queue (task -> task data pass)       7. Task Notification (lightweight)
 *    4. Binary + Counting Semaphore          8. vTaskDelayUntil (periodic)
 *                                            9. Critical Section
 *
 *  Idea: Oru "machine" — sensor value padikkum, control task process pannum,
 *  display task print pannum, button (ISR) emergency stop trigger pannum.
 *
 *  Build: idle ESP-IDF project-la main/ folder-la idha podu, target ESP32.
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
#define RESOURCE_SLOTS   3            // counting semaphore — 3 "tool slots"

/* ---- Handles (global, tasks share panna) ---- */
static QueueHandle_t     sensor_queue;     // (3) Queue
static SemaphoreHandle_t emergency_sem;    // (4) Binary semaphore (ISR -> task)
static SemaphoreHandle_t tool_slots;       // (4) Counting semaphore
static SemaphoreHandle_t state_mutex;      // (5) Mutex
static TaskHandle_t      display_task_h;   // (7) Task notification target

/* ---- Shared machine state (mutex-oda protect pannanum) ---- */
typedef struct {
    int   current_speed;
    int   parts_done;
    bool  running;
} machine_state_t;

static machine_state_t g_state = { .current_speed = 0, .parts_done = 0, .running = true };

/* ---- Critical-section-la protect panra global counter ---- */
static volatile uint32_t g_isr_count = 0;
static portMUX_TYPE isr_mux = portMUX_INITIALIZER_UNLOCKED;


/* ============================================================================
 *  (6) ISR — Button press. ISR-la NORMAL API koodadhu, ...FromISR than.
 * ============================================================================ */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t hp_task_woken = pdFALSE;

    /* Critical section (ISR version) — shared counter safe-a update */
    portENTER_CRITICAL_ISR(&isr_mux);
    g_isr_count++;
    portEXIT_CRITICAL_ISR(&isr_mux);

    /* (4)(6) Binary semaphore give — task-a wake pannu (signaling) */
    xSemaphoreGiveFromISR(emergency_sem, &hp_task_woken);

    /* High-priority task wake aana-na, ISR mudinja udane switch pannu */
    portYIELD_FROM_ISR(hp_task_woken);
}


/* ============================================================================
 *  (8) SENSOR TASK — periodic, vTaskDelayUntil (exact 500ms period)
 *  Sensor value read panni queue-la podum (producer)
 * ============================================================================ */
static void sensor_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    int reading = 0;

    while (1) {
        reading = (reading + 10) % 100;          // fake sensor value

        /* (3) Queue-la podu — control task edukkum */
        if (xQueueSend(sensor_queue, &reading, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Sensor queue full!");
        }

        /* (8) vTaskDelayUntil — drift illama exact 500ms period.
         * (vTaskDelay use panna, execution time-um sேrndhu drift aagum) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(500));
    }
}


/* ============================================================================
 *  CONTROL TASK — queue-la irundhu edukkum (consumer), state update,
 *  display task-ku notify pannum
 * ============================================================================ */
static void control_task(void *arg)
{
    int reading;

    while (1) {
        /* (3) Queue-la data varum varaikkum block aagum (CPU waste illa) */
        if (xQueueReceive(sensor_queue, &reading, portMAX_DELAY) == pdTRUE) {

            /* (4) Counting semaphore — "tool slot" edukka try pannu.
             * 3 slot than, over-a use pannaama limit pannum */
            if (xSemaphoreTake(tool_slots, pdMS_TO_TICKS(100)) == pdTRUE) {

                /* (5) Mutex — shared state-a protect panni update pannu */
                if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                    g_state.current_speed = reading;
                    g_state.parts_done++;
                    xSemaphoreGive(state_mutex);       // mutex-a thிரump kudu
                }

                vTaskDelay(pdMS_TO_TICKS(50));         // "processing" time
                xSemaphoreGive(tool_slots);            // slot-a release pannu

                /* (7) Task Notification — display task-a wake pannu.
                 * Semaphore vida lightweight + fast */
                xTaskNotifyGive(display_task_h);
            }
        }
    }
}


/* ============================================================================
 *  (7) DISPLAY TASK — notification varum varaikkum wait, state print
 * ============================================================================ */
static void display_task(void *arg)
{
    while (1) {
        /* (7) Notification varum varaikkum block (LVGL update maadhiri) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        machine_state_t snapshot;
        /* (5) Mutex — read panra pothum protect (consistent snapshot) */
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            snapshot = g_state;
            xSemaphoreGive(state_mutex);
        }

        ESP_LOGI(TAG, "Speed=%d | Parts=%d | Running=%d",
                 snapshot.current_speed, snapshot.parts_done, snapshot.running);
    }
}


/* ============================================================================
 *  EMERGENCY TASK — button ISR semaphore-ku wait, machine stop
 * ============================================================================ */
static void emergency_task(void *arg)
{
    while (1) {
        /* (4) Binary semaphore — ISR give pannum varaikkum block */
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
 *  app_main — ellathaiyum create panni scheduler-a start pannu
 * ============================================================================ */
void app_main(void)
{
    /* ---- (3)(4)(5) Kernel objects create ---- */
    sensor_queue  = xQueueCreate(5, sizeof(int));      // 5 int hold pannum
    emergency_sem = xSemaphoreCreateBinary();          // binary
    tool_slots    = xSemaphoreCreateCounting(RESOURCE_SLOTS, RESOURCE_SLOTS);
    state_mutex   = xSemaphoreCreateMutex();           // mutex (priority inheritance)

    if (!sensor_queue || !emergency_sem || !tool_slots || !state_mutex) {
        ESP_LOGE(TAG, "Kernel object create fail!");
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

    /* ---- (1)(2) Tasks create — different PRIORITIES (number periisu = high) ----
     * emergency (4) > control (3) > display (2) > sensor (2)
     * Preemptive scheduler high-priority ready task-a udane run pannum */
    xTaskCreate(sensor_task,    "sensor",    2048, NULL, 2, NULL);
    xTaskCreate(control_task,   "control",   2048, NULL, 3, NULL);
    xTaskCreate(display_task,   "display",   2048, NULL, 2, &display_task_h);
    xTaskCreate(emergency_task, "emergency", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "Machine started. Press BOOT button = emergency stop.");
    /* app_main return aanalum tasks run aagum — scheduler already ODUthu */
}
