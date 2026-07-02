#ifndef _BT_AUDIO_CODEC_H_
#define _BT_AUDIO_CODEC_H_

#include <sdkconfig.h>

#ifdef CONFIG_USE_BT_SPEAKER

#include "audio_codec.h"

#include <driver/gpio.h>
#include <esp_a2dp_api.h>
#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// A discovered Bluetooth device (deduplicated by address; name may arrive
// in a later inquiry event and is updated in place).
struct BtDeviceInfo {
    std::string name;
    std::string address;  // "xx:xx:xx:xx:xx:xx"
};

/**
 * BtAudioCodec — Bluetooth A2DP speaker output + I2S microphone input.
 *
 * The ESP32 acts as an A2DP Source (sends audio to a Bluetooth speaker).
 * Classic Bluetooth (BR/EDR) is required; this is only available on the
 * original ESP32. ESP32-S3/C3/C6/P4 are BLE-only and do not support A2DP.
 *
 * Enable via the project option CONFIG_USE_BT_SPEAKER (menuconfig →
 * Xiaozhi Assistant), which requires the BT stack options:
 *   CONFIG_BT_ENABLED=y
 *   CONFIG_BT_CLASSIC_ENABLED=y
 *   CONFIG_BT_A2DP_ENABLE=y
 *
 * The constructor takes ownership of the BT controller in Classic-BT mode
 * and releases BLE controller memory. This codec therefore cannot be
 * combined with BLE features such as BluFi WiFi provisioning
 * (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING).
 *
 * Audio path:
 *   Upstream (mic):  I2S mic  → AudioInputTask → Opus encode → network
 *   Downstream (spk): network → Opus decode → AudioOutputTask → BT A2DP
 *
 * The codec reports output_sample_rate() = 44100 so AudioService's rate
 * converter delivers 44.1 kHz mono PCM to Write(); Write() only applies
 * volume, duplicates to stereo, and hands the PCM to the BT stack.
 */
class BtAudioCodec : public AudioCodec {
public:
    /**
     * @param input_sample_rate  Microphone sample rate in Hz (e.g. 16000)
     * @param mic_bclk           I2S bit-clock GPIO for the microphone
     * @param mic_ws             I2S word-select GPIO for the microphone
     * @param mic_din            I2S data-in GPIO for the microphone
     */
    BtAudioCodec(int input_sample_rate, gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din);
    ~BtAudioCodec() override;

    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

    /** Start a Bluetooth device scan (~5 s). Results via GetDiscoveredDevices(). */
    void StartScan();

    /**
     * Initiate an A2DP connection to a device found during scan.
     * @param bda  6-byte Bluetooth device address
     * @return true if the connect request was accepted by the stack
     */
    bool Connect(const esp_bd_addr_t bda);

    /** Disconnect from the currently connected Bluetooth speaker. */
    void Disconnect();

    /** Returns true when the A2DP link is established. */
    bool IsConnected() const { return bt_connected_.load(); }

    /** Returns the device list from the most recent scan. */
    std::vector<BtDeviceInfo> GetDiscoveredDevices() const;

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;

private:
    // Singleton pointer for static BT callbacks (single instance enforced
    // by an assert in the constructor).
    static BtAudioCodec* instance_;

    static void GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    static void A2dCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static int32_t A2dDataCallback(uint8_t* buf, int32_t len);

    void InitBluetooth();

    // Thread-safe PCM ring buffer shared between Write() and A2dDataCallback()
    RingbufHandle_t pcm_ringbuf_ = nullptr;

    std::atomic<bool> bt_connected_{false};

    // Address of the currently connected device (used for disconnection)
    esp_bd_addr_t connected_bda_{};

    mutable std::mutex devices_mutex_;
    std::vector<BtDeviceInfo> discovered_devices_;

    // Reused by Write() to avoid a heap allocation on every audio frame
    std::vector<int16_t> stereo_buffer_;

    static constexpr int PCM_RING_BUF_BYTES = 32 * 1024;  // ~186 ms at 44.1 kHz stereo
    static constexpr int A2DP_SAMPLE_RATE = 44100;
};

#endif  // CONFIG_USE_BT_SPEAKER
#endif  // _BT_AUDIO_CODEC_H_
