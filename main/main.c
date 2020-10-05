/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h"

/* Note: ESP32 don't support temperature sensor */
#include "driver/temp_sensor.h"

#define ESP_WIFI_SSID      		""		// Please insert your SSID
#define ESP_WIFI_PASS      		""		// Please insert your password
#define ESP_WIFI_AUTH_MODE		WIFI_AUTH_WPA2_PSK // See esp_wifi_types.h
#define ESP_WIFI_MAX_RETRY 		5U

#define MAX_HTTP_RECV_BUFFER 	512U
#define MAX_HTTP_OUTPUT_BUFFER 	2048U

#define THETHINGSIO_TOKEN_ID 	""		// Please insert your TOKEN ID
#define DEEP_SLEEP_TIMEOUT		300000000U	// In microseconds

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static ip_event_got_ip_t* event = NULL;
static uint8_t u8RetryCounter = 0U;

static const char *pcTAG = "TTIO_HTTP_CLIENT";

static float fTemperature = 0.0f;

static void WIFI_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{

	if (event_base == WIFI_EVENT)
	{
		switch (event_id)
		{
			case WIFI_EVENT_STA_START:
				esp_wifi_connect();
				break;
			case WIFI_EVENT_STA_DISCONNECTED:
				if (u8RetryCounter < ESP_WIFI_MAX_RETRY)
				{
					esp_wifi_connect();
					u8RetryCounter++;
					ESP_LOGI(pcTAG, "Retry to connect to the access point");
				}
				else
				{
					xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
					ESP_LOGI(pcTAG,"Connect to the access point fail");
				}
				break;
			default:
				// Do nothing (see WiFi event declarations in the esp_wifi_types.h)
				break;
		}
	}
	else if (event_base == IP_EVENT)
	{
		switch (event_id)
		{
			case IP_EVENT_STA_GOT_IP:
				event = (ip_event_got_ip_t*) event_data;
				u8RetryCounter = 0U;
				xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
				ESP_LOGI(pcTAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
				break;
			default:
				// Do nothing (see WiFi event declarations in the esp_netif_types.h)
				break;
		}
	}

}

esp_err_t HTTP_event_handler(esp_http_client_event_t *event)
{

    static char *pcBuffer;  		// Buffer to store response of http request from event handler
    static uint16_t u16BufferLen;   // Stores number of bytes read

    int mbedtls_err = 0U;
    esp_err_t err = 0U;

    switch (event->event_id)
    {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(pcTAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(pcTAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(pcTAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(pcTAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", event->header_key, event->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(pcTAG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(event->client))
            {
                // If user_data buffer is configured, copy the response into the buffer
                if (event->user_data)
                {
                    memcpy(event->user_data + u16BufferLen, event->data, event->data_len);
                }
                else
                {
                    if (pcBuffer == NULL)
                    {
                        pcBuffer = (char *) malloc(esp_http_client_get_content_length(event->client));
                        u16BufferLen = 0U;

                        if (pcBuffer == NULL) {
                            ESP_LOGE(pcTAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(pcBuffer + u16BufferLen, event->data, event->data_len);
                }
                u16BufferLen += event->data_len;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(pcTAG, "HTTP_EVENT_ON_FINISH");
            if (pcBuffer != NULL)
            {
                // Response is accumulated in pcBuffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, pcBuffer, u16BufferLen);
                free(pcBuffer);
                pcBuffer = NULL;
                u16BufferLen = 0U;
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(pcTAG, "HTTP_EVENT_DISCONNECTED");
            err = esp_tls_get_and_clear_last_error(event->data, &mbedtls_err, NULL);

            if (err != 0U)
            {
                if (pcBuffer != NULL)
                {
                    free(pcBuffer);
                    pcBuffer = NULL;
                    u16BufferLen = 0U;
                }
                ESP_LOGI(pcTAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(pcTAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
        default:
        	// Do nothing
        	break;
    }

    return ESP_OK;

}

void wifi_init_sta(void)
{

	wifi_init_config_t sWifiInitCfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;

	EventBits_t WifiEventBits = 0U;

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&sWifiInitCfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WIFI_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WIFI_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t sWifiConfig = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = ESP_WIFI_AUTH_MODE,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sWifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(pcTAG, "Wi-Fi initializated");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    WifiEventBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (WifiEventBits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(pcTAG, "Connected to access point SSID: %s, Password: %s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (WifiEventBits & WIFI_FAIL_BIT) {
        ESP_LOGI(pcTAG, "Failed to connect to SSID: %s, Password: %s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(pcTAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));

    vEventGroupDelete(s_wifi_event_group);

}

void TempSensor_Task(void *pvParameters)
{

	temp_sensor_config_t sTemperatureInitCfg = TSENS_CONFIG_DEFAULT();

	esp_err_t err = 0U;

    // Initialize temperature sensor
    ESP_LOGI(pcTAG, "Initializing Temperature sensor");
    // Configure temperature sensor
    temp_sensor_set_config(sTemperatureInitCfg);
    // Start sampling
    temp_sensor_start();
    ESP_LOGI(pcTAG, "Temperature sensor started");

    while (1)
	{
    	err = temp_sensor_read_celsius(&fTemperature);

    	if (err == ESP_OK)
    	{
    		ESP_LOGI(pcTAG, "Temperature out Celsius: %2.2f", fTemperature);
    		// Stop sampling
    		temp_sensor_stop();
    		ESP_LOGI(pcTAG, "Temperature sensor stopped");
    		break;
    	}
    }

    ESP_LOGI(pcTAG, "Finish Temperature requests");
    vTaskDelete(NULL);

}

static void HTTP_Task(void *pvParameters)
{
	const char acThingAddr[] = "https://api.thethings.io/v2/things/" THETHINGSIO_TOKEN_ID;
	const char acThingData[] = "{\"values\":[{\"key\":\"Temperature\",\"value\":\"25.00\"}]}";

	char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};

    esp_http_client_config_t sHttpClientCfg = {
    	.host = "thethings.io",
		.url = (const char *)acThingAddr,
		.transport_type = HTTP_TRANSPORT_OVER_TCP,
		.port = 80,
		.method = HTTP_METHOD_POST,
		.event_handler = HTTP_event_handler,
		.user_data = local_response_buffer,        	// Pass address of local buffer to get response
//		.is_async = true,
	};

    // Initialize HTTP protocol
	ESP_LOGI(pcTAG, "Initializing HTTP protocol");
	// Initialize HTTP client object
	esp_http_client_handle_t sHttpClient = esp_http_client_init(&sHttpClientCfg);

	esp_err_t err = 0U;

	esp_http_client_set_header(sHttpClient, "Content-Type", "application/json");

    vTaskDelay(100U / portTICK_RATE_MS);

    while (1)
    {
    	sprintf((char *)acThingData, "{\"values\":[{\"key\":\"Temperature\",\"value\":\"%2.2f\"}]}", fTemperature);

    	esp_http_client_set_post_field(sHttpClient, acThingData, strlen(acThingData));
    	// Perform the transfer as described in the options (blocking)
    	err = esp_http_client_perform(sHttpClient);
    	// Close connection
		esp_http_client_close(sHttpClient);
		// Check perform result
        if (err == ESP_OK)
        {
			// Show a success transfer message
            ESP_LOGI(pcTAG, "HTTP POST Status = %d, content_length = %d",
                    esp_http_client_get_status_code(sHttpClient),
                    esp_http_client_get_content_length(sHttpClient));
        	// Enable wakeup by timer
        	esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIMEOUT);
        	// Show a warning message
        	ESP_LOGI(pcTAG, "Entering in deep sleep...\n");
        	// Wait for a little delay before entering deep sleep
			vTaskDelay(50U / portTICK_RATE_MS);
        	// Enter deep sleep
        	esp_deep_sleep_start();
        }
        else
        {
			// Show an error transfer message
            ESP_LOGE(pcTAG, "HTTP POST request failed: %s", esp_err_to_name(err));
            // Wait for a little delay
            vTaskDelay(100U / portTICK_RATE_MS);
        }

    }

    ESP_LOGI(pcTAG, "Finish HTTP requests");
    vTaskDelete(NULL);

}

void app_main(void)
{

	// Initialize NVS
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Initialize station mode
    wifi_init_sta();
    // Check wakeup cause
	switch (esp_sleep_get_wakeup_cause())
	{
		case ESP_SLEEP_WAKEUP_TIMER:
			ESP_LOGI(pcTAG, "Wake up from timer\n");
			break;
		case ESP_SLEEP_WAKEUP_UNDEFINED:
		default:
			ESP_LOGI(pcTAG, "Not a deep sleep reset\n");
			break;
	}

    xTaskCreate(TempSensor_Task, "tempsensor_task", 2048U, NULL, 5U, NULL);
    xTaskCreate(&HTTP_Task, "http_task", 8192U, NULL, 5U, NULL);

}

