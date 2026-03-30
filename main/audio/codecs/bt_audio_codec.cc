#include "bt_audio_codec.h"

#ifdef CONFIG_BT_A2DP_ENABLE

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#define TAG "BtAudioCodec"

BtAudioCodec* BtAudioCodec::instance_ = nullptr;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

BtAudioCodec::BtAudioCodec(int input_sample_rate,
                             gpio_num_t mic_bclk, gpio_num_t mic_ws,
                             gpio_num_t mic_din) {
    instance_ = this;

    // Opus decoder outputs 48 kHz; we resample to 44.1 kHz inside Write()
    output_sample_rate_ = 48000;
    input_sample_rate_ = input_sample_rate;
    input_channels_ = 1;
    output_channels_ = 1;
    duplex_ = false;
    input_reference_ = false;

    // --- Ring buffer for PCM data passed from Write() to A2dDataCallback() ---
    pcm_ringbuf_ = xRingbufferCreate(kRingBufBytes, RINGBUF_TYPE_BYTEBUF);
    assert(pcm_ringbuf_ != nullptr);

    // --- I2S receive channel for the microphone ---
    i2s_chan_config_t rx_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = false,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, nullptr, &rx_handle_));

    i2s_std_config_t rx_std = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = mic_bclk,
            .ws = mic_ws,
            .dout = I2S_GPIO_UNUSED,
            .din = mic_din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std));

    // --- Bluetooth ---
    InitBluetooth();

    ESP_LOGI(TAG, "BtAudioCodec ready  mic=%d Hz  bt_out=%d→%d Hz",
             input_sample_rate_, output_sample_rate_, kTargetSampleRate);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

BtAudioCodec::~BtAudioCodec() {
    if (bt_connected_.load()) {
        esp_a2d_source_disconnect(connected_bda_);
    }
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (rx_handle_) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
    if (pcm_ringbuf_) {
        vRingbufferDelete(pcm_ringbuf_);
        pcm_ringbuf_ = nullptr;
    }
    instance_ = nullptr;
}

// ---------------------------------------------------------------------------
// Bluetooth initialisation
// ---------------------------------------------------------------------------

void BtAudioCodec::InitBluetooth() {
    // Release BLE memory — we only need Classic BT
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(GapCallback));
    ESP_ERROR_CHECK(esp_a2d_register_callback(A2dCallback));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(A2dDataCallback));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    // Connectable but not discoverable (we initiate connections ourselves)
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_gap_set_device_name("XiaoZhi");

    ESP_LOGI(TAG, "Bluetooth initialised");
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------

void BtAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(i2s_mutex_);
    if (enable == input_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void BtAudioCodec::EnableOutput(bool enable) {
    // BT output state is managed by the A2DP connection; nothing to toggle here
    AudioCodec::EnableOutput(enable);
}

void BtAudioCodec::SetOutputVolume(int volume) {
    // Use square law for perceived loudness (matches NoAudioCodec)
    volume_scale_ = std::pow(static_cast<float>(volume) / 100.0f, 2.0f);
    AudioCodec::SetOutputVolume(volume);
}

// ---------------------------------------------------------------------------
// Bluetooth scan / connect / disconnect
// ---------------------------------------------------------------------------

void BtAudioCodec::StartScan() {
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        discovered_devices_.clear();
    }
    ESP_LOGI(TAG, "Starting BT device scan...");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 5, 0);
}

void BtAudioCodec::StopScan() {
    esp_bt_gap_cancel_discovery();
}

bool BtAudioCodec::Connect(const esp_bd_addr_t bda) {
    ESP_LOGI(TAG, "Connecting to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
    esp_err_t err = esp_a2d_source_connect(const_cast<esp_bd_addr_t>(bda));
    return err == ESP_OK;
}

void BtAudioCodec::Disconnect() {
    if (bt_connected_.load()) {
        esp_a2d_source_disconnect(connected_bda_);
    }
}

std::vector<std::string> BtAudioCodec::GetDiscoveredDevices() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return discovered_devices_;
}

// ---------------------------------------------------------------------------
// Write — called by AudioOutputTask with mono 48 kHz PCM
// Resample 48000 → 44100 Hz, convert mono → stereo, buffer for A2DP.
// ---------------------------------------------------------------------------

int BtAudioCodec::Write(const int16_t* data, int samples) {
    if (!bt_connected_.load()) {
        return samples;  // silently drop if no speaker connected
    }

    // Advance ratio: for every output sample we consume src/dst input samples
    // 48000/44100 ≈ 1.0884 — we consume more than 1 input sample per output sample
    const double ratio = static_cast<double>(output_sample_rate_) / kTargetSampleRate;

    std::vector<int16_t> out;
    out.reserve(static_cast<int>(samples / ratio + 2) * 2);  // *2 for stereo

    while (resample_pos_ < static_cast<double>(samples)) {
        int idx = static_cast<int>(resample_pos_);
        double frac = resample_pos_ - idx;

        int16_t s0 = data[idx];
        int16_t s1 = (idx + 1 < samples) ? data[idx + 1] : s0;

        // Linear interpolation
        float interp = static_cast<float>(s0) + static_cast<float>(frac) * (s1 - s0);

        // Apply volume scaling
        int32_t scaled = static_cast<int32_t>(interp * volume_scale_);
        scaled = std::max(std::min(scaled, (int32_t)INT16_MAX), (int32_t)INT16_MIN);
        int16_t sample = static_cast<int16_t>(scaled);

        out.push_back(sample);  // L
        out.push_back(sample);  // R

        resample_pos_ += ratio;
    }

    // Carry fractional overshoot into the next buffer
    resample_pos_ -= static_cast<double>(samples);
    if (resample_pos_ < 0.0) resample_pos_ = 0.0;

    if (!out.empty()) {
        // Non-blocking: drop data rather than block AudioOutputTask
        xRingbufferSend(pcm_ringbuf_, out.data(),
                        out.size() * sizeof(int16_t), 0);
    }
    return samples;
}

// ---------------------------------------------------------------------------
// Read — I2S microphone input (32-bit → 16-bit, matching NoAudioCodec)
// ---------------------------------------------------------------------------

int BtAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    std::vector<int32_t> buf32(samples);

    if (i2s_channel_read(rx_handle_, buf32.data(),
                         samples * sizeof(int32_t), &bytes_read,
                         pdMS_TO_TICKS(200)) != ESP_OK) {
        return 0;
    }
    int n = bytes_read / sizeof(int32_t);
    for (int i = 0; i < n; i++) {
        int32_t v = buf32[i] >> 12;
        dest[i] = (v > INT16_MAX) ? INT16_MAX : (v < -INT16_MAX) ? -INT16_MAX : (int16_t)v;
    }
    return n;
}

// ---------------------------------------------------------------------------
// A2DP data callback — called by the BT stack when it needs PCM audio data.
// Runs in the BT task context; must return promptly.
// ---------------------------------------------------------------------------

int32_t BtAudioCodec::A2dDataCallback(uint8_t* buf, int32_t len) {
    if (!instance_ || !instance_->pcm_ringbuf_) {
        memset(buf, 0, len);
        return len;
    }

    size_t received = 0;
    void* item = xRingbufferReceiveUpTo(instance_->pcm_ringbuf_,
                                         &received, 0, (size_t)len);
    if (item) {
        memcpy(buf, item, received);
        vRingbufferReturnItem(instance_->pcm_ringbuf_, item);
        if (received < (size_t)len) {
            memset(buf + received, 0, len - received);
        }
    } else {
        // Ring buffer empty: send silence to avoid glitches
        memset(buf, 0, len);
    }
    return len;
}

// ---------------------------------------------------------------------------
// A2DP event callback
// ---------------------------------------------------------------------------

void BtAudioCodec::A2dCallback(esp_a2d_cb_event_t event,
                                 esp_a2d_cb_param_t* param) {
    if (!instance_) return;

    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            auto state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                memcpy(instance_->connected_bda_,
                       param->conn_stat.remote_bda,
                       sizeof(esp_bd_addr_t));
                instance_->bt_connected_.store(true);
                ESP_LOGI(TAG, "A2DP connected to " ESP_BD_ADDR_STR,
                         ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                instance_->bt_connected_.store(false);
                ESP_LOGI(TAG, "A2DP disconnected");
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// GAP callback — handles Bluetooth device discovery
// ---------------------------------------------------------------------------

void BtAudioCodec::GapCallback(esp_bt_gap_cb_event_t event,
                                 esp_bt_gap_cb_param_t* param) {
    if (!instance_) return;

    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        const esp_bd_addr_t& bda = param->disc_res.bda;
        char bda_str[18];
        snprintf(bda_str, sizeof(bda_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        std::string name;
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            auto& p = param->disc_res.prop[i];
            if (p.type == ESP_BT_GAP_DEV_PROP_BDNAME && p.len > 0) {
                name.assign(reinterpret_cast<char*>(p.val), p.len);
                break;
            }
        }
        ESP_LOGI(TAG, "Found: %s  name=\"%s\"", bda_str, name.c_str());

        std::string entry = name + "|" + bda_str;
        std::lock_guard<std::mutex> lock(instance_->devices_mutex_);
        // Deduplicate
        for (const auto& e : instance_->discovered_devices_) {
            if (e == entry) return;
        }
        instance_->discovered_devices_.push_back(entry);

    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "BT scan complete, found %zu device(s)",
                     instance_->discovered_devices_.size());
        }
    }
}

#endif  // CONFIG_BT_A2DP_ENABLE
