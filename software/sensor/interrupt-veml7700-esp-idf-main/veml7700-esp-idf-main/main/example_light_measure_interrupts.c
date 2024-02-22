#include <stdio.h>
#include "driver/i2c.h"
#include "veml7700.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "esp_task_wdt.h" // Include the watchdog header
#include "esp_intr_alloc.h" // interrupt handler


#define I2C_MASTER_SCL_IO 22          // GPIO pin for I2C clock
#define I2C_MASTER_SDA_IO 21          // GPIO pin for I2C data
#define I2C_MASTER_NUM I2C_NUM_0      // I2C port number
#define TCA9548_ADDR 0x70             // TCA9548 I2C address
#define CHANNEL_1_BIT 0x02            // Channel 1 selection bit
#define TIMER_INTERVAL_MS 10000 // Timer interval (in milliseconds)
#define TIMER_GROUP TIMER_GROUP_0 // Timer group
#define TIMER_NUM TIMER_0 // Timer number
// Timer interrupt flag
volatile bool timer_int_flag = false;

void i2c_master_init() {
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000; // 100 kHz clock speed
    conf.clk_flags = 0;
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

void select_channel(uint8_t channel) {
    uint8_t cmd = 1 << channel;
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (TCA9548_ADDR << 1) | I2C_MASTER_WRITE, true); // why does this shift TCA9548 left one?
    i2c_master_write_byte(cmd_handle, cmd, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_handle, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd_handle);
}

// ISR function for the timer interrupt
void IRAM_ATTR timer_isr(void* arg) {
    timer_int_flag = true; // Set flag to indicate timer interrupt

    printf("Software interrupt triggered\n");
    select_channel(2); // Select channel 2
    printf("Channel 2 on the TCA9548 is now open.\n\n");

    double lux_als_2 = 0, fc_als_2 = 0;

    veml7700_handle_t veml7700_dev_2;
    esp_err_t veml7700_init_result_2 = veml7700_initialize(&veml7700_dev_2, I2C_MASTER_NUM);

    if (veml7700_init_result_2 != ESP_OK) {
        ESP_LOGE("VEML7700_2", "Failed to initialize VEML7700 (Channel 2). Result: %d\n", veml7700_init_result_2);  
    }
    else
    {
        ESP_ERROR_CHECK(veml7700_read_als_lux_auto(veml7700_dev_2, &lux_als_2));
        fc_als_2 = lux_als_2 * LUX_FC_COEFFICIENT;
        printf("Sensor 2 (Channel 2) - ALS: %0.4f lux, %0.4f fc\n", lux_als_2, fc_als_2);
    }

    select_channel(3); // Select channel 3

    printf("Channel 3 on the TCA9548 is now open.\n");

    double lux_als_3 = 0, fc_als_3 = 0;

    veml7700_handle_t veml7700_dev_3;
    esp_err_t veml7700_init_result_3 = veml7700_initialize(&veml7700_dev_3, I2C_MASTER_NUM);

    if (veml7700_init_result_3 != ESP_OK) {
        ESP_LOGE("VEML7700_3", "Failed to initialize VEML7700 (Channel 3). Result: %d\n", veml7700_init_result_3);  
    }
    else
    {
        ESP_ERROR_CHECK(veml7700_read_als_lux_auto(veml7700_dev_3, &lux_als_3));
        fc_als_3 = lux_als_3 * LUX_FC_COEFFICIENT;
        printf("Sensor 3 (Channel 3) - ALS: %0.4f lux, %0.4f fc\n", lux_als_3, fc_als_3);
    }

    
}

void setup_timer() {
    timer_config_t config = {
        .alarm_en = TIMER_ALARM_EN,
        .counter_en = TIMER_PAUSE,
        .intr_type = TIMER_INTR_LEVEL,
        .counter_dir = TIMER_COUNT_UP,
        .auto_reload = TIMER_AUTORELOAD_EN,
        .divider = 80, // 1 MHz timer clock
    };
    timer_init(TIMER_GROUP, TIMER_NUM, &config);
    timer_set_alarm_value(TIMER_GROUP, TIMER_NUM, TIMER_INTERVAL_MS * 1000); // Convert ms to us
    timer_enable_intr(TIMER_GROUP, TIMER_NUM);
    timer_isr_register(TIMER_GROUP, TIMER_NUM, timer_isr, NULL, ESP_INTR_FLAG_IRAM, NULL);
    timer_start(TIMER_GROUP, TIMER_NUM);
}

void app_main() {
    i2c_master_init();
    printf("ESP32 Initialized for I2C.\n");

    setup_timer();

    while(1){
        // Check if timer interrupt occurred
        if (timer_int_flag) {
            // Reset interrupt flag
            timer_int_flag = false;

            // Your custom logic here (e.g., toggle an LED, print a message)
            printf("Timer interrupt triggered\n");
        }
        vTaskDelay(10 / portTICK_PERIOD_MS); // Delay to reduce CPU usage
    }
}