#ifndef _BT_SPEAKER_MCP_TOOL_H_
#define _BT_SPEAKER_MCP_TOOL_H_

#include <sdkconfig.h>

#ifdef CONFIG_USE_BT_SPEAKER

#include "codecs/bt_audio_codec.h"
#include "mcp_server.h"

/**
 * BtSpeakerMcpTool — registers MCP tools for controlling the Bluetooth speaker.
 *
 * Exposed tools:
 *   self.bluetooth.scan        — scan for nearby BT speakers (~5 s)
 *   self.bluetooth.connect     — connect to a device by address (xx:xx:xx:xx:xx:xx)
 *   self.bluetooth.disconnect  — disconnect from the current speaker
 *   self.bluetooth.list        — list devices found in the last scan
 *
 * Usage in *_board.cc (the codec must be a BtAudioCodec):
 *   bt_mcp_tool_ = new BtSpeakerMcpTool(bt_codec_);
 *   bt_mcp_tool_->Initialize();
 */
class BtSpeakerMcpTool {
public:
    explicit BtSpeakerMcpTool(BtAudioCodec* codec);

    /** Register all BT tools with the global McpServer. */
    void Initialize();

private:
    BtAudioCodec* codec_;  // owned by the Board, outlives this object
};

#endif  // CONFIG_USE_BT_SPEAKER
#endif  // _BT_SPEAKER_MCP_TOOL_H_
