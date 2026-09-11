#include "csi_processor.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "csi_capture.h"
#include "csi_types.h"
#include "motion_detector.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "CSI_PROCESSOR";

#define CSI_PROCESSED_QUEUE_LENGTH       1U
#define CSI_PROCESSOR_TASK_STACK_SIZE    4096U
#define CSI_PROCESSOR_TASK_PRIORITY      5U
#define CSI_PROCESSOR_STOP_WAIT_MS       1000U
#define CSI_PROCESSOR_LOG_PERIOD         100U

static QueueHandle_t s_raw_queue = NULL;
static QueueHandle_t s_processed_queue = NULL;
static TaskHandle_t s_processor_task = NULL;
static volatile bool s_running = false;

static int fft_position_to_subcarrier(size_t position)
{
    return (position <= 31U) ? (int)position : (int)position - 64;
}

static bool is_valid_lltf_subcarrier(int subcarrier)
{
    return ((subcarrier >= -26) && (subcarrier <= -1)) ||
           ((subcarrier >= 1) && (subcarrier <= 26));
}

static bool process_raw_sample(
    const csi_raw_sample_t *raw,
    csi_processed_sample_t *output,
    uint32_t sequence
)
{
    size_t pair_count;
    size_t first_pair;
    float sum_power = 0.0f;
    float min_power = 0.0f;
    float max_power = 0.0f;
    float amplitudes[CSI_VALID_SUBCARRIER_COUNT] = {0};
    float normalized_amplitudes[CSI_VALID_SUBCARRIER_COUNT] = {0};
    float sum_amplitude = 0.0f;
    uint16_t valid_count = 0U;
    csi_capture_stats_t stats = {0};
    motion_result_t motion = {0};

    if ((raw == NULL) || (output == NULL) || (raw->csi_len < 2U))
    {
        return false;
    }

    pair_count = (size_t)raw->csi_len / 2U;
    if (pair_count > CSI_LLTF_COMPLEX_COUNT)
    {
        pair_count = CSI_LLTF_COMPLEX_COUNT;
    }

    first_pair = raw->first_word_invalid ? 2U : 0U;

    for (size_t pair = first_pair; pair < pair_count; pair++)
    {
        const int subcarrier = fft_position_to_subcarrier(pair);
        const size_t byte_index = pair * 2U;
        float power;
        int8_t imaginary;
        int8_t real;

        if (!is_valid_lltf_subcarrier(subcarrier))
        {
            continue;
        }

        imaginary = raw->data[byte_index];
        real = raw->data[byte_index + 1U];

        power = ((float)real * (float)real) +
                ((float)imaginary * (float)imaginary);

        if ((valid_count == 0U) || (power < min_power))
        {
            min_power = power;
        }

        if ((valid_count == 0U) || (power > max_power))
        {
            max_power = power;
        }

        sum_power += power;
        if (valid_count < CSI_VALID_SUBCARRIER_COUNT)
        {
            amplitudes[valid_count] = sqrtf(power);
            sum_amplitude += amplitudes[valid_count];
        }
        valid_count++;
    }

    if ((valid_count != CSI_VALID_SUBCARRIER_COUNT) ||
        (sum_amplitude <= 0.0f))
    {
        return false;
    }

    {
        const float mean_amplitude =
            sum_amplitude / (float)valid_count;

        for (size_t index = 0U; index < valid_count; index++)
        {
            normalized_amplitudes[index] =
                amplitudes[index] / mean_amplitude;
        }
    }

    motion_detector_process(
        normalized_amplitudes,
        valid_count,
        &motion
    );

    csi_capture_get_stats(&stats);

    output->timestamp_ms = esp_timer_get_time() / 1000LL;
    output->sequence = sequence;
    output->rssi = raw->rssi;
    output->noise_floor = raw->noise_floor;
    output->channel = raw->channel;
    output->valid_subcarriers = valid_count;
    output->mean_power = sum_power / (float)valid_count;
    output->min_power = min_power;
    output->max_power = max_power;
    output->motion_score = motion.score;
    output->motion_threshold_low = motion.threshold_low;
    output->motion_threshold_high = motion.threshold_high;
    output->calibration_progress = motion.calibration_progress;
    output->motion_state = motion.state;
    output->motion_calibrated = motion.calibrated;
    output->received_packets = stats.received_packets;
    output->dropped_packets = stats.dropped_packets;

    return true;
}

static void csi_processor_task(void *argument)
{
    csi_raw_sample_t raw_sample;
    uint32_t sequence = 0U;

    (void)argument;

    while (s_running)
    {
        if (xQueueReceive(s_raw_queue, &raw_sample, pdMS_TO_TICKS(100U)) == pdTRUE)
        {
            csi_processed_sample_t output = {0};

            if (process_raw_sample(&raw_sample, &output, sequence))
            {
                sequence++;
                (void)xQueueOverwrite(s_processed_queue, &output);

                if ((sequence % CSI_PROCESSOR_LOG_PERIOD) == 0U)
                {
                    ESP_LOGI(
                        TAG,
                        "seq=%" PRIu32 ", power=%.2f, motion=%.6f, state=%s, dropped=%" PRIu32,
                        output.sequence,
                        (double)output.mean_power,
                        (double)output.motion_score,
                        motion_detector_state_name(output.motion_state),
                        output.dropped_packets
                    );
                }
            }
        }
    }

    s_processor_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t csi_processor_start(
    QueueHandle_t raw_queue,
    uint16_t sample_rate_hz
)
{
    BaseType_t task_result;

    if (s_running)
    {
        return ESP_OK;
    }

    if ((raw_queue == NULL) || (sample_rate_hz == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_processed_queue == NULL)
    {
        s_processed_queue = xQueueCreate(
            CSI_PROCESSED_QUEUE_LENGTH,
            sizeof(csi_processed_sample_t)
        );

        if (s_processed_queue == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    else
    {
        (void)xQueueReset(s_processed_queue);
    }

    s_raw_queue = raw_queue;
    {
        const esp_err_t detector_err =
            motion_detector_configure(sample_rate_hz);

        if (detector_err != ESP_OK)
        {
            s_raw_queue = NULL;
            return detector_err;
        }
    }
    s_running = true;

    task_result = xTaskCreate(
        csi_processor_task,
        "csi_processor",
        CSI_PROCESSOR_TASK_STACK_SIZE,
        NULL,
        CSI_PROCESSOR_TASK_PRIORITY,
        &s_processor_task
    );

    if (task_result != pdPASS)
    {
        s_running = false;
        s_raw_queue = NULL;
        s_processor_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CSI processor started");
    return ESP_OK;
}

esp_err_t csi_processor_stop(void)
{
    uint32_t elapsed_ms = 0U;

    if (!s_running && (s_processor_task == NULL))
    {
        return ESP_OK;
    }

    s_running = false;

    while ((s_processor_task != NULL) &&
           (elapsed_ms < CSI_PROCESSOR_STOP_WAIT_MS))
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
        elapsed_ms += 10U;
    }

    if (s_processor_task != NULL)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_raw_queue = NULL;
    ESP_LOGI(TAG, "CSI processor stopped");
    return ESP_OK;
}

bool csi_processor_is_running(void)
{
    return s_running;
}

QueueHandle_t csi_processor_get_queue(void)
{
    return s_processed_queue;
}
