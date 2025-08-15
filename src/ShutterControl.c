#include "ShutterControl.h"
#include "lwip/ip4_addr.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

esp_http_client_handle_t CreateClient(ip4_addr_t shutter_ip, char* method)
{
    char ip_str[16];
    char url[64];
    snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(&shutter_ip));
    snprintf(url, sizeof(url), "http://%s/rpc/Cover.%s", ip_str, method);
    
    esp_http_client_config_t config = {
        .url = url,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    return client;
}


void SendPostRequest(esp_http_client_handle_t client, const char* payload)
{
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    char post_data[128];
    if (payload && strlen(payload) > 0) {
        snprintf(post_data, sizeof(post_data), "{\"id\":0, %s}", payload);
    } else {
        snprintf(post_data, sizeof(post_data), "{\"id\":0}");
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
#ifdef DEBUG
    if (err == ESP_OK) {
        ESP_LOGI("HTTP", "Status = %d, content_length = %lld\n",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        
        ESP_LOGE("HTTP", "HTTP request failed: %s\n", esp_err_to_name(err));
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(200));
}

void SendGetRequest(esp_http_client_handle_t client, char* buffer, size_t bufferSize)
{
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    char url[128];
    esp_http_client_get_url(client, url, sizeof(url));
    strncat(url, "?id=0", sizeof(url) - strlen(url) - 1);
    esp_http_client_set_url(client, url);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
#ifdef DEBUG
        ESP_LOGE("HTTP", "Failed to open HTTP connectio: %s", esp_err_to_name(err));
#endif
    }
    else
    {
        int contentLength = esp_http_client_fetch_headers(client);
        if (contentLength < 0)
        {
#ifdef DEBUG
            ESP_LOGE("HTTP", "HTTP client fetch headers failed");
#endif
        }
        else
        {
            int dataRead = esp_http_client_read_response(client, buffer, bufferSize - 1);
            buffer[dataRead] = '\0';
        }
    }

    esp_http_client_close(client);
    vTaskDelay(pdMS_TO_TICKS(200));
}

void OpenShutter(ip4_addr_t shutter_ip)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "Open");
    SendPostRequest(client, "");
}

void CloseShutter(ip4_addr_t shutter_ip)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "Close");
    SendPostRequest(client, "");
}

void StopShutter(ip4_addr_t shutter_ip)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "Stop");
    SendPostRequest(client, "");
}

enum ShutterStatus GetShutterStatus(ip4_addr_t shutter_ip)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "GetStatus");

    char buffer[512];
    SendGetRequest(client, buffer, sizeof(buffer));

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
#ifdef DEBUG
        if (error_ptr != NULL) {
            ESP_LOGE("JSON", "JSON Parse Error: %s\n", error_ptr);
        }
#endif
        return SHUTTER_STATUS_UNKNOWN;
    }
    cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "state");

    enum ShutterStatus returnVal;
    if (strcmp(cJSON_GetStringValue(state), "closed") == 0)
    {
        returnVal = SHUTTER_STATUS_CLOSED;
    }
    else if (strcmp(cJSON_GetStringValue(state), "closing") == 0)
    {
        returnVal = SHUTTER_STATUS_CLOSING;
    }
    else if (strcmp(cJSON_GetStringValue(state), "open") == 0)
    {
        returnVal = SHUTTER_STATUS_OPEN;
    }
    else if (strcmp(cJSON_GetStringValue(state), "opening") == 0)
    {
        returnVal = SHUTTER_STATUS_OPENING;
    }
    else if (strcmp(cJSON_GetStringValue(state), "stopped") == 0)
    {
        returnVal = SHUTTER_STATUS_STOPPED;
    }
    else
    {
        returnVal = SHUTTER_STATUS_UNKNOWN;
    }

    return returnVal;
}

void SetShutterPosition(ip4_addr_t shutter_ip, int position)
{
    esp_http_client_handle_t client = CreateClient(shutter_ip, "GoToPosition");
    char positionRequest[12];
    snprintf(positionRequest, sizeof(positionRequest), "\"pos\":%d", position);
    SendPostRequest(client, positionRequest);
}
