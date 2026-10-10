#pragma once

// Called before USB.begin(). Registers one explicit CDC port and starts its
// low-priority worker; no wait for a host or for the respeaker xvf3800 to boot.
bool beginRespeakerControl();

#include <stddef.h>
#include <stdint.h>
using RespeakerAudioStatusProvider = size_t (*)(uint8_t *, size_t);
// Register before USB.begin(); callback must return at most 64 bytes.
void setRespeakerAudioStatusProvider(RespeakerAudioStatusProvider provider, RespeakerAudioStatusProvider usbProvider = nullptr);
