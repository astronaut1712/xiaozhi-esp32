#include "bt_audio_codec.h"

#ifdef CONFIG_USE_BT_SPEAKER

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_log.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "BtAudioCodec"

BtAudioCodec* BtAudioCodec::instance_ = nullptr;

namespace {

std::string BdaToString(const esp_bd_addr_t bda) {
    char buf[18];
    snprintf(buf, sizeof(buf), ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
    return std::string(buf);
}

// Device names commonly arrive in the EIR blob rather than as a BDNAME property.
std::string ExtractDeviceName(esp_bt_gap_dev_prop_t* props, int num_props) {
    for (int i = 0; i < num_props; i++) {
        if (props[i].type == ESP_BT_GAP_DEV_PROP_BDNAME && props[i].len > 0) {
            return std::string(reinterpret_cast<char*>(props[i].val), props[i].len);
        }
    }
    for (int i = 0; i < num_props; i++) {
        if (props[i].type != ESP_BT_GAP_DEV_PROP_EIR) {
            continue;
        }
        auto* eir = reinterpret_cast<uint8_t*>(props[i].val);
        uint8_t len = 0;
        uint8_t* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
        if (name == nullptr) {
            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
        }
        if (name != nullptr && len > 0) {
            return std::string(reinterpret_cast<char*>(name), len);
        }
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

BtAudioCodec::BtAudioCodec(int input_sample_rate, gpio_num_t mic_bclk, gpio_num_t mic_ws,
                           gpio_num_t mic_din) {
    // Static BT callbacks route through instance_; only one codec may exist.
    assert(instance_ == nullptr);
    instance_ = this;

    // AudioService's rate converter resamples decoder output to this rate,
    // so Write() receives PCM already at the A2DP sample rate.
    output_sample_rate_ = A2DP_SAMPLE_RATE;
    input_sample_rate_ = input_sample_rate;
    input_channels_ = 1;
    output_channels_ = 1;
    duplex_ = false;
    input_reference_ = false;

    pcm_ringbuf_ = xRingbufferCreate(PCM_RING_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    assert(pcm_ringbuf_ != nullptr);

    // I2S receive channel for the microphone
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
        .clk_cfg =
            {
                .sample_rate_hz = (uint32_t)input_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
#endif
            },
        .slot_cfg =
            {
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
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = mic_bclk,
                .ws = mic_ws,
                .dout = I2S_GPIO_UNUSED,
                .din = mic_din,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_std));

    InitBluetooth();

    ESP_LOGI(TAG, "BtAudioCodec ready, mic=%d Hz, bt_out=%d Hz", input_sample_rate_,
             output_sample_rate_);
}

BtAudioCodec::~BtAudioCodec() {
    if (bt_connected_.load()) {
        esp_a2d_source_disconnect(connected_bda_);
    }
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
    if (pcm_ringbuf_ != nullptr) {
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
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void BtAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    // Suspend/resume the A2DP media stream so the radio stops transmitting
    // silence when AudioService powers the output path down.
    if (bt_connected_.load()) {
        esp_a2d_media_ctrl(enable ? ESP_A2D_MEDIA_CTRL_START : ESP_A2D_MEDIA_CTRL_SUSPEND);
    }
    AudioCodec::EnableOutput(enable);
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

bool BtAudioCodec::Connect(const esp_bd_addr_t bda) {
    ESP_LOGI(TAG, "Connecting to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
    esp_err_t err = esp_a2d_source_connect(const_cast<uint8_t*>(bda));
    return err == ESP_OK;
}

void BtAudioCodec::Disconnect() {
    if (bt_connected_.load()) {
        esp_a2d_source_disconnect(connected_bda_);
    }
}

std::vector<BtDeviceInfo> BtAudioCodec::GetDiscoveredDevices() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return discovered_devices_;
}

// ---------------------------------------------------------------------------
// Write — called by AudioOutputTask with mono PCM at A2DP_SAMPLE_RATE.
// Applies volume, duplicates mono → stereo, and buffers for the BT stack.
// ---------------------------------------------------------------------------

int BtAudioCodec::Write(const int16_t* data, int samples) {
    if (!bt_connected_.load()) {
        return samples;  // silently drop if no speaker connected
    }

    // Perceived-loudness curve, same as NoAudioCodec. Recomputed from
    // output_volume_ each frame so the NVS-restored volume applies at boot.
    float scale = powf(static_cast<float>(output_volume_) / 100.0f, 2.0f);

    stereo_buffer_.resize(samples * 2);
    for (int i = 0; i < samples; i++) {
        int32_t scaled = static_cast<int32_t>(data[i] * scale);
        scaled = std::max(std::min(scaled, (int32_t)INT16_MAX), (int32_t)INT16_MIN);
        stereo_buffer_[i * 2] = static_cast<int16_t>(scaled);      // L
        stereo_buffer_[i * 2 + 1] = static_cast<int16_t>(scaled);  // R
    }

    // Non-blocking: drop data rather than block AudioOutputTask
    xRingbufferSend(pcm_ringbuf_, stereo_buffer_.data(), stereo_buffer_.size() * sizeof(int16_t),
                    0);
    return samples;
}

// ---------------------------------------------------------------------------
// Read — I2S microphone input (32-bit → 16-bit, matching NoAudioCodec)
// ---------------------------------------------------------------------------

int BtAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    std::vector<int32_t> buf32(samples);

    if (i2s_channel_read(rx_handle_, buf32.data(), samples * sizeof(int32_t), &bytes_read,
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
// Runs in the BT task context; must not block.
// ---------------------------------------------------------------------------

int32_t BtAudioCodec::A2dDataCallback(uint8_t* buf, int32_t len) {
    // The stack signals a buffer flush with buf == NULL / len < 0
    if (buf == nullptr || len <= 0) {
        return 0;
    }
    if (instance_ == nullptr || instance_->pcm_ringbuf_ == nullptr) {
        memset(buf, 0, len);
        return len;
    }

    // A byte ring buffer returns only the contiguous region up to the wrap
    // point per call, so drain in a loop until full or truly empty.
    size_t filled = 0;
    while (filled < (size_t)len) {
        size_t received = 0;
        void* item =
            xRingbufferReceiveUpTo(instance_->pcm_ringbuf_, &received, 0, (size_t)len - filled);
        if (item == nullptr) {
            break;
        }
        memcpy(buf + filled, item, received);
        vRingbufferReturnItem(instance_->pcm_ringbuf_, item);
        filled += received;
    }
    if (filled < (size_t)len) {
        memset(buf + filled, 0, len - filled);  // pad with silence
    }
    return len;
}

// ---------------------------------------------------------------------------
// A2DP event callback
// ---------------------------------------------------------------------------

void BtAudioCodec::A2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    if (instance_ == nullptr) {
        return;
    }

    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            auto state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                memcpy(instance_->connected_bda_, param->conn_stat.remote_bda,
                       sizeof(esp_bd_addr_t));
                instance_->bt_connected_.store(true);
                ESP_LOGI(TAG, "A2DP connected to " ESP_BD_ADDR_STR,
                         ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
                // The source only starts invoking the data callback after the
                // media stream is checked and started.
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                instance_->bt_connected_.store(false);
                ESP_LOGI(TAG, "A2DP disconnected");
            }
            break;
        }
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                if (instance_->output_enabled_) {
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                }
            }
            break;
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

void BtAudioCodec::GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (instance_ == nullptr) {
        return;
    }

    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        std::string address = BdaToString(param->disc_res.bda);
        std::string name = ExtractDeviceName(param->disc_res.prop, param->disc_res.num_prop);
        ESP_LOGI(TAG, "Found: %s name=\"%s\"", address.c_str(), name.c_str());

        std::lock_guard<std::mutex> lock(instance_->devices_mutex_);
        // Inquiry fires multiple events per device (the name often arrives
        // later) — dedup by address and fill in the name when it appears.
        for (auto& device : instance_->discovered_devices_) {
            if (device.address == address) {
                if (device.name.empty() && !name.empty()) {
                    device.name = name;
                }
                return;
            }
        }
        instance_->discovered_devices_.push_back({name, address});

    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            std::lock_guard<std::mutex> lock(instance_->devices_mutex_);
            ESP_LOGI(TAG, "BT scan complete, found %zu device(s)",
                     instance_->discovered_devices_.size());
        }
    }
}

#endif  // CONFIG_USE_BT_SPEAKER
