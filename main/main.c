/*
 * Author: yolicap
 */

#include <stdio.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/i2c_master.h"
// #include "driver/i2c_types.h"
// #include "driver/i2c.h"
#include "driver/gpio.h"

#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_log.h"

#include "esp_wifi.h"
#include "esp_event.h"

#include "nvs_flash.h"

static const char *TAG = "MINITOP";
static const char *TAG_WIFI = "WIFI_INFO";

// FROM IS31FL3741 STEMMA (LED MATRIX) DOCS:
// fSCL Serial-clock frequency - 400 - 1000 kHz
// TODO export I2C configs to headers file
// TODO export 12C code to external file
// ***** I2C CONFIGS *****
#define I2C_CONTROLLER_SCL_IO GPIO_NUM_8
#define I2C_CONTROLLER_SDA_IO GPIO_NUM_9
#define I2C_CONTROLLER_NUM I2C_NUM_0
#define I2C_CONTROLLER_TX_BUF_DISABLE 0
#define I2C_CONTROLLER_RX_BUF_DISABLE 0   
#define I2C_FREQ_HZ_MAX 1000000 // max speed of IS31FL3741 STEMMA (1000kHz)
#define I2C_FREQ_HZ_MIN 400000 // min speed of IS31FL3741 STEMMA (400kHz)
#define TEMP 100000
#define I2C_CONTROLLER_FREQ_HZ TEMP
#define I2C_CONTROLLER_TIMEOUT_MS       1000

/*
* A0 - 0 to write, 1 to read
* A7:A3 - 01100 hard coded
* A2:A1 - ADDR connected to GND, ADDR = 00
*/
#define IS31FL3741_ADDR_W 0x30
// #define IS31FL3741_ADDR_R 0x61
// #define IS31FL3741_ADDR_W 0x00

// kilometers per second
#define PWN1_R 0x00
#define PWN2_R 0x01
#define LED1_R 0x02
#define LED2_R 0x03
#define FUNC_R 0x04

#define FDH_WRITE_CMD 0xC5

// ***** KEY MATRIX CONFIGS *****
#define ROWS 2
#define COLS 3

// TODO GPIO values are arbitrary as of now. needs to be updated
#define GPIO_OUTPUT_IO_0 21
#define GPIO_OUTPUT_IO_1 22
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))

#define GPIO_INPUT_IO_0 18
#define GPIO_INPUT_IO_1 19
#define GPIO_INPUT_IO_2 20
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1) | (1ULL<<GPIO_INPUT_IO_2))

// TODO
gpio_num_t row_pins[ROWS] = {GPIO_INPUT_IO_0, GPIO_INPUT_IO_1};
gpio_num_t col_pins[COLS] = {GPIO_INPUT_IO_0, GPIO_INPUT_IO_1, GPIO_INPUT_IO_2};

typedef struct {
    uint8_t row;
    uint8_t col;
} key_event_t;

// QueueHandle_t key_event_queue;

// user perspective
typedef struct {
    uint8_t row;
    uint8_t col;
} led_coords_t;

// code perspective
typedef struct {
    uint8_t page;
    uint8_t addr;
} led_mapping_t;

// rgb data
typedef struct {
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;
} rgb_t;

/**
 * @brief led coordinate to page {0, 1} and addr
 * if scaling, offset page by 2
 * row/col 0-indext
 */
void coord_to_mapping(led_coords_t * coords, led_mapping_t * mapping) {
    if (coords->col <= 10u && coords->row <= 6u) {
        mapping->page = 0x00;
        mapping->addr = coords->col + (coords->row * 10u);
    } else {
        mapping->page = 0x01;

        // bottom side of page 1
        if (coords->col <= 10u) {
            mapping->addr = coords->col + ((coords->row - 6u) * 10u);
        } else {
            mapping->addr = ((coords->col - 10u) + (coords->row * 10u)) + 0x59;
        }
    }
}

/**
 * @brief i2c master initialization
 */
void i2c_controller_init(i2c_master_bus_handle_t *bus_handle, i2c_master_dev_handle_t *dev_handle) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_CONTROLLER_NUM,
        .sda_io_num = I2C_CONTROLLER_SDA_IO,
        .scl_io_num = I2C_CONTROLLER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, bus_handle));

    // init IS31FL3741 I2C
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IS31FL3741_ADDR_W,
        .scl_speed_hz = I2C_CONTROLLER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(*bus_handle, &dev_config, dev_handle));
}

/**
 * @brief set brightness of led
 * 
 */
// static esp_err_t set_led_brightness(i2c_master_dev_handle_t dev_handle, uint8_t *data, size_t len) {
//     // get led page and addr
//     // set FEh to write
//     // set FDh to page
//     // set brightness
//     return ESP_OK;
// }

/**
 * @brief Read a byte from a IS31FL3741 registers
 */
static esp_err_t is31fl3741_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data) {
    uint8_t data_tx = reg_addr;
    return i2c_master_transmit_receive(
        dev_handle, 
        &data_tx, 1, 
        data, 1, 
        -1
    );
    // return i2c_master_receive(
    //     dev_handle,
    //     data,
    //     1,
    //     -1
    // ); 
}

/**
 * @brief Write a byte to a IS31FL3741 register
 * FDh Command Register (W)
 * - 0x00, 0x01 PWN Register
 * - 0x02, 0x03 Scaling Register
 * - 0x04 Function Register
 * FEh Command Register Write Lock (R/W)
 * F0h Interrupt Mask Register (W)
 * F1h Interrupt Status Register (R)
 * FCh ID Register  (R)
 * a human wrote this btw
 */
static esp_err_t is31fl3741_register_write(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data) {
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(
        dev_handle, 
        write_buf, 
        2, 
        // I2C_CONTROLLER_TIMEOUT_MS / portTICK_PERIOD_MS
        -1
    );
}

/**
 * @brief set color of led
 * 
 */
static esp_err_t set_led_color(i2c_master_dev_handle_t dev_handle, rgb_t *data, led_coords_t *coords) {
    // get led page and addr
    led_mapping_t mapping;
    coord_to_mapping(coords, &mapping);
    
    // do 3 times
    // set FEh to write: this unlocks FDh register 
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFE, FDH_WRITE_CMD));
    // set FDh to page
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFD, mapping.page));
    // set brightness
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, (mapping.addr), data->led_r));
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, (mapping.addr)+1, data->led_g));
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, (mapping.addr)+2, data->led_b));

    return ESP_OK;
}

// void led_matrix_task(void *arg) {
//     key_event_t event;

//     uint8_t frame[16] = {0};

//     while (1)
//     {
//         if (xQueueReceive(event_queue, &event, portMAX_DELAY))
//         {
//             int index = event.row * COLS + event.col;

//             frame[index] ^= 1;

//             led_matrix_write(frame, sizeof(frame));
//         }
//     }
// }

// void keyscan_task(void *arg) {
//     key_event_t event;

//     while (1) {
//         for (int r = 0; r < ROWS; r++) {
//             // set to low
//             gpio_set_level(row_pins[r], 0);

//             for (int c = 0; c < COLS; c++) {
//                 // if signal low, col is being pressed
//                 if (gpio_get_level(col_pins[c]) == 0) {
//                     event.row = r;
//                     event.col = c;

//                     xQueueSend(key_event_queue, &event, 0);
//                     vTaskDelay(pdMS_TO_TICKS(200)); // debounce
//                 }
//             }
//             // set signal back to high
//             gpio_set_level(row_pins[r], 1);
//         }

//         vTaskDelay(pdMS_TO_TICKS(10));
//     }
// }

void key_matrix_init() {
    gpio_config_t row_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL
    };

    gpio_config(&row_conf);

    // pull up means input always read high unless there is a low signal
    gpio_config_t col_conf = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = GPIO_INPUT_PIN_SEL
    };

    gpio_config(&col_conf);

    for(int i=0;i<ROWS;i++)
        gpio_set_level(row_pins[i],1);
}

esp_err_t led_handler(httpd_req_t *req) {
    char resp[64];

    snprintf(resp, sizeof(resp), "LED command received");

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

void http_server_task(void *arg) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_uri_t uri = {
            .uri = "/led",
            .method = HTTP_GET,
            .handler = led_handler
        };

        httpd_register_uri_handler(server, &uri);
    }

    ESP_LOGI(TAG, "HTTP task done!");
    vTaskDelete(NULL);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("Wi-Fi started! Now attempting to connect...\n");
        esp_wifi_connect(); // This actually triggers the connection
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("Disconnected. Retrying...\n");
        esp_wifi_connect(); // Optional: Auto-reconnect logic
    }else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

esp_err_t wifi_init(){
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_config);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "", // DONT PUSH ME BREH
            .password = "", // DONT PUSH ME BREH
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    esp_wifi_start();
    return ESP_OK;
}

void app_main() {

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    i2c_controller_init(&bus_handle, &dev_handle);

    uint8_t data_rd;
    ESP_ERROR_CHECK(is31fl3741_register_read(dev_handle, 0xFC, &data_rd));
    printf("we good fam. %02x\n", data_rd);
    // ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFE, FDH_WRITE_CMD));

    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFE, FDH_WRITE_CMD));
    // Switch to Page 4 (Function Page)
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFD, 0x04));
    // Set Configuration Register (0x00) to 0x01 (Normal Operation)
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0x00, 0x01));
    // Set Global Current Control (0x01) to 0xFF (Max brightness)
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0x01, 0xFF));

    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0x01, 0xFF)); // Max global current

    // Fill Page 2 (Scaling 1) with 0xFF
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFE, FDH_WRITE_CMD)); // Unlock
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFD, 0x02));          // Page 2
    for (int i = 0; i <= 0xAB; i++) {
        is31fl3741_register_write(dev_handle, i, 0xFF);
    }

    // Fill Page 3 (Scaling 2) with 0xFF
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFE, FDH_WRITE_CMD)); // Unlock
    ESP_ERROR_CHECK(is31fl3741_register_write(dev_handle, 0xFD, 0x03));          // Page 3
    for (int i = 0; i <= 0xAA; i++) {
        is31fl3741_register_write(dev_handle, i, 0xFF);
    }

    // key_matrix_init();
    // key_event_queue = xQueueCreate(10, sizeof(key_event_t));

    // xTaskCreate(
    //     led_matrix_task,
    //     "led_matrix_task",
    //     4096,
    //     NULL,
    //     5,
    //     NULL
    // );

    // xTaskCreate(
    //     keyscan_task,
    //     "keyscan_task",
    //     2048,
    //     NULL,
    //     5,
    //     NULL
    // );

    ESP_ERROR_CHECK(wifi_init());

    // TODO: only if wifi connected
    xTaskCreate(
        http_server_task,
        "http_server_task",
        4096,
        NULL,
        4,
        NULL
    );

    ESP_LOGI(TAG, "app started");

    // ESP_ERROR_CHECK(i2c_master_bus_rm_device(dev_handle));
    // ESP_ERROR_CHECK(i2c_del_master_bus(bus_handle));
    // ESP_LOGI(TAG, "I2C de-initialized successfully");

    uint8_t led = 0;
    led_coords_t coords;
    rgb_t data;
    data.led_r = 0xFF;
    data.led_g = 0xFF;
    data.led_b = 0xFF;
        
    while (true) {
        if (led <= 104) {
            coords.row = led / 13;
            coords.col = led % 13;
            set_led_color(dev_handle, &data, &coords);
        } else {
            led = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        led++;
    }
}