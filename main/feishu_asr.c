#include "feishu_asr.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "feishu_asr";

#define ASR_SAMPLE_RATE 16000
#define ASR_CHUNK_MS 100
#define ASR_CHUNK_SAMPLES (ASR_SAMPLE_RATE * ASR_CHUNK_MS / 1000)
#define ASR_BUFFER_COUNT 4
#define ASR_MIN_MS 400
#define ASR_MAX_MS 30000
#define ASR_CAPTURE_STACK 4096

#define ASR_SENTINEL_DONE (-1)
#define ASR_SENTINEL_ERROR (-2)

typedef struct {
    int16_t pcm[ASR_BUFFER_COUNT][ASR_CHUNK_SAMPLES];
    QueueHandle_t free_queue;
    QueueHandle_t ready_queue;
    SemaphoreHandle_t capture_done;
    TaskHandle_t capture_task;
    volatile bool *stop_requested;
    volatile unsigned *elapsed_ms;
    volatile esp_err_t capture_error;
} asr_context_t;

static void capture_task(void *argument)
{
    asr_context_t *context = argument;
    int index;

    context->capture_error = ESP_OK;
    for (;;) {
        bool minimum_met = *context->elapsed_ms >= ASR_MIN_MS;
        if ((*context->stop_requested && minimum_met) ||
            *context->elapsed_ms >= ASR_MAX_MS) {
            break;
        }
        if (xQueueReceive(context->free_queue, &index, portMAX_DELAY) != pdTRUE) {
            context->capture_error = ESP_FAIL;
            break;
        }
        esp_err_t err = bsp_audio_read(context->pcm[index],
                                       sizeof(context->pcm[index]));
        if (err != ESP_OK) {
            xQueueSend(context->free_queue, &index, portMAX_DELAY);
            context->capture_error = err;
            break;
        }
        *context->elapsed_ms += ASR_CHUNK_MS;
        if (xQueueSend(context->ready_queue, &index, portMAX_DELAY) != pdTRUE) {
            context->capture_error = ESP_FAIL;
            break;
        }
    }
    index = context->capture_error == ESP_OK ? ASR_SENTINEL_DONE :
                                               ASR_SENTINEL_ERROR;
    xQueueSend(context->ready_queue, &index, portMAX_DELAY);
    context->capture_task = NULL;
    xSemaphoreGive(context->capture_done);
    vTaskDelete(NULL);
}

static void make_stream_id(char output[17])
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < 16; ++i) {
        output[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    output[16] = '\0';
}

static void context_destroy(asr_context_t *context)
{
    if (context == NULL) return;
    if (context->capture_done != NULL) vSemaphoreDelete(context->capture_done);
    if (context->free_queue != NULL) vQueueDelete(context->free_queue);
    if (context->ready_queue != NULL) vQueueDelete(context->ready_queue);
    memset(context, 0, sizeof(*context));
    free(context);
}

esp_err_t feishu_asr_record(feishu_api_session_t *session,
                            volatile bool *stop_requested,
                            volatile unsigned *elapsed_ms,
                            char *recognition, size_t recognition_size)
{
    asr_context_t *context;
    char stream_id[17];
    uint32_t sequence = 0;
    int pending = -1;
    int index;
    esp_err_t err = ESP_OK;

    if (session == NULL || stop_requested == NULL || elapsed_ms == NULL ||
        recognition == NULL || recognition_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    recognition[0] = '\0';
    *elapsed_ms = 0;
    *stop_requested = false;
    err = bsp_audio_set_format(ASR_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) return err;

    context = calloc(1, sizeof(*context));
    if (context == NULL) return ESP_ERR_NO_MEM;
    context->stop_requested = stop_requested;
    context->elapsed_ms = elapsed_ms;
    context->free_queue = xQueueCreate(ASR_BUFFER_COUNT, sizeof(int));
    context->ready_queue = xQueueCreate(ASR_BUFFER_COUNT + 1, sizeof(int));
    context->capture_done = xSemaphoreCreateBinary();
    if (context->free_queue == NULL || context->ready_queue == NULL ||
        context->capture_done == NULL) {
        context_destroy(context);
        return ESP_ERR_NO_MEM;
    }
    for (index = 0; index < ASR_BUFFER_COUNT; ++index) {
        xQueueSend(context->free_queue, &index, 0);
    }
    make_stream_id(stream_id);
    if (xTaskCreate(capture_task, "feishu_capture", ASR_CAPTURE_STACK,
                    context, 6, &context->capture_task) != pdPASS) {
        context_destroy(context);
        return ESP_ERR_NO_MEM;
    }

    for (;;) {
        if (xQueueReceive(context->ready_queue, &index, portMAX_DELAY) != pdTRUE) {
            err = ESP_FAIL;
            break;
        }
        if (index == ASR_SENTINEL_ERROR) {
            err = context->capture_error;
            break;
        }
        if (index == ASR_SENTINEL_DONE) {
            if (pending >= 0) {
                err = feishu_api_asr_stream_packet(
                    session, stream_id, sequence++, 2, context->pcm[pending],
                    ASR_CHUNK_SAMPLES, recognition, recognition_size);
                xQueueSend(context->free_queue, &pending, portMAX_DELAY);
                pending = -1;
            }
            break;
        }
        if (pending >= 0) {
            int action = sequence == 0 ? 1 : 0;
            err = feishu_api_asr_stream_packet(
                session, stream_id, sequence++, action, context->pcm[pending],
                ASR_CHUNK_SAMPLES, recognition, recognition_size);
            xQueueSend(context->free_queue, &pending, portMAX_DELAY);
            pending = -1;
            if (err != ESP_OK) break;
        }
        pending = index;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream failed at sequence %lu: %s",
                 (unsigned long)sequence, esp_err_to_name(err));
        *stop_requested = true;
    }
    if (pending >= 0) xQueueSend(context->free_queue, &pending, 0);
    while (context->capture_task != NULL) {
        if (xQueueReceive(context->ready_queue, &index,
                          pdMS_TO_TICKS(250)) == pdTRUE && index >= 0) {
            xQueueSend(context->free_queue, &index, portMAX_DELAY);
        }
    }
    xSemaphoreTake(context->capture_done, pdMS_TO_TICKS(1000));
    context_destroy(context);
    feishu_api_asr_stream_close();
    memset(stream_id, 0, sizeof(stream_id));
    return err;
}
