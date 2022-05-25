#include "reOTA.h"
#include <string.h>
#include "reEvents.h"
#include "rLog.h"
#include "reEsp32.h"
#include "rStrings.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "project_config.h"
#include "def_consts.h"
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

static const char* logTAG = "OTA";
static const char* otaTaskName = "ota";
static TaskHandle_t _otaTask = nullptr;

static void otaTaskWatchdog(void* arg)
{
  espRestart(RR_OTA_TIMEOUT, 0); 
}

void otaTaskExec(void *pvParameters)
{
  if (pvParameters) {
    char* otaSource = (char*)pvParameters;

    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA
      tgSend(MK_SERVICE, CONFIG_NOTIFY_TELEGRAM_OTA_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, 
        CONFIG_MESSAGE_TG_OTA, otaSource);
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA

    // Notify other tasks to suspend activities
    eventLoopPostSystem(RE_SYS_OTA, RE_SYS_SET);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_DELAY));

    // Start watchdog timer
    esp_timer_create_args_t cfgWatchdog;
    esp_timer_handle_t hWatchdog;
    memset(&cfgWatchdog, 0, sizeof(cfgWatchdog));
    cfgWatchdog.callback = otaTaskWatchdog;
    cfgWatchdog.name = "ota_watchdog";
    if (esp_timer_create(&cfgWatchdog, &hWatchdog) == ESP_OK) {
      esp_timer_start_once(hWatchdog, CONFIG_OTA_WATCHDOG * 1000000);
    };

    esp_http_client_config_t cfgOTA;

    uint8_t tryUpdate = 0;
    esp_err_t err = ESP_OK;
    do {
      tryUpdate++;
      rlog_i(logTAG, "Start of firmware upgrade from \"%s\", attempt %d", otaSource, tryUpdate);
      
      memset(&cfgOTA, 0, sizeof(cfgOTA));
      cfgOTA.skip_cert_common_name_check = false;
      cfgOTA.use_global_ca_store = true;
      cfgOTA.url = otaSource;
      cfgOTA.is_async = false;

      err = esp_https_ota(&cfgOTA);
      if (err == ESP_OK) {
        rlog_i(logTAG, "Firmware upgrade completed!");
      } else {
        rlog_e(logTAG, "Firmware upgrade failed: %d!", err);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_DELAY));
      };
    } while ((err != ESP_OK) && (tryUpdate < CONFIG_OTA_ATTEMPTS));

    // Notify other tasks to restore activities
    eventLoopPostSystem(RE_SYS_OTA, RE_SYS_CLEAR);

    #if CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA
    if (err == ESP_OK) {
      tgSend(MK_SERVICE, CONFIG_NOTIFY_TELEGRAM_OTA_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, 
        CONFIG_MESSAGE_TG_OTA_OK, err);
    } else {
      tgSend(MK_SERVICE, CONFIG_NOTIFY_TELEGRAM_OTA_PRIORITY, CONFIG_NOTIFY_TELEGRAM_ALERT_OTA, CONFIG_TELEGRAM_DEVICE, 
        CONFIG_MESSAGE_TG_OTA_FAILED, err);
    };
    #endif // CONFIG_TELEGRAM_ENABLE && CONFIG_NOTIFY_TELEGRAM_OTA

    // Free resources
    if (otaSource) free(otaSource);
    _otaTask = nullptr;
    
    // Stop timer
    if (hWatchdog) {
      if (esp_timer_is_active(hWatchdog)) {
        esp_timer_stop(hWatchdog);
      }
      esp_timer_delete(hWatchdog);
    };

    if (err == ESP_OK) {
      espRestart(RR_OTA, CONFIG_OTA_DELAY);
    };
  };

  eventLoopPostSystem(RE_SYS_OTA, RE_SYS_CLEAR);

  vTaskDelete(nullptr);
  rlog_i(logTAG, "Task [ %s ] has been deleted", otaTaskName);
}

void otaStart(char *otaSource)
{
  if (otaSource) {
    if (_otaTask == nullptr) {
      xTaskCreatePinnedToCore(otaTaskExec, otaTaskName, CONFIG_OTA_TASK_STACK_SIZE, (void*)otaSource, CONFIG_OTA_TASK_PRIORITY, &_otaTask, CONFIG_OTA_TASK_CORE);
      if (_otaTask) {
        rloga_i("Task [ %s ] has been successfully created and started", otaTaskName);
      }
      else {
        rloga_e("Failed to create a task for OTA update!");
        free(otaSource);
      };
    } else {
      rloga_e("OTA update has already started!");
      free(otaSource);
    };
  } else {
    rlog_e(logTAG, "Update source not specified");
  };
}