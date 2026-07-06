//
// Bandwidth-tier policy for multi-slot operation. The scarce resource is
// Bluetooth AIR TIME, not CPU: one controller's duplex audio (160 kbps CBR
// Opus + ~93 Hz 398-byte 0x36 frames) already approaches the sustainable
// BR/EDR budget, so audio must yield as slots fill. Adaptive triggers and
// classic rumble stay available at EVERY tier — they ride the ordinary
// 78-byte output report and cost no meaningful airtime.
//
//   1 pad        : full audio (speaker + headset + mic) + HD haptics
//   2 pads       : audio only on the designated audio slot
//   3-4 pads     : audio off entirely (classic rumble fallback)
//

#ifndef DS5_BRIDGE_TIER_H
#define DS5_BRIDGE_TIER_H

#include <cstdint>

#include "slots.h"

// The slot whose controller owns the audio path (speaker/haptics frames, mic,
// mute, headset jack). Configurable via the web UI (config audio_slot),
// clamped to a valid slot. Single-slot builds always return 0.
uint8_t tier_audio_slot();

// May the audio path stream to the controller right now? False at 3+
// connected pads; audio_loop then drains the UAC FIFO without emitting BT
// audio frames, which also makes the controller fall back to classic rumble.
bool tier_audio_allowed();

#endif // DS5_BRIDGE_TIER_H
