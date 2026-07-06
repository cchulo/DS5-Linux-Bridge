//
// Bandwidth-tier policy for multi-slot operation. The scarce resource is
// Bluetooth AIR TIME, not CPU: one controller's duplex audio (160 kbps CBR
// Opus + ~93 Hz 398-byte 0x36 frames) already approaches the sustainable
// BR/EDR budget, so audio must yield as slots fill. Adaptive triggers and
// classic rumble stay available at EVERY tier — they ride the ordinary
// 78-byte output report and cost no meaningful airtime.
//
//   1 pad        : full audio (speaker + headset + mic) + HD haptics
//   2+ pads      : audio off entirely (classic rumble fallback)
//

#ifndef DS5_BRIDGE_TIER_H
#define DS5_BRIDGE_TIER_H

#include <cstdint>

#include "slots.h"

// The slot whose controller owns the audio path (speaker/haptics frames, mic,
// mute, headset jack): the lowest connected slot. Audio is only ALLOWED when
// exactly one pad is connected, so this is normally just "the" pad; with 2+
// connected the value still exists (mute state bookkeeping) but streaming is
// gated off. Slot 0 when nothing is connected.
uint8_t tier_audio_slot();

// May the audio path stream to the controller right now? False at 2+
// connected pads; audio_loop then drains the UAC FIFO without emitting BT
// audio frames, which also makes the controller fall back to classic rumble.
bool tier_audio_allowed();

#endif // DS5_BRIDGE_TIER_H
