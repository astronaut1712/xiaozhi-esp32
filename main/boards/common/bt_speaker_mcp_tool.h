#ifndef _BT_SPEAKER_MCP_TOOL_H_
#define _BT_SPEAKER_MCP_TOOL_H_

#ifdef CONFIG_BT_A2DP_ENABLE

#include "mcp_server.h"
#include "bt_audio_codec.h"

/**
 * BtSpeakerMcpTool — registers MCP tools for controlling the Bluetooth speaker.
 *
 * Exposed tools:
 *   bluetooth.scan        — scan for nearby BT speakers (~5 s)
 *   bluetooth.connect     — connect to a device by address (xx:xx:xx:xx:xx:xx)
 *   bluetooth.disconnect  — disconnect from the current speaker
 *   bluetooth.list        — list devices found in the last scan
 *
 * Usage in *_board.cc:
 *   BtSpeakerMcpTool bt_tool(GetAudioCodec());  // stored as member
 *   bt_tool.Initialize();                         // call after Board init
 */
class BtSpeakerMcpTool {
public:
    explicit BtSpeakerMcpTool(AudioCodec* codec);

    /** Register all BT tools with the global McpServer. */
    void Initialize();

private:
    BtAudioCodec* codec_;  // owned by the Board, outlives this object
};

#endif  // CONFIG_BT_A2DP_ENABLE
#endif  // _BT_SPEAKER_MCP_TOOL_H_
