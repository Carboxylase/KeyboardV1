#include <stdlib.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define ROW_0 4
#define ROW_1 5
#define ROW_2 6
#define ROW_3 7

#define COL_0 9
#define COL_1 10
#define COL_2 11
#define COL_3 12

#define ESP_INTR_FLAG_DEFAULT 0

#define POLLING_CYCLE_DURATION 100

static uint32_t activeRowNum = 0;

static QueueHandle_t gpio_evt_queue = NULL;

static void gpio_isr_handler(void* arg);

static void gpio_key_event_process(void *arg);

static void gpio_stimulate_pins(void *args);

/* START TINYUSB DESCRIPTORS*/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD))
};

const char *hid_string_descriptor[5] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
    "TinyUSB",             // 1: Manufacturer
    "TinyUSB Device",      // 2: Product
    "123456",              // 3: Serials, should use chip ID
    "Example HID interface",  // 4: HID
};

static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/* END TINYUSB DESCRIPTORS*/

/* START TINYUSB CALLBACKS*/
// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    // We use only one interface and one HID report descriptor, so we can ignore parameter 'instance'
    return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}

/* END TINYUSB CALLBACKS */

typedef struct keyPressIsrArgs
{
    uint32_t colNum;
    uint32_t *activeRowNum;
}keyPressIsrArgs_t;

typedef struct keyProcessArgs
{
    uint32_t colNum;
    uint32_t activeRowNum;
    TickType_t timeStamp;
}keyProcessArgs_t;

typedef struct gpioStimArgs
{
    uint32_t *rowArray;
    uint32_t rowArrayLen;
}gpioStimArgs_t;

typedef struct keycode
{
    uint8_t keyVal[6];
}keycode_t;

static void gpioInitCol(uint32_t colNum, gpio_config_t * cfg);

static void gpioInitRow(uint32_t rowNum, gpio_config_t * cfg);

static void atomicDelay(uint32_t numCycles);

static bool deltaLowerEps(uint32_t curr, uint32_t prev, uint32_t eps);

void app_main(void) {
    // write your code here
    uint32_t colArray[] = {COL_0, COL_1, COL_2};
    uint32_t rowArray[] = {ROW_0, ROW_1, ROW_2};

    uint32_t colArrayLen = sizeof(colArray) / sizeof(uint32_t);
    uint32_t rowArrayLen = sizeof(rowArray) / sizeof(uint32_t);

    // initialize all columns
    gpio_config_t col0cfg = {};
    gpioInitCol(COL_0, &col0cfg);

    gpio_config_t col1cfg = {};
    gpioInitCol(COL_1, &col1cfg);

    gpio_config_t col2cfg = {};
    gpioInitCol(COL_2, &col2cfg);

    // gpio_config_t col3cfg = {};
    // gpioInitCol(COL_3, &col3cfg);

    printf("Initialized Column GPIO\n");

    // initialize all rows
    gpio_config_t row0cfg = {};
    gpioInitRow(ROW_0, &row0cfg);

    gpio_config_t row1cfg = {};
    gpioInitRow(ROW_1, &row1cfg);

    gpio_config_t row2cfg = {};
    gpioInitRow(ROW_2, &row2cfg);

    gpio_dump_io_configuration(stdout, (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 9) | (1ULL << 10) | (1ULL << 11));

    // gpio_config_t row3cfg = {};
    // gpioInitRow(ROW_3, &row3cfg);
    printf("Initializing Row GPIO\n");

    // add the ISR for the column gpio
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    printf("Added ISR Service\n");

    // setup arguments for column GPIO ISR
    keyPressIsrArgs_t *colArgs[colArrayLen];

    for (uint32_t i = 0; i < colArrayLen; i++)
    {
        colArgs[i] = malloc(sizeof(keyPressIsrArgs_t));
        colArgs[i]->colNum = i; // colArray[i]
        colArgs[i]->activeRowNum = &activeRowNum;
        gpio_isr_handler_add(colArray[i], gpio_isr_handler, (void*) colArgs[i]);
        printf("Added ISR Handler for Col: %ld\n", colArray[i]);
    }

    gpio_evt_queue = xQueueCreate(10, sizeof(keyProcessArgs_t));
    printf("Created GPIO Event Queue\n");

    // create task to read the event queue
    // xTaskCreate(gpio_key_event_process,"KeyEventProcess", 2024, NULL, 10, NULL);
    // printf("Created Processing Task\n");

    gpioStimArgs_t stimArgs;
    stimArgs.rowArray = rowArray;
    stimArgs.rowArrayLen = rowArrayLen;
    xTaskCreate(gpio_stimulate_pins,"StimulatePins", 2024, &stimArgs, 10, NULL);
    printf("Created GPIO Stimulate Task");

    // iterate over the rows and check
    activeRowNum = 0;

    // create the USB config
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = NULL;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    printf("Finished initializing TUSB\n");

    static bool suspended = false;
    static bool wakeup_host = false;

    uint8_t keyMatrix[3][3][6] =    {
                                        {{HID_KEY_A}, {HID_KEY_B}, {HID_KEY_C}},
                                        {{HID_KEY_D}, {HID_KEY_E}, {HID_KEY_F}},
                                        {{HID_KEY_G}, {HID_KEY_H}, {HID_KEY_I}}
                                    };

    uint32_t timeMatrix[3][3] = {   
                                    {0,0,0},
                                    {0,0,0},
                                    {0,0,0}
                                };

    while(1)
    {
        // for (uint32_t i = 0; i < rowArrayLen; i++)
        // {
        //     activeRowNum = i; // declare which row is being stimulated
        //     gpio_set_level(rowArray[i], 1);            
        //     gpio_set_level(rowArray[i], 0);
        //     vTaskDelay(10/portTICK_PERIOD_MS);
        // }

        // ---------------------------------------------------------------------------------

        // activeRowNum = rowArray[0];
        // gpio_set_level(rowArray[0], 1); // test
        //  atomicDelay(POLLING_CYCLE_DURATION);
        // taskYIELD();
        // vTaskDelay(10/portTICK_PERIOD_MS);

        // ---------------------------------------------------------------------------------

        if (tud_mounted())
        {
            static bool send_hid_data = true;

            // printf("USB Mounted\n");

            if (send_hid_data)
            {
                if (!suspended)
                {
                    keyProcessArgs_t keyProcessArgs;
                    
                    if (xQueueReceive(gpio_evt_queue,&keyProcessArgs,portMAX_DELAY))
                    {
                        // read the row and col value and then send data to USB for the key
                        // printf("Processing - Row: %lu | Col: %lu\n",keyProcessArgs.activeRowNum,keyProcessArgs.colNum);

                        // get the mapping of the 
                        // uint8_t keycode[6] = keyMatrix[keyProcessArgs.activeRowNum][keyProcessArgs.colNum];
                        uint8_t keycode[6];
                        memcpy(keycode, keyMatrix[keyProcessArgs.activeRowNum][keyProcessArgs.colNum], sizeof(uint8_t)*6);

                        if (!deltaLowerEps(keyProcessArgs.timeStamp, timeMatrix[keyProcessArgs.activeRowNum][keyProcessArgs.colNum], 7.5)
                        && gpio_get_level(keyProcessArgs.colNum))
                        {

                            printf(" >> Sending %ld + %ld | Processed @ %ld\n", keyProcessArgs.activeRowNum, keyProcessArgs.colNum, keyProcessArgs.timeStamp);

                            tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycode);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
                        }

                        timeMatrix[keyProcessArgs.activeRowNum][keyProcessArgs.colNum] = keyProcessArgs.timeStamp;
                    }
                }
                else
                {
                    if (wakeup_host)
                    {
                        tud_remote_wakeup();
                        wakeup_host = false;
                        printf("Wokeup host\n");
                    }
                    else
                    {
                        printf("Unable to wakeup host\n");
                    }
                }
            }
            
        }

        vTaskDelay(10/portTICK_PERIOD_MS);

    }

    // return 0;
}

static void gpio_isr_handler(void* arg)
{
    keyPressIsrArgs_t *keyPressArgs= (keyPressIsrArgs_t*) arg;

    keyProcessArgs_t keyProcessArgs;
    keyProcessArgs.colNum = keyPressArgs->colNum;
    keyProcessArgs.activeRowNum = *(keyPressArgs->activeRowNum);
    keyProcessArgs.timeStamp = xTaskGetTickCountFromISR();

    xQueueSendFromISR(gpio_evt_queue,&keyProcessArgs,NULL);
    // printf("Adding gpio");
}

static void gpio_key_event_process(void *arg)
{
    keyProcessArgs_t keyProcessArgs;

    while (1)
    {
        if (xQueueReceive(gpio_evt_queue,&keyProcessArgs,portMAX_DELAY))
        {
            // read the row and col value and then send data to USB for the key
            printf("Processing - Row: %lu | Col: %lu\n",keyProcessArgs.activeRowNum,keyProcessArgs.colNum);
        }
        taskYIELD();
    }
}

static void gpio_stimulate_pins(void *args)
{

    gpioStimArgs_t *stimArgs = (gpioStimArgs_t*)args;
    while(1)
    {
        for (uint32_t i = 0; i < stimArgs->rowArrayLen; i++)
        {
            activeRowNum = i; //stimArgs->rowArray[i]; // declare which row is being stimulated
            gpio_set_level(stimArgs->rowArray[activeRowNum], 1);            
            gpio_set_level(stimArgs->rowArray[activeRowNum], 0);
            // vTaskDelay(10/portTICK_PERIOD_MS);
            // taskYIELD();
        }
        // taskYIELD();
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

static void gpioInitRow(uint32_t rowNum, gpio_config_t *cfg)
{
    cfg->intr_type = GPIO_INTR_DISABLE;
    cfg->mode = GPIO_MODE_OUTPUT;
    cfg->pin_bit_mask = 1ULL << rowNum;
    cfg->pull_up_en = GPIO_PULLUP_DISABLE;
    cfg->pull_down_en = GPIO_PULLDOWN_ENABLE;

    gpio_config(cfg);
}

static void gpioInitCol(uint32_t colNum, gpio_config_t * cfg)
{
    cfg->intr_type = GPIO_INTR_POSEDGE;
    cfg->mode = GPIO_MODE_INPUT;
    cfg->pin_bit_mask = 1ULL << colNum;
    cfg->pull_up_en = GPIO_PULLUP_DISABLE;
    cfg->pull_down_en = GPIO_PULLDOWN_ENABLE;

    gpio_config(cfg);
}

static void atomicDelay(uint32_t numCycles)
{
    uint32_t cycleCount = 0;
    for (cycleCount = 0; cycleCount < numCycles; cycleCount++)
    {
        asm("nop");
    }
}

static bool deltaLowerEps(uint32_t curr, uint32_t prev, uint32_t eps)
{
    printf("Curr time: %ld | Prev time: %ld\n", curr, prev);
    uint32_t diff = curr - prev;

    if (diff < eps)
    {
        return true;
    }
    else
    {
        // printf("Curr time: %ld | Prev time: %ld\n", curr, prev);
        return false;
    }
}
