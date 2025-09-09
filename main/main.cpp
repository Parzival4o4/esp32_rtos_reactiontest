#include <stdio.h>
#include <inttypes.h>
#include <string>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "esp_chip_info.h"
// #include "esp_flash.h"
// #include "esp_system.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define BUTTON_GPIO GPIO_NUM_16  // Button connected to GPIO 16
#define ESP_INTR_FLAG_DEFAULT 0
#define DEBOUNCE_DELAY 250000  // 250 ms debounce delay in microseconds

volatile int64_t debounce_time = 0;

volatile int64_t react_time_start = 0;
volatile int64_t react_time_end = 0;

SemaphoreHandle_t serialMutex;

TaskHandle_t thandle_waitForGameStart = NULL;
TaskHandle_t thandle_start_game = NULL;
TaskHandle_t thandle_too_early = NULL;
TaskHandle_t thandle_reacted = NULL;

void IRAM_ATTR start_game_isr(void* arg);
void IRAM_ATTR too_early_isr(void* arg);
void IRAM_ATTR reacted_isr(void* arg);

int advert_index = 0;
const char *adverts[] = {
    "Think you're quick? Prove it! Test your reflexes and claim your spot on the leaderboard!",
    "Do you have what it takes to be the fastest? Tap, click, react—show us your speed!",
    "Warning: Highly Addictive! Test your reflexes and challenge your friends to see who's the real speedster.",
    "One tap is all it takes to reveal your true reaction time—dare to find out?",
    "Get ready to push your limits! Only the sharpest minds and fastest fingers will dominate this test!"
};

void waitForGameStart(void* not_used) {
    /*wait for a player to press the button to start the game*/
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // send a message every 2 seconds
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        printf("%s\n", adverts[advert_index]);
        advert_index = (advert_index + 1) % (sizeof(adverts) / sizeof(adverts[0]));
        xSemaphoreGive(serialMutex);
    }
}

void return_to_start() {
    /*this function brings everything back to the initial state*/

    // register the start_game_isr
    gpio_isr_handler_add(BUTTON_GPIO, start_game_isr, NULL);  // Setup and test button

    // enable the waitForGameStart task
    if (thandle_waitForGameStart == NULL) {
        xTaskCreate(
            waitForGameStart,
            "waitForGameStart",
            2048,
            NULL,
            1,
            &thandle_waitForGameStart // Task handle
        );
    } else {
        vTaskResume(thandle_waitForGameStart);
    }
}

void reacted(void* not_used) {
    /*user pressed the button after the measurement was started*/

    // calc Time in microseconds
    int react_time = (react_time_end - react_time_start);

    // convert to seconds as float
    float react_time_sec = react_time / 1000000.0f;

    // get mutex
    xSemaphoreTake(serialMutex, portMAX_DELAY);

    printf("Your reaction time was %.6f seconds\n", react_time_sec);
    printf("\n\n\n");

    // release mutex done printing
    xSemaphoreGive(serialMutex);

    // reset to start
    return_to_start();

    // delete this task
    vTaskDelete(thandle_reacted);
    thandle_reacted = NULL;
}


void IRAM_ATTR reacted_isr(void* arg) {
    /*init the reacted task*/

    int64_t now = esp_timer_get_time();
    if (now - debounce_time < DEBOUNCE_DELAY) return;
    debounce_time = now;

    // save Time
    react_time_end = esp_timer_get_time();

    xTaskCreate(
        reacted,    // Function name of the task
        "reacted",  // Name of the task (e.g. for debugging)
        2048,       // Stack size (bytes)
        NULL,
        1,          // Task priority
        &thandle_reacted // Task handle
    );

    gpio_isr_handler_remove(BUTTON_GPIO);
}

void too_early(void* not_used) {
    /*player pressed too early*/

    // get mutex
    xSemaphoreTake(serialMutex, portMAX_DELAY);

    // you suck message
    printf("you pressed too early, you suck\n");

    // delete the start_measurement task
    if (thandle_start_game == NULL) {
        printf("ERROR too early: start measurement task is NULL");
    } else {
        vTaskDelete(thandle_start_game);
        thandle_start_game = NULL;
    }

    // release mutex done printing
    xSemaphoreGive(serialMutex);

    // return to start
    return_to_start();

    // delete this task
    vTaskDelete(thandle_too_early);
    thandle_too_early = NULL;
}

void IRAM_ATTR too_early_isr(void* arg) {
    /*init the too early task*/

    int64_t now = esp_timer_get_time();
    if (now - debounce_time < DEBOUNCE_DELAY) return;
    debounce_time = now;

    xTaskCreate(
        too_early,
        "too_early",
        2048,
        NULL,
        1,
        &thandle_too_early
    );

    gpio_isr_handler_remove(BUTTON_GPIO);
}

void start_game(void* not_used) {
    /*inits the game*/

    // get mutex
    xSemaphoreTake(serialMutex, portMAX_DELAY);

    // disable waitForGameStart 
    // because the mutex is taken already we know that wait for game is allowed to be interrupted
    vTaskSuspend(thandle_waitForGameStart);

    // notify player game starts
    printf("\n");
    printf("----------------- Game start -----------------\n");

    // release mutex done printing
    xSemaphoreGive(serialMutex);

    // register interrupt handler for early press
    gpio_isr_handler_add(BUTTON_GPIO, too_early_isr, NULL);

    // choose wait time 
    long base_delay = 3000; // 3 seconds base delay
    uint32_t rand = esp_random() % (10 * 1000);
    long delay_time = base_delay + rand;

    // wait the time
    vTaskDelay(pdMS_TO_TICKS(delay_time));

    // get mutex
    xSemaphoreTake(serialMutex, portMAX_DELAY);

    // unregister the too early interrupt handler
    gpio_isr_handler_remove(BUTTON_GPIO);

    // register react interrupt handler
    gpio_isr_handler_add(BUTTON_GPIO, reacted_isr, NULL);

    // notify the user 
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("!!!!!!!!!       Press      !!!!!!!!!\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    xSemaphoreGive(serialMutex);

    // start measurement
    react_time_start = esp_timer_get_time();

    // delete self 
    vTaskDelete(thandle_start_game);
    thandle_start_game = NULL;
}

void IRAM_ATTR start_game_isr(void* arg) {
    /*init the start game task*/

    int64_t now = esp_timer_get_time();
    if (now - debounce_time < DEBOUNCE_DELAY) return;
    debounce_time = now;

    xTaskCreate(
        start_game,
        "start_game",
        2048,
        NULL,
        1,
        &thandle_start_game
    );

    gpio_isr_handler_remove(BUTTON_GPIO);
}

volatile int pressed = 0;
// Function to handle the button press (not used in your current flow)
void IRAM_ATTR button_isr_handler(void* arg) {
    pressed = 1;
    // Perform necessary actions
    gpio_isr_handler_remove(BUTTON_GPIO);
}

extern "C" void app_main(void) {
    // Setup
    serialMutex = xSemaphoreCreateMutex();

    // Configure button GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Falling edge interrupt (button press)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Install ISR service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);  // Correct spelling

    return_to_start();
}
