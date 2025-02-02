// YOU NEED TO DISABLE WATCHDOG TIMER IN MENUCONFIG TO RUN THIS CODE
// Component config -> ESP System settings -> Disable Task Watchdog Timer

// includes Vishay VEML7700 Light Sensor driver for integration with ESP-IDF framework by Kristijan Grozdanovski
// Copyright (c) 2022, Kristijan Grozdanovski

#include <stdio.h>
#include <math.h>
#include "driver/i2c.h"
#include "veml7700.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
// zigbee libraries
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managed_components/espressif__esp-zigbee-lib/include/ha/esp_zigbee_ha_standard.h"
#include "managed_components/espressif__esp-zigbee-lib/include/esp_zb_light.h"
#include "nvs_flash.h"

#define I2C_MASTER_SCL_IO 10            // GPIO for sensor I2C clock
#define I2C_MASTER_SDA_IO 11            // GPIO for sensor I2C data
#define I2C_MASTER_NUM I2C_NUM_0
#define TCA9548_ADDR 0x71
//-----------------------------------------------------------------------------------------
#define USBC_PIN GPIO_NUM_12 // Define GPIO pin 12
#define USBA_PIN GPIO_NUM_1 // Define GPIO pin 1
#define STEP_PIN GPIO_NUM_22        // CLK pin 22
#define DIR_PIN GPIO_NUM_23         // DIR pin 23

// PCA9685 I2C address
#define PCA9685_ADDR    0x40

// Registers
#define PCA9685_MODE1   0x00
#define PCA9685_PRESCALE 0xFE
#define LED0_ON_L       0x06
#define LED0_ON_H       0x07
#define LED0_OFF_L      0x08
#define LED0_OFF_H      0x09

#define SERVO_NUM       3

// I2C Configuration
#define I2C_PORT        0
#define I2C_FREQ_HZ     50000 // 20kHz

// Configuration for servo motors
#define SERVO_FREQ_HZ   50
#define SERVO_MIN_US    500
#define SERVO_MAX_US    2500
#define SERVO_MIN_AN    90
#define SERVO_MAX_AN    360

// global variables to store sensor and mirror values
float angle_spin = 0.0;
float save_value = 0.0;
float angle_val = 0.0;
float new_mirror_vert = 0.0;
float new_mirror_horiz = 0.0;

int target_num = 0; // target number
int dir = 1; // direction of stepper
int target_change_var = 0; // boolean that ignores thresholds when changing targets

// global variables to store stepper values
float current_stepper_val = 0.0;
float stepper_save_value = 0.0;
float stepper_val_new = 0.0;

// target location arrays
float target_val_servo[] = {600,450};
float target_val_stepper[] = {0,720};

// function prototype for slow servo spin
void set_servo_angle_loop(float new_value, float current_value, int channel);

//-------------------------ZIGBEE------------------------------------
/**
 * @note Make sure set idf.py menuconfig in zigbee component as zigbee end device!
*/
#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

// manufacturer info
char modelid[] = {11, 'E', 'S', 'P', '3', '2', 'C', '6', '.', 'E', 'n', 'd'};
char manufname[] = {9, 'E', 's', 'p', 'r', 'e', 's', 's', 'i', 'f'};

static const char *TAG = "ASTROCOM";
/********************* Define functions **************************/
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void attr_cb(uint8_t status, uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id, void *new_value)
{
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        uint8_t value = *(uint8_t *)new_value;
        if (attr_id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            /* implemented light on/off control */
            if (!target_num)
            {
                target_change_var++;
                target_num++;
                ESP_LOGI(TAG, "set to target 1");
                gpio_set_level(20, 1);
            }
            else if (target_num)
            {
                new_mirror_horiz -= 20;
                target_change_var++;
                target_num--;
                ESP_LOGI(TAG, "set to target 0");
                gpio_set_level(20, 0);
            }
            light_driver_set_power((bool)value);
        }
    } else {
        /* Implement some actions if needed when other cluster changed */
        ESP_LOGI(TAG, "cluster:0x%x, attribute:0x%x changed ", cluster_id, attr_id);
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Start network steering");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            /* commissioning failed */
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %d)", err_status);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %d)", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %d, status: %d", sig_type, err_status);
        break;
    }
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack with Zigbee end-device config */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    /* set the on-off light device config */
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *esp_zb_on_off_light_ep = esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);
    // make cluster + send info
    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_BASIC);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, &modelid[0]);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, &manufname[0]);
    //
    esp_zb_device_register(esp_zb_on_off_light_ep);
    esp_zb_device_add_set_attr_value_cb(attr_cb);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}
//------------------------------END OF ZIGBEE---------------------------------------------------

//------------------------------SERVO MUX FUNCTIONS---------------------------------------------
void pca9685_write_byte(i2c_port_t i2c_num, uint8_t reg_addr, uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCA9685_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(i2c_num, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
}

void pca9685_set_pwm(i2c_port_t i2c_num, uint8_t channel, uint16_t on_time, uint16_t off_time) {
    uint8_t reg_addr = LED0_ON_L + 4 * channel;
    uint8_t on_l = on_time & 0xFF;
    uint8_t on_h = (on_time >> 8) & 0x0F;
    uint8_t off_l = off_time & 0xFF;
    uint8_t off_h = (off_time >> 8) & 0x0F;

    pca9685_write_byte(i2c_num, reg_addr, on_l);
    pca9685_write_byte(i2c_num, reg_addr + 1, on_h);
    pca9685_write_byte(i2c_num, reg_addr + 2, off_l);   
    pca9685_write_byte(i2c_num, reg_addr + 3, off_h);

}

void pca9685_init(i2c_port_t i2c_num) {
    pca9685_write_byte(i2c_num, PCA9685_MODE1, 0x00); // Reset PCA9685

    // Set PWM frequency
    uint8_t prescale_val = (uint8_t)(25000000 / (4096 * SERVO_FREQ_HZ) - 1);
    pca9685_write_byte(i2c_num, PCA9685_PRESCALE, prescale_val);

    // Restart PCA9685
    pca9685_write_byte(i2c_num, PCA9685_MODE1, 0xA1);
    vTaskDelay(pdMS_TO_TICKS(5)); // Delay at least 500us after restart

    // Set all channels off initially
    for (int i = 0; i < SERVO_NUM; i++) {
        pca9685_set_pwm(i2c_num, i, 0, 0);
    }
}
//-----------------------END OF SERVO MUX FUNCTIONS---------------------------------------------

//------------------------------SERVO CTL FUNCTIONS---------------------------------------------
void set_servo_angle(i2c_port_t i2c_num, uint8_t channel, double angle) {
    // Calculate pulse length
    uint16_t pulse = (uint16_t)(SERVO_MIN_US + (angle / 180.0) * (SERVO_MAX_US - SERVO_MIN_US));
    uint16_t pwm_val = (uint16_t)(pulse * 4096 / 20000); // 20ms (50Hz) period for servos

    // Set PWM for the servo channel
    pca9685_set_pwm(i2c_num, channel, 0, pwm_val);
}

void set_servo_angle_loop(float new_value, float current_value, int channel)
{
    if(new_value < current_value)
    {
        for(int i=current_value; i>=new_value; i--){
            set_servo_angle(I2C_PORT, channel, i);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    else if(new_value > current_value)
    {
        for(int i=current_value; i<=new_value; i++){
            set_servo_angle(I2C_PORT, channel, i);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    } 
}
//------------------------END OF SERVO CTL FUNCTIONS---------------------------------------------

//-------------------------------------------------------------------------------
// timer interrupt handler prototype
void timer_isr(void* arg);

// structure of light sensor
typedef struct {
    veml7700_handle_t dev;
    double lux;
    double fc;
} VEML_Data;

// I2C init function
void i2c_master_init() {
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_FREQ_HZ; // 100 kHz clock speed
    conf.clk_flags = 0;
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// sensor mux channel select
void select_channel(uint8_t channel) {
    uint8_t cmd = 1 << channel;
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (TCA9548_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_handle, cmd, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_handle, 1000 / portTICK_PERIOD_MS);
}

// sensor init function
void initialize_veml_device(VEML_Data *data, int channel) {
    esp_err_t init_result = veml7700_initialize(&(data->dev), I2C_MASTER_NUM);
    if (init_result != ESP_OK) {
        ESP_LOGE("VEML7700", "Failed to initialize VEML7700 (Channel %d). Result: %d\n", channel, init_result);
    }
    else{
        ESP_LOGI("VEML7700", "Successfully initialized VEML7700 (Channel %d). Result: %d\n", channel, init_result);
    }
}
//-----------------------------------------------------------------------

//Set stepper task
void stepper_task(float val) {  
    esp_rom_gpio_pad_select_gpio(STEP_PIN);         //selecting CLK pin
    esp_rom_gpio_pad_select_gpio(DIR_PIN);          //selecting DIR pin
    gpio_set_direction(STEP_PIN, GPIO_MODE_OUTPUT); //set CLK pin to output
    gpio_set_direction(DIR_PIN, GPIO_MODE_OUTPUT);  //set DIR pin to output
    //Pull USBC pin up to switch on power to Servos
    esp_rom_gpio_pad_select_gpio(USBC_PIN);
    gpio_set_direction(USBC_PIN, GPIO_MODE_OUTPUT);
    //gpio_set_pull_mode(USBC_PIN, GPIO_PULLUP_ONLY);
    gpio_set_level(USBC_PIN,1);
    val = 1.0/val;
    printf("val after inverse is %.4f\n\n", val);
    val = pow(val,8); // set to 8th power to adjust accurately to sensor values
    // if above 200 reset to 200
    if(val >= 200)
    {
        val = 200;
    }
    current_stepper_val = 0.0;
    ESP_LOGI(TAG, "old horiz value of target is %.4f", target_val_stepper[0]);
    target_val_stepper[0] = ((dir*val) + target_val_stepper[0]);
    ESP_LOGI(TAG, "new horiz value of target is %.4f", target_val_stepper[0]);
    target_val_stepper[1] = ((dir*val) + target_val_stepper[1]);
    printf("\n\nstep size is %.4f\n\n", val);
    for (int i = 0; i < val; i++) { // 200 steps for one revolution
        gpio_set_level(STEP_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(10)); // Adjust delay
        gpio_set_level(STEP_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(10)); // Adjust delay
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // Delay between rotations

    esp_rom_gpio_pad_select_gpio(USBA_PIN);
    gpio_set_direction(USBA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(USBA_PIN, 1);

    return;
}

// main function
void app_main() {
    esp_rom_gpio_pad_select_gpio(USBA_PIN);
    gpio_set_direction(USBA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(USBA_PIN, 1); // power switch **sensor**

    gpio_reset_pin(USBC_PIN);
    esp_rom_gpio_pad_select_gpio(20);
    gpio_set_direction(20, GPIO_MODE_OUTPUT);

    i2c_master_init();
    pca9685_init(I2C_PORT);
    printf("ESP32 Initialized for I2C.\n");


    //----------------------ZIGBEE---------------------------
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    /* load Zigbee light_bulb platform config to initialization */
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    /* hardware related and device init */
    light_driver_init(0);
    //----------------------ZIGBEE---------------------------

    VEML_Data veml_devices[] = {
        { .dev = NULL, .lux = 0, .fc = 0 },  // Data for channel 0 (index 0)
        { .dev = NULL, .lux = 0, .fc = 0 },  // Data for channel 1 (index 1)
        { .dev = NULL, .lux = 0, .fc = 0 },  // Data for channel 2 (index 2)
        { .dev = NULL, .lux = 0, .fc = 0 },  // Data for channel 3 (index 3)
        { .dev = NULL, .lux = 0, .fc = 0 },  // Data for channel 4 (index 4)
    };
    int num_veml_devices = sizeof(veml_devices) / sizeof(VEML_Data);

    for (int i = 0; i < num_veml_devices; i++) { // Start from index 0
        select_channel(i); // Channel index starts from 0
        initialize_veml_device(&veml_devices[i], i); // Initialize VEML device for this channel
    }

    // Initialize timer
    esp_timer_create_args_t timer_args = {
        .callback = timer_isr,
        .name = "software_timer",
        .arg = &veml_devices
    };
    esp_timer_handle_t software_timer;
    esp_timer_create(&timer_args, &software_timer);

    //ZIGBEE-------------------------------------------------
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
    //ZIGBEE-------------------------------------------------


    //-----------------STARTING VALS FOR MOTORS--------------
    save_value = 600;
    set_servo_angle_loop(600,495, 0);
    ESP_LOGI(TAG, "set sensor servo to 600");

    /*set_servo_angle_loop(510, 510, 1);
    set_servo_angle_loop(600, 600, 2);
    ESP_LOGI(TAG, "set mirror horizontal servo to 520 and vertical to 600");
    vTaskDelay(pdMS_TO_TICKS(1000));

    set_servo_angle_loop(459, 510, 1);
    set_servo_angle_loop(450, 600, 2);
    ESP_LOGI(TAG, "set mirror horizontal servo to 459 and vertical to 450");
    vTaskDelay(pdMS_TO_TICKS(2000));*/
    //-----------------STARTING VALS FOR MOTORS--------------

    // Start timer
    esp_timer_start_periodic(software_timer, 5000000); // 5s timer

    // infinite loop
    while (1) {
        
    }
}

// interrupt handler
void timer_isr(void* arg) {
    printf("Software interrupt timer ISR\n");
    VEML_Data* veml_devices = (VEML_Data*)arg;
    for (int i = 0; i < 5; i++) { // Start from index 0
        select_channel(i); // Channel index starts from 0
        printf("Channel %d on the TCA9548 is now open.\n", i);

        // Read VEML data
        if (veml_devices[i].dev != NULL) {
            double lux = 0, fc = 0;
            esp_err_t read_result = veml7700_read_als_lux_auto(veml_devices[i].dev, &lux);
            if (read_result == ESP_OK) {
                fc = lux * LUX_FC_COEFFICIENT;
                veml_devices[i].lux = lux;
                veml_devices[i].fc = fc;
                printf("Sensor %d (Channel %d) - ALS: %0.4f lux, %0.4f fc\n", i, i, lux, fc);
            } else {
                ESP_LOGE("VEML7700", "Failed to read data from VEML7700 (Channel %d). Result: %d\n", i, read_result);
            }
        } else {
            ESP_LOGE("VEML7700", "VEML7700 device not initialized for Channel %d\n", i);
        }
    }

    // calculate ratios of sensor
    float servo_spin_down = (((veml_devices[0].lux + veml_devices[3].lux)/2) / ((veml_devices[1].lux + veml_devices[2].lux)/2)); // ratio for down movement
    float servo_spin_up = (((veml_devices[1].lux + veml_devices[2].lux)/2) / ((veml_devices[0].lux + veml_devices[3].lux)/2)); // ratio for up movement
    float stepper_spin_clockwise = (((veml_devices[2].lux + veml_devices[3].lux)/2) / ((veml_devices[0].lux + veml_devices[1].lux)/2)); // ratio for clkwise stepper
    float stepper_spin_counterclockwise = (((veml_devices[0].lux + veml_devices[1].lux)/2) / ((veml_devices[2].lux + veml_devices[3].lux)/2)); // ratio for cntrclkwise stepper
    float servo_spin_down_inv = 1/servo_spin_down; // inverse of down
    float servo_spin_up_inv = 1/servo_spin_up; // inverse of up
    if(servo_spin_down_inv > 20) // reset to 20 if bigger
        servo_spin_down_inv = 20;
    if(servo_spin_up_inv > 20) // reset to 20 if bigger
        servo_spin_up_inv = 20;

    printf("\n\n");
    if(stepper_spin_counterclockwise < 1)
    {
        printf("counterclockwise is %.4f\n", stepper_spin_counterclockwise);
    }
    if(stepper_spin_clockwise < 1)
    {
        printf("clockwise is %.4f\n", stepper_spin_clockwise);
    }

    if(servo_spin_down < 0.8 || (target_change_var == 1 && servo_spin_down < 1)) // sensor needs to be moved down
    {
        printf("Servo value is %.4f\n", servo_spin_down);
        if((save_value - servo_spin_down_inv) < 495)
        {
            printf("hit max range at 495, horizontal\n");
            angle_spin = 495;
        }
        else{
            angle_spin = save_value - servo_spin_down_inv;
        }
        
        printf("setting servo step to %.4f\n\n", angle_spin);
        printf("\n\nservo begins spinning\n"); 
        set_servo_angle_loop(angle_spin, save_value, 0);
        printf("servo done spinning\n\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        target_change_var = 0;
    }
    else if(servo_spin_up < 0.8 || (target_change_var == 1 && servo_spin_up < 1)) // sensor needs to be moved up
    {
        printf("Servo value is %.4f\n", servo_spin_up);
        if((save_value + servo_spin_up_inv) > 765)
        {
            printf("hit max range at 765, vertical\n");
            angle_spin = 765;
        }
        else
        {
            angle_spin = save_value + servo_spin_up_inv;
        }
        printf("setting servo step to %.4f\n\n", angle_spin);  
        printf("\n\nservo begins spinning\n"); 
        set_servo_angle_loop(angle_spin, save_value, 0);
        printf("servo done spinning\n\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        target_change_var = 0;
    }
    if(stepper_spin_clockwise < 0.9 || (target_change_var == 1 && stepper_spin_clockwise < 1)) // stepper needs to be moved clockwise
    {
         printf("clockwise is %.4f\n", stepper_spin_clockwise);
         gpio_set_level(DIR_PIN, 1); // Set direction 1 clockwise
         ESP_LOGI(TAG, "dir is %d-----------------------------", dir);
         if(dir < 0)
         {
            dir *= -1;
            ESP_LOGI(TAG, "dir changes");
         }
         printf("\n\nstepper begins spinning\n");  
         stepper_task(stepper_spin_clockwise);
         printf("stepper done spinning\n\n"); 
         target_change_var = 0;
    }
    else if(stepper_spin_counterclockwise < 0.9 || (target_change_var == 1 && stepper_spin_counterclockwise < 1)) // stepper needs to be moved counterclockwise
    {
        printf("counterclockwise is %.4f\n", stepper_spin_counterclockwise);
        gpio_set_level(DIR_PIN, 0); // Set direction 0 counterclockwise
        ESP_LOGI(TAG, "dir is %d-----------------------------", dir);
        if(dir != -1)
        {
            dir *= -1;
            ESP_LOGI(TAG, "dir changes");
        }

        printf("\n\nstepper begins spinning\n");  
        stepper_task(stepper_spin_counterclockwise);
        printf("stepper done spinning\n\n");  
        target_change_var = 0;
    }

    printf("angle val is %.4f\n\n", angle_val);

    // scale vertical movement of sensor position to mirror
    float new_servo_exchange = (((angle_spin - 495)*(400-600))/(765-495)) + 600;
    float old_servo_exchange = (((save_value - 495)*(400-600))/(765-495)) + 600;

    // set values to half of target and sensor range
    float servo_vertical_pos_new = (new_servo_exchange + target_val_servo[target_num])/2;
    float servo_vertical_pos_old = (old_servo_exchange + target_val_servo[target_num])/2;
    new_mirror_vert = servo_vertical_pos_new;

    ESP_LOGI(TAG, "target val servo is %.4f", target_val_servo[target_num]);

    // if past furthest range reset
    if(servo_vertical_pos_new > 600)
    {
        servo_vertical_pos_new = 600;
        servo_vertical_pos_old = 600;
        ESP_LOGI(TAG, "GOT PAST 600");
    }

    // move mirror with servo function
    set_servo_angle_loop(servo_vertical_pos_new, servo_vertical_pos_old, 2);
    ESP_LOGI(TAG, "set servo to %.4f", servo_vertical_pos_new);

    
    // new stepper value is half of the target, since sensor is always at 0
    stepper_val_new = target_val_stepper[target_num]/2;

    // scale values to mirror range
    float scaled_stepper_val_new = ((stepper_val_new*(300-510))/1440) + 510;
    float scaled_stepper_val_old = ((stepper_save_value*(300-510))/1440) + 510;
    
    // if past max range reset to max range
    if(scaled_stepper_val_new > 510)
    {
        scaled_stepper_val_new = 510;
        scaled_stepper_val_old = 510;
        ESP_LOGI(TAG, "GOT PAST 510");
    }
    ESP_LOGI(TAG, "horizontal mirror servo set from %.4f to %.4f", scaled_stepper_val_old, scaled_stepper_val_new);


    // move mirror with servo function
    set_servo_angle_loop(scaled_stepper_val_new, scaled_stepper_val_old, 1);

    // save old values of servos
    stepper_save_value = stepper_val_new;
    new_mirror_horiz = stepper_save_value;
    save_value = angle_spin;
}
