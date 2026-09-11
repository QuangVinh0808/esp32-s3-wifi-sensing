#include "motion_detector.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MOTION_DETECTOR";

#define MOTION_WARMUP_SECONDS              5U
#define MOTION_CALIBRATION_SECONDS         15U
#define MOTION_FAST_EMA_ALPHA              0.25f
#define MOTION_SCORE_EMA_ALPHA             0.20f
#define MOTION_HIGH_SIGMA_MULTIPLIER       4.0f
#define MOTION_LOW_SIGMA_MULTIPLIER        2.0f
#define MOTION_HIGH_CONFIRM_SAMPLES        3U
#define MOTION_THRESHOLD_MIN_GAP           0.001f

static float s_filtered[CSI_VALID_SUBCARRIER_COUNT] = {0};

static uint16_t s_sample_rate_hz = 0U;
static uint32_t s_warmup_target = 0U;
static uint32_t s_calibration_target = 0U;
static uint32_t s_phase_samples = 0U;
static uint32_t s_calibration_count = 0U;
static uint32_t s_above_count = 0U;
static uint32_t s_below_count = 0U;

static float s_score_filtered = 0.0f;
static float s_calibration_mean = 0.0f;
static float s_calibration_m2 = 0.0f;
static float s_threshold_low = 0.0f;
static float s_threshold_high = 0.0f;

static bool s_signal_initialized = false;
static bool s_score_initialized = false;
static volatile bool s_recalibration_requested = false;
static csi_motion_state_t s_state = CSI_MOTION_STATE_WARMUP;

static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;

static void reset_detector(void)
{
    memset(s_filtered, 0, sizeof(s_filtered));

    s_phase_samples = 0U;
    s_calibration_count = 0U;
    s_above_count = 0U;
    s_below_count = 0U;

    s_score_filtered = 0.0f;
    s_calibration_mean = 0.0f;
    s_calibration_m2 = 0.0f;
    s_threshold_low = 0.0f;
    s_threshold_high = 0.0f;

    s_signal_initialized = false;
    s_score_initialized = false;
    s_state = CSI_MOTION_STATE_WARMUP;
}

static bool consume_recalibration_request(void)
{
    bool requested;

    portENTER_CRITICAL(&s_request_lock);
    requested = s_recalibration_requested;
    s_recalibration_requested = false;
    portEXIT_CRITICAL(&s_request_lock);

    return requested;
}

static uint8_t calibration_progress(void)
{
    const uint32_t total = s_warmup_target + s_calibration_target;
    uint32_t completed;

    if ((s_state == CSI_MOTION_STATE_STATIC) ||
        (s_state == CSI_MOTION_STATE_MOVING))
    {
        return 100U;
    }

    if (total == 0U)
    {
        return 0U;
    }

    completed = s_phase_samples;
    if (s_state == CSI_MOTION_STATE_CALIBRATING)
    {
        completed += s_warmup_target;
    }

    if (completed >= total)
    {
        return 100U;
    }

    return (uint8_t)((completed * 100U) / total);
}

static void update_calibration_statistics(float score)
{
    float delta;
    float delta_after_update;

    s_calibration_count++;
    delta = score - s_calibration_mean;
    s_calibration_mean += delta / (float)s_calibration_count;
    delta_after_update = score - s_calibration_mean;
    s_calibration_m2 += delta * delta_after_update;
}

static void finish_calibration(void)
{
    float variance = 0.0f;
    float standard_deviation;

    if (s_calibration_count > 1U)
    {
        variance = s_calibration_m2 /
                   (float)(s_calibration_count - 1U);
    }

    if (variance < 0.0f)
    {
        variance = 0.0f;
    }

    standard_deviation = sqrtf(variance);
    s_threshold_low = s_calibration_mean +
                      (MOTION_LOW_SIGMA_MULTIPLIER * standard_deviation);
    s_threshold_high = s_calibration_mean +
                       (MOTION_HIGH_SIGMA_MULTIPLIER * standard_deviation);

    if (s_threshold_high < (s_threshold_low + MOTION_THRESHOLD_MIN_GAP))
    {
        s_threshold_high = s_threshold_low + MOTION_THRESHOLD_MIN_GAP;
    }

    s_above_count = 0U;
    s_below_count = 0U;
    s_state = CSI_MOTION_STATE_STATIC;

    ESP_LOGI(
        TAG,
        "Calibration complete: mean=%.6f, std=%.6f, low=%.6f, high=%.6f",
        (double)s_calibration_mean,
        (double)standard_deviation,
        (double)s_threshold_low,
        (double)s_threshold_high
    );
}

static void update_motion_state(float score)
{
    uint32_t low_confirm_samples = (uint32_t)s_sample_rate_hz / 2U;

    if (low_confirm_samples < MOTION_HIGH_CONFIRM_SAMPLES)
    {
        low_confirm_samples = MOTION_HIGH_CONFIRM_SAMPLES;
    }

    if (s_state == CSI_MOTION_STATE_STATIC)
    {
        s_below_count = 0U;

        if (score > s_threshold_high)
        {
            s_above_count++;
            if (s_above_count >= MOTION_HIGH_CONFIRM_SAMPLES)
            {
                s_state = CSI_MOTION_STATE_MOVING;
                s_above_count = 0U;
                ESP_LOGI(TAG, "Motion detected, score=%.6f", (double)score);
            }
        }
        else
        {
            s_above_count = 0U;
        }
    }
    else if (s_state == CSI_MOTION_STATE_MOVING)
    {
        s_above_count = 0U;

        if (score < s_threshold_low)
        {
            s_below_count++;
            if (s_below_count >= low_confirm_samples)
            {
                s_state = CSI_MOTION_STATE_STATIC;
                s_below_count = 0U;
                ESP_LOGI(TAG, "Motion ended, score=%.6f", (double)score);
            }
        }
        else
        {
            s_below_count = 0U;
        }
    }
}

esp_err_t motion_detector_configure(uint16_t sample_rate_hz)
{
    if (sample_rate_hz == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_sample_rate_hz = sample_rate_hz;
    s_warmup_target =
        (uint32_t)sample_rate_hz * MOTION_WARMUP_SECONDS;
    s_calibration_target =
        (uint32_t)sample_rate_hz * MOTION_CALIBRATION_SECONDS;

    reset_detector();

    ESP_LOGI(
        TAG,
        "Configured at %u Hz: warm-up=%us, calibration=%us",
        (unsigned int)sample_rate_hz,
        (unsigned int)MOTION_WARMUP_SECONDS,
        (unsigned int)MOTION_CALIBRATION_SECONDS
    );

    return ESP_OK;
}

void motion_detector_request_calibration(void)
{
    portENTER_CRITICAL(&s_request_lock);
    s_recalibration_requested = true;
    portEXIT_CRITICAL(&s_request_lock);
}

void motion_detector_process(
    const float *normalized_amplitudes,
    size_t subcarrier_count,
    motion_result_t *result
)
{
    float sum_squared_difference = 0.0f;
    float instant_score;

    if (result == NULL)
    {
        return;
    }

    memset(result, 0, sizeof(*result));

    if ((normalized_amplitudes == NULL) ||
        (subcarrier_count != CSI_VALID_SUBCARRIER_COUNT) ||
        (s_sample_rate_hz == 0U))
    {
        result->state = CSI_MOTION_STATE_WARMUP;
        return;
    }

    if (consume_recalibration_request())
    {
        reset_detector();
        ESP_LOGI(TAG, "Recalibration requested");
    }

    if (!s_signal_initialized)
    {
        memcpy(
            s_filtered,
            normalized_amplitudes,
            sizeof(s_filtered)
        );
        s_signal_initialized = true;
    }

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        const float previous = s_filtered[index];
        const float filtered =
            (MOTION_FAST_EMA_ALPHA * normalized_amplitudes[index]) +
            ((1.0f - MOTION_FAST_EMA_ALPHA) * previous);
        const float difference = filtered - previous;

        s_filtered[index] = filtered;
        sum_squared_difference += difference * difference;
    }

    instant_score = sqrtf(
        sum_squared_difference / (float)CSI_VALID_SUBCARRIER_COUNT
    );

    if (!s_score_initialized)
    {
        s_score_filtered = instant_score;
        s_score_initialized = true;
    }
    else
    {
        s_score_filtered =
            (MOTION_SCORE_EMA_ALPHA * instant_score) +
            ((1.0f - MOTION_SCORE_EMA_ALPHA) * s_score_filtered);
    }

    if (s_state == CSI_MOTION_STATE_WARMUP)
    {
        s_phase_samples++;
        if (s_phase_samples >= s_warmup_target)
        {
            s_phase_samples = 0U;
            s_calibration_count = 0U;
            s_calibration_mean = 0.0f;
            s_calibration_m2 = 0.0f;
            s_state = CSI_MOTION_STATE_CALIBRATING;
            ESP_LOGI(TAG, "Warm-up complete; keep the room still");
        }
    }
    else if (s_state == CSI_MOTION_STATE_CALIBRATING)
    {
        update_calibration_statistics(s_score_filtered);
        s_phase_samples++;

        if (s_phase_samples >= s_calibration_target)
        {
            finish_calibration();
        }
    }
    else
    {
        update_motion_state(s_score_filtered);
    }

    result->score = s_score_filtered;
    result->threshold_low = s_threshold_low;
    result->threshold_high = s_threshold_high;
    result->calibration_progress = calibration_progress();
    result->state = s_state;
    result->calibrated =
        (s_state == CSI_MOTION_STATE_STATIC) ||
        (s_state == CSI_MOTION_STATE_MOVING);
}

const char *motion_detector_state_name(csi_motion_state_t state)
{
    switch (state)
    {
        case CSI_MOTION_STATE_WARMUP:
            return "WARMUP";

        case CSI_MOTION_STATE_CALIBRATING:
            return "CALIBRATING";

        case CSI_MOTION_STATE_STATIC:
            return "STATIC";

        case CSI_MOTION_STATE_MOVING:
            return "MOVING";

        default:
            return "UNKNOWN";
    }
}