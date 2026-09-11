#include "motion_detector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MOTION_DETECTOR";

#define MOTION_MAX_SAMPLE_RATE_HZ          100U
#define MOTION_WARMUP_SECONDS              5U
#define MOTION_BASELINE_SECONDS            15U
#define MOTION_THRESHOLD_SECONDS           15U
#define MOTION_WINDOW_MS                   1500U
#define MOTION_SIGNAL_EMA_ALPHA            0.25f
#define MOTION_SCORE_EMA_ALPHA             0.30f
#define MOTION_FAST_WEIGHT                 0.35f
#define MOTION_WINDOW_WEIGHT               0.65f
#define MOTION_RESIDUAL_LIMIT              10.0f
#define MOTION_NOISE_FLOOR_MIN             0.001f
#define MOTION_ROBUST_SIGMA_FLOOR          0.05f
#define MOTION_LOW_SIGMA_MULTIPLIER        3.0f
#define MOTION_HIGH_SIGMA_MULTIPLIER       5.0f
#define MOTION_THRESHOLD_MIN_GAP           0.10f
#define MOTION_ENTER_TIME_MS               200U
#define MOTION_EXIT_TIME_MS                3000U
#define MOTION_BASELINE_GUARD_MS           5000U
#define MOTION_BASELINE_EMA_ALPHA          0.001f

#define MOTION_MAX_WINDOW_SAMPLES \
    ((MOTION_MAX_SAMPLE_RATE_HZ * MOTION_WINDOW_MS + 999U) / 1000U)

#define MOTION_MAX_THRESHOLD_SAMPLES \
    (MOTION_MAX_SAMPLE_RATE_HZ * MOTION_THRESHOLD_SECONDS)

static float s_filtered[CSI_VALID_SUBCARRIER_COUNT] = {0};
static float s_baseline[CSI_VALID_SUBCARRIER_COUNT] = {0};
static float s_baseline_m2[CSI_VALID_SUBCARRIER_COUNT] = {0};
static float s_noise_scale[CSI_VALID_SUBCARRIER_COUNT] = {0};
static float s_previous_residual[CSI_VALID_SUBCARRIER_COUNT] = {0};

static float s_window[MOTION_MAX_WINDOW_SAMPLES]
                     [CSI_VALID_SUBCARRIER_COUNT] = {{0}};
static float s_window_sum[CSI_VALID_SUBCARRIER_COUNT] = {0};
static float s_window_sum_sq[CSI_VALID_SUBCARRIER_COUNT] = {0};

static float s_threshold_samples[MOTION_MAX_THRESHOLD_SAMPLES] = {0};

static uint16_t s_sample_rate_hz = 0U;
static uint16_t s_window_target = 0U;
static uint16_t s_window_head = 0U;
static uint16_t s_window_count = 0U;

static uint32_t s_warmup_target = 0U;
static uint32_t s_baseline_target = 0U;
static uint32_t s_threshold_target = 0U;
static uint32_t s_calibration_target = 0U;
static uint32_t s_phase_samples = 0U;
static uint32_t s_baseline_count = 0U;
static uint32_t s_threshold_count = 0U;
static uint32_t s_above_count = 0U;
static uint32_t s_below_count = 0U;
static uint32_t s_static_stable_count = 0U;

static float s_fast_change = 0.0f;
static float s_window_activity = 0.0f;
static float s_baseline_distance = 0.0f;
static float s_raw_score = 0.0f;
static float s_score_filtered = 0.0f;
static float s_threshold_low = 0.0f;
static float s_threshold_high = 0.0f;

static bool s_signal_initialized = false;
static bool s_baseline_ready = false;
static bool s_previous_residual_ready = false;
static bool s_score_initialized = false;
static volatile bool s_recalibration_requested = false;
static csi_motion_state_t s_state = CSI_MOTION_STATE_WARMUP;

static portMUX_TYPE s_request_lock = portMUX_INITIALIZER_UNLOCKED;

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum)
    {
        return minimum;
    }

    if (value > maximum)
    {
        return maximum;
    }

    return value;
}

static int compare_float(const void *left, const void *right)
{
    const float a = *(const float *)left;
    const float b = *(const float *)right;

    if (a < b)
    {
        return -1;
    }

    if (a > b)
    {
        return 1;
    }

    return 0;
}

static float median_in_place(float *values, size_t count)
{
    if ((values == NULL) || (count == 0U))
    {
        return 0.0f;
    }

    qsort(values, count, sizeof(values[0]), compare_float);

    if ((count & 1U) != 0U)
    {
        return values[count / 2U];
    }

    return 0.5f *
           (values[(count / 2U) - 1U] + values[count / 2U]);
}

static void reset_feature_history(void)
{
    memset(s_previous_residual, 0, sizeof(s_previous_residual));
    memset(s_window, 0, sizeof(s_window));
    memset(s_window_sum, 0, sizeof(s_window_sum));
    memset(s_window_sum_sq, 0, sizeof(s_window_sum_sq));

    s_window_head = 0U;
    s_window_count = 0U;
    s_previous_residual_ready = false;
    s_score_initialized = false;

    s_fast_change = 0.0f;
    s_window_activity = 0.0f;
    s_baseline_distance = 0.0f;
    s_raw_score = 0.0f;
    s_score_filtered = 0.0f;
}

static void reset_detector(void)
{
    memset(s_filtered, 0, sizeof(s_filtered));
    memset(s_baseline, 0, sizeof(s_baseline));
    memset(s_baseline_m2, 0, sizeof(s_baseline_m2));
    memset(s_noise_scale, 0, sizeof(s_noise_scale));
    memset(s_threshold_samples, 0, sizeof(s_threshold_samples));

    reset_feature_history();

    s_phase_samples = 0U;
    s_baseline_count = 0U;
    s_threshold_count = 0U;
    s_above_count = 0U;
    s_below_count = 0U;
    s_static_stable_count = 0U;

    s_threshold_low = 0.0f;
    s_threshold_high = 0.0f;

    s_signal_initialized = false;
    s_baseline_ready = false;
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

static void update_filtered_signal(const float *normalized_amplitudes)
{
    if (!s_signal_initialized)
    {
        memcpy(s_filtered, normalized_amplitudes, sizeof(s_filtered));
        s_signal_initialized = true;
        return;
    }

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        s_filtered[index] =
            (MOTION_SIGNAL_EMA_ALPHA * normalized_amplitudes[index]) +
            ((1.0f - MOTION_SIGNAL_EMA_ALPHA) * s_filtered[index]);
    }
}

static void update_baseline_statistics(void)
{
    s_baseline_count++;

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        const float delta = s_filtered[index] - s_baseline[index];

        s_baseline[index] += delta / (float)s_baseline_count;
        s_baseline_m2[index] +=
            delta * (s_filtered[index] - s_baseline[index]);
    }
}

static void finish_baseline_calibration(void)
{
    float standard_deviations[CSI_VALID_SUBCARRIER_COUNT] = {0};
    float noise_floor;

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        float variance = 0.0f;

        if (s_baseline_count > 1U)
        {
            variance = s_baseline_m2[index] /
                       (float)(s_baseline_count - 1U);
        }

        if (variance < 0.0f)
        {
            variance = 0.0f;
        }

        standard_deviations[index] = sqrtf(variance);
    }

    noise_floor = 0.25f * median_in_place(
        standard_deviations,
        CSI_VALID_SUBCARRIER_COUNT
    );

    if (noise_floor < MOTION_NOISE_FLOOR_MIN)
    {
        noise_floor = MOTION_NOISE_FLOOR_MIN;
    }

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        float variance = 0.0f;
        float standard_deviation;

        if (s_baseline_count > 1U)
        {
            variance = s_baseline_m2[index] /
                       (float)(s_baseline_count - 1U);
        }

        if (variance < 0.0f)
        {
            variance = 0.0f;
        }

        standard_deviation = sqrtf(variance);
        s_noise_scale[index] =
            (standard_deviation > noise_floor) ?
            standard_deviation : noise_floor;
    }

    s_baseline_ready = true;
    reset_feature_history();

    ESP_LOGI(
        TAG,
        "Static baseline ready: samples=%lu, noise_floor=%.6f",
        (unsigned long)s_baseline_count,
        (double)noise_floor
    );
}

static void push_residual_window(const float *residual)
{
    const bool window_full = (s_window_count == s_window_target);

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        if (window_full)
        {
            const float old_value = s_window[s_window_head][index];

            s_window_sum[index] -= old_value;
            s_window_sum_sq[index] -= old_value * old_value;
        }

        s_window[s_window_head][index] = residual[index];
        s_window_sum[index] += residual[index];
        s_window_sum_sq[index] += residual[index] * residual[index];
    }

    s_window_head++;
    if (s_window_head >= s_window_target)
    {
        s_window_head = 0U;
    }

    if (!window_full)
    {
        s_window_count++;
    }
}

static void calculate_features(void)
{
    float residual[CSI_VALID_SUBCARRIER_COUNT] = {0};
    float sum_fast_squared = 0.0f;
    float sum_baseline_squared = 0.0f;
    float sum_window_variance = 0.0f;

    if (!s_baseline_ready)
    {
        return;
    }

    for (size_t index = 0U;
         index < CSI_VALID_SUBCARRIER_COUNT;
         index++)
    {
        residual[index] = clamp_float(
            (s_filtered[index] - s_baseline[index]) /
            s_noise_scale[index],
            -MOTION_RESIDUAL_LIMIT,
            MOTION_RESIDUAL_LIMIT
        );

        sum_baseline_squared += residual[index] * residual[index];

        if (s_previous_residual_ready)
        {
            const float difference =
                residual[index] - s_previous_residual[index];

            sum_fast_squared += difference * difference;
        }
    }

    s_baseline_distance = sqrtf(
        sum_baseline_squared / (float)CSI_VALID_SUBCARRIER_COUNT
    );

    if (s_previous_residual_ready)
    {
        s_fast_change = sqrtf(
            sum_fast_squared / (float)CSI_VALID_SUBCARRIER_COUNT
        );
    }
    else
    {
        s_fast_change = 0.0f;
    }

    memcpy(s_previous_residual, residual, sizeof(s_previous_residual));
    s_previous_residual_ready = true;

    push_residual_window(residual);

    if (s_window_count >= 2U)
    {
        const float count = (float)s_window_count;

        for (size_t index = 0U;
             index < CSI_VALID_SUBCARRIER_COUNT;
             index++)
        {
            const float mean = s_window_sum[index] / count;
            float variance =
                (s_window_sum_sq[index] / count) - (mean * mean);

            if (variance < 0.0f)
            {
                variance = 0.0f;
            }

            sum_window_variance += variance;
        }

        s_window_activity = sqrtf(
            sum_window_variance /
            (float)CSI_VALID_SUBCARRIER_COUNT
        );
    }
    else
    {
        s_window_activity = 0.0f;
    }

    s_raw_score =
        (MOTION_FAST_WEIGHT * s_fast_change) +
        (MOTION_WINDOW_WEIGHT * s_window_activity);

    if (!s_score_initialized)
    {
        s_score_filtered = s_raw_score;
        s_score_initialized = true;
    }
    else
    {
        s_score_filtered =
            (MOTION_SCORE_EMA_ALPHA * s_raw_score) +
            ((1.0f - MOTION_SCORE_EMA_ALPHA) * s_score_filtered);
    }
}

static void collect_threshold_sample(void)
{
    if ((s_window_count < s_window_target) ||
        (s_threshold_count >= MOTION_MAX_THRESHOLD_SAMPLES))
    {
        return;
    }

    s_threshold_samples[s_threshold_count] = s_score_filtered;
    s_threshold_count++;
}

static void finish_threshold_calibration(void)
{
    float median;
    float mad;
    float robust_sigma;

    if (s_threshold_count == 0U)
    {
        median = 0.0f;
        mad = MOTION_ROBUST_SIGMA_FLOOR / 1.4826f;
    }
    else
    {
        median = median_in_place(
            s_threshold_samples,
            s_threshold_count
        );

        for (size_t index = 0U;
             index < s_threshold_count;
             index++)
        {
            s_threshold_samples[index] =
                fabsf(s_threshold_samples[index] - median);
        }

        mad = median_in_place(
            s_threshold_samples,
            s_threshold_count
        );
    }

    robust_sigma = 1.4826f * mad;
    if (robust_sigma < MOTION_ROBUST_SIGMA_FLOOR)
    {
        robust_sigma = MOTION_ROBUST_SIGMA_FLOOR;
    }

    s_threshold_low = median +
                      (MOTION_LOW_SIGMA_MULTIPLIER * robust_sigma);
    s_threshold_high = median +
                       (MOTION_HIGH_SIGMA_MULTIPLIER * robust_sigma);

    if (s_threshold_high < (s_threshold_low + MOTION_THRESHOLD_MIN_GAP))
    {
        s_threshold_high = s_threshold_low + MOTION_THRESHOLD_MIN_GAP;
    }

    s_above_count = 0U;
    s_below_count = 0U;
    s_static_stable_count = 0U;
    s_state = CSI_MOTION_STATE_STATIC;

    ESP_LOGI(
        TAG,
        "Calibration complete: samples=%lu, median=%.4f, MAD=%.4f, low=%.4f, high=%.4f",
        (unsigned long)s_threshold_count,
        (double)median,
        (double)mad,
        (double)s_threshold_low,
        (double)s_threshold_high
    );
}

static void update_motion_state(void)
{
    uint32_t enter_samples =
        ((uint32_t)s_sample_rate_hz * MOTION_ENTER_TIME_MS + 999U) /
        1000U;
    uint32_t exit_samples =
        ((uint32_t)s_sample_rate_hz * MOTION_EXIT_TIME_MS + 999U) /
        1000U;

    if (enter_samples == 0U)
    {
        enter_samples = 1U;
    }

    if (exit_samples == 0U)
    {
        exit_samples = 1U;
    }

    if (s_state == CSI_MOTION_STATE_STATIC)
    {
        s_below_count = 0U;

        if (s_score_filtered > s_threshold_high)
        {
            s_above_count++;
            if (s_above_count >= enter_samples)
            {
                s_state = CSI_MOTION_STATE_MOVING;
                s_above_count = 0U;
                s_static_stable_count = 0U;
                ESP_LOGI(
                    TAG,
                    "Motion detected: score=%.4f",
                    (double)s_score_filtered
                );
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

        if (s_score_filtered < s_threshold_low)
        {
            s_below_count++;
            if (s_below_count >= exit_samples)
            {
                s_state = CSI_MOTION_STATE_STATIC;
                s_below_count = 0U;
                s_static_stable_count = 0U;
                ESP_LOGI(
                    TAG,
                    "Motion ended: score=%.4f",
                    (double)s_score_filtered
                );
            }
        }
        else
        {
            s_below_count = 0U;
        }
    }
}

static void update_static_baseline(void)
{
    uint32_t guard_samples =
        ((uint32_t)s_sample_rate_hz * MOTION_BASELINE_GUARD_MS + 999U) /
        1000U;

    if (guard_samples == 0U)
    {
        guard_samples = 1U;
    }

    if ((s_state == CSI_MOTION_STATE_STATIC) &&
        (s_score_filtered < s_threshold_low))
    {
        if (s_static_stable_count < guard_samples)
        {
            s_static_stable_count++;
        }

        if (s_static_stable_count >= guard_samples)
        {
            for (size_t index = 0U;
                 index < CSI_VALID_SUBCARRIER_COUNT;
                 index++)
            {
                s_baseline[index] =
                    ((1.0f - MOTION_BASELINE_EMA_ALPHA) *
                     s_baseline[index]) +
                    (MOTION_BASELINE_EMA_ALPHA * s_filtered[index]);
            }
        }
    }
    else
    {
        s_static_stable_count = 0U;
    }
}

esp_err_t motion_detector_configure(uint16_t sample_rate_hz)
{
    uint32_t window_target;

    if ((sample_rate_hz == 0U) ||
        (sample_rate_hz > MOTION_MAX_SAMPLE_RATE_HZ))
    {
        return ESP_ERR_INVALID_ARG;
    }

    window_target =
        ((uint32_t)sample_rate_hz * MOTION_WINDOW_MS + 999U) /
        1000U;

    if (window_target < 2U)
    {
        window_target = 2U;
    }

    if (window_target > MOTION_MAX_WINDOW_SAMPLES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    s_sample_rate_hz = sample_rate_hz;
    s_window_target = (uint16_t)window_target;
    s_warmup_target =
        (uint32_t)sample_rate_hz * MOTION_WARMUP_SECONDS;
    s_baseline_target =
        (uint32_t)sample_rate_hz * MOTION_BASELINE_SECONDS;
    s_threshold_target =
        (uint32_t)sample_rate_hz * MOTION_THRESHOLD_SECONDS;
    s_calibration_target = s_baseline_target + s_threshold_target;

    reset_detector();

    ESP_LOGI(
        TAG,
        "Configured: rate=%u Hz, window=%u samples, warmup=%us, calibration=%us",
        (unsigned int)s_sample_rate_hz,
        (unsigned int)s_window_target,
        (unsigned int)MOTION_WARMUP_SECONDS,
        (unsigned int)(MOTION_BASELINE_SECONDS +
                       MOTION_THRESHOLD_SECONDS)
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

    update_filtered_signal(normalized_amplitudes);

    if (s_state == CSI_MOTION_STATE_WARMUP)
    {
        s_phase_samples++;

        if (s_phase_samples >= s_warmup_target)
        {
            s_phase_samples = 0U;
            s_baseline_count = 0U;
            memset(s_baseline, 0, sizeof(s_baseline));
            memset(s_baseline_m2, 0, sizeof(s_baseline_m2));
            s_state = CSI_MOTION_STATE_CALIBRATING;
            ESP_LOGI(TAG, "Warm-up complete; keep the room still");
        }
    }
    else if (s_state == CSI_MOTION_STATE_CALIBRATING)
    {
        if (s_phase_samples < s_baseline_target)
        {
            update_baseline_statistics();
            s_phase_samples++;

            if (s_phase_samples == s_baseline_target)
            {
                finish_baseline_calibration();
            }
        }
        else
        {
            calculate_features();
            collect_threshold_sample();
            s_phase_samples++;

            if (s_phase_samples >= s_calibration_target)
            {
                finish_threshold_calibration();
            }
        }
    }
    else
    {
        calculate_features();
        update_motion_state();
        update_static_baseline();
    }

    result->fast_change = s_fast_change;
    result->window_activity = s_window_activity;
    result->baseline_distance = s_baseline_distance;
    result->raw_score = s_raw_score;
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