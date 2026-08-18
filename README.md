FreeRTOS Machine Controller Demo (ESP-IDF)

A compact ESP-IDF project that demonstrates all the core FreeRTOS concepts in one place, modeled as a mini industrial machine controller (sensor, control, display, and an emergency stop). Built to show practical, real-world use of an RTOS on the ESP32.

Features
Tasks with different priorities managed by the preemptive scheduler
Queue for thread-safe data passing (sensor → control)
Binary semaphore for ISR-to-task signaling (button → emergency stop)
Counting semaphore to manage a limited resource pool
Mutex to protect shared machine state
ISR-safe APIs (...FromISR) with portYIELD_FROM_ISR
Task notifications as a lightweight signaling mechanism
vTaskDelayUntil for drift-free periodic execution
Critical section for atomic access to a shared counter
Architecture
[Button ISR] --binary sem--> [Emergency Task]  (stops the machine)

[Sensor Task] --queue--> [Control Task] --notify--> [Display Task]
 (periodic)             (updates state,             (prints state)
                         mutex-protected)
FreeRTOS Concept Map
Concept	Where in code
Task / Priority / Scheduler	xTaskCreate(...) in app_main
Queue	sensor_queue, xQueueSend / xQueueReceive
Binary Semaphore	emergency_sem (ISR → task)
Counting Semaphore	tool_slots
Mutex	state_mutex
ISR-safe API	xSemaphoreGiveFromISR, portYIELD_FROM_ISR
Task Notification	xTaskNotifyGive / ulTaskNotifyTake
Periodic timing	vTaskDelayUntil
Critical Section	portENTER_CRITICAL_ISR
Hardware
ESP32 development board
Uses the on-board BOOT button (GPIO0) to trigger the emergency stop
Build & Run
bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor

Expected serial output:

I MACHINE: Machine started. Press BOOT button = emergency stop.
I MACHINE: Speed=10 | Parts=1 | Running=1
I MACHINE: Speed=20 | Parts=2 | Running=1
...

Pressing the BOOT button:

E MACHINE: >>> EMERGENCY STOP! (button presses so far: 1)
Things to Experiment With
Change task priorities and observe scheduling behavior
Remove the mutex to see a race condition on shared state
Reduce the counting semaphore count and observe throughput changes
Replace vTaskDelayUntil with vTaskDelay and observe timing drift
Swap the task notification for a binary semaphore and compare
Tech Stack

C, ESP-IDF, FreeRTOS, ESP32

Built by Rasu J — Embedded Software Engineer.
