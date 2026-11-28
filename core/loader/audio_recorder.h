// Records what the title process plays, from the launcher, for checking
// audio without listening to it.
//
// The host's audio engine can capture one process's output (and its child
// processes'), whatever else plays on the host (process loopback, documented
// for Windows 10 2004 and later). The recorder captures the title's process
// tree as 48 kHz stereo 16-bit samples into a WAV file, from its own thread,
// until it is destroyed; stretches the engine flags as silent are written as
// zeros, so the file runs at the wall clock and gaps in the title's sound
// show as silence in it (tools/audio_report analyses it). Packets the engine
// flags as discontinuous (it dropped some) are counted.
#pragma once

#include <windows.h>

#include <memory>
#include <string>

namespace holy::loader {

class AudioRecorder {
 public:
  AudioRecorder(DWORD process_id, std::wstring wav_path);
  ~AudioRecorder();  // stops recording, completes the file and prints a summary

  AudioRecorder(const AudioRecorder&) = delete;
  AudioRecorder& operator=(const AudioRecorder&) = delete;

  struct State;

 private:
  std::unique_ptr<State> state_;
};

}  // namespace holy::loader
