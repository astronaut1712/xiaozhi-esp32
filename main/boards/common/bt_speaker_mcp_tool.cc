#include "bt_speaker_mcp_tool.h"

#ifdef CONFIG_BT_A2DP_ENABLE

#include <esp_log.h>
#include <esp_bt_defs.h>
#include <cstring>
#include <cstdio>
#include <sstream>

#define TAG "BtSpeakerMcpTool"

BtSpeakerMcpTool::BtSpeakerMcpTool(AudioCodec* codec)
    : codec_(static_cast<BtAudioCodec*>(codec)) {
}

void BtSpeakerMcpTool::Initialize() {
    auto& mcp = McpServer::GetInstance();

    // -----------------------------------------------------------------------
    // bluetooth.scan
    // -----------------------------------------------------------------------
    mcp.AddTool("bluetooth.scan",
        "Scan for nearby Bluetooth speakers (~5 seconds). "
        "Call bluetooth.list after the scan completes to see results.",
        PropertyList({}),
        [this](const PropertyList&) -> ReturnValue {
            codec_->StartScan();
            return std::string("Scanning for Bluetooth devices...");
        });

    // -----------------------------------------------------------------------
    // bluetooth.list
    // -----------------------------------------------------------------------
    mcp.AddTool("bluetooth.list",
        "Return the list of Bluetooth devices found in the last scan. "
        "Each entry is formatted as \"Device Name|xx:xx:xx:xx:xx:xx\".",
        PropertyList({}),
        [this](const PropertyList&) -> ReturnValue {
            auto devices = codec_->GetDiscoveredDevices();
            if (devices.empty()) {
                return std::string("No devices found. Run bluetooth.scan first.");
            }
            std::ostringstream oss;
            for (size_t i = 0; i < devices.size(); i++) {
                if (i > 0) oss << "\n";
                oss << devices[i];
            }
            return oss.str();
        });

    // -----------------------------------------------------------------------
    // bluetooth.connect
    // -----------------------------------------------------------------------
    mcp.AddTool("bluetooth.connect",
        "Connect to a Bluetooth speaker by its device address. "
        "Use bluetooth.list to get the address in \"xx:xx:xx:xx:xx:xx\" format.",
        PropertyList({
            Property("address", kPropertyTypeString),
        }),
        [this](const PropertyList& props) -> ReturnValue {
            auto addr_str = props["address"].value<std::string>();

            // Parse "xx:xx:xx:xx:xx:xx" into 6 bytes
            esp_bd_addr_t bda{};
            int parts[6];
            if (sscanf(addr_str.c_str(),
                       "%x:%x:%x:%x:%x:%x",
                       &parts[0], &parts[1], &parts[2],
                       &parts[3], &parts[4], &parts[5]) != 6) {
                throw std::runtime_error(
                    "Invalid address format. Expected xx:xx:xx:xx:xx:xx");
            }
            for (int i = 0; i < 6; i++) {
                bda[i] = static_cast<uint8_t>(parts[i]);
            }

            bool ok = codec_->Connect(bda);
            if (!ok) {
                throw std::runtime_error("Failed to initiate connection to " + addr_str);
            }
            return std::string("Connecting to " + addr_str + "...");
        });

    // -----------------------------------------------------------------------
    // bluetooth.disconnect
    // -----------------------------------------------------------------------
    mcp.AddTool("bluetooth.disconnect",
        "Disconnect from the currently connected Bluetooth speaker.",
        PropertyList({}),
        [this](const PropertyList&) -> ReturnValue {
            if (!codec_->IsConnected()) {
                return std::string("No Bluetooth speaker is connected.");
            }
            codec_->Disconnect();
            return std::string("Disconnected from Bluetooth speaker.");
        });

    ESP_LOGI(TAG, "Bluetooth MCP tools registered");
}

#endif  // CONFIG_BT_A2DP_ENABLE
