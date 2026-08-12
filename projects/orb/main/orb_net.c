#include "orb_net.h"

#include "freertos/semphr.h"

static SemaphoreHandle_t s_http_lock;

esp_err_t orb_net_init(void)
{
    if (s_http_lock) return ESP_OK;
    s_http_lock = xSemaphoreCreateMutex();
    return s_http_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool orb_net_take(TickType_t timeout)
{
    return s_http_lock && xSemaphoreTake(s_http_lock, timeout) == pdTRUE;
}

void orb_net_give(void)
{
    if (s_http_lock) xSemaphoreGive(s_http_lock);
}
