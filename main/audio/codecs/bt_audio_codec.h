#ifndef _BT_AUDIO_CODEC_H_
#define _BT_AUDIO_CODEC_H_

#ifdef CONFIG_BT_A2DP_ENABLE

#include "audio_codec.h"

#include <driver/gpio.h>
#include <esp_a2dp_api.h>
#include <esp_bt_defs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

/**
 * BtAudioCodec — Bluetooth A2DP speaker output + I2S microphone input.
 *
 * The ESP32 acts as an A2DP Source (sends audio to a Bluetooth speaker).
 * Classic Bluetooth (BR/EDR) is required; this is only available on the
 * original ESP32. ESP32-S3/C3/C6/P4 are BLE-only and do not support A2DP.
 *
 * Required sdkconfig options:
 *   CONFIG_BT_ENABLED=y
 *   CONFIG_BT_CLASSIC_ENABLED=y
 *   CONFIG_BT_A2DP_ENABLE=y
 *   CONFIG_BT_SPP_ENABLED=n  (optional, saves RAM)
 *
 * Audio path:
 *   Upstream (mic):  I2S mic  → AudioInputTask → Opus encode → network
 *   Downstream (spk): network → Opus decode → AudioOutputTask → BT A2DP
 *
 * The codec receives 48 kHz mono PCM from the audio service and resamples
 * to 44.1 kHz stereo before handing it to the BT stack.
 */
class BtAudioCodec : public AudioCodec {
public:
    /**
     * @param input_sample_rate  Microphone sample rate in Hz (e.g. 16000)
     * @param mic_bclk           I2S bit-clock GPIO for the microphone
     * @param mic_ws             I2S word-select GPIO for the microphone
     * @param mic_din            I2S data-in GPIO for the microphone
     */
    BtAudioCodec(int input_sample_rate,
                 gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din);
    ~BtAudioCodec() override;

    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    void SetOutputVolume(int volume) override;

    /** Start a Bluetooth device scan (~5 s). Results are logged via ESP_LOGI. */
    void StartScan();

    /** Cancel an ongoing scan. */
    void StopScan();

    /**
     * Initiate an A2DP connection to a device found during scan.
     * @param bda  6-byte Bluetooth device address
     * @return true if the connect request was accepted by the stack
     */
    bool Connect(const esp_bd_addr_t bda);

    /** Disconnect from the currently connected Bluetooth speaker. */
    void Disconnect();

    /** Returns true when the A2DP link is established and streaming. */
    bool IsConnected() const { return bt_connected_.load(); }

    /** Returns the most recently discovered device list as "name|xx:xx:xx:xx:xx:xx" pairs. */
    std::vector<std::string> GetDiscoveredDevices() const;

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

private:
    // Singleton pointer for static BT callbacks
    static BtAudioCodec* instance_;

    static void GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    static void A2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static int32_t A2dDataCallback(uint8_t* buf, int32_t len);

    void InitBluetooth();

    // Thread-safe PCM ring buffer shared between Write() and A2dDataCallback()
    RingbufHandle_t pcm_ringbuf_ = nullptr;

    std::atomic<bool> bt_connected_{false};
    std::mutex i2s_mutex_;

    // Address of the currently connected device (used for disconnection)
    esp_bd_addr_t connected_bda_{};

    // Discovered devices: list of "name|xx:xx:xx:xx:xx:xx" strings
    mutable std::mutex devices_mutex_;
    std::vector<std::string> discovered_devices_;

    // Linear resampler state: src=48000 Hz → dst=44100 Hz
    double resample_pos_ = 0.0;

    // Volume scale factor [0.0 – 1.0], set from output_volume_
    float volume_scale_ = 0.49f;  // default: volume=70 → 0.7^2

    static constexpr int kRingBufBytes = 64 * 1024;
    static constexpr int kTargetSampleRate = 44100;
};

#endif  // CONFIG_BT_A2DP_ENABLE
#endif  // _BT_AUDIO_CODEC_H_
