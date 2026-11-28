#include "core/loader/audio_recorder.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace holy::loader {
namespace {

constexpr uint32_t kRate = 48000, kChannels = 2, kBits = 16;

// Waits for the asynchronous activation of the loopback client.
class Activation : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                       Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
 public:
  Activation() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Activation() override {
    if (done_) CloseHandle(done_);
  }
  STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) override {
    SetEvent(done_);
    return S_OK;
  }
  HANDLE done() const { return done_; }

 private:
  HANDLE done_;
};

void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }

void write_header(FILE* f, uint32_t data_bytes) {
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);  // PCM
  put16(f, kChannels);
  put32(f, kRate);
  put32(f, kRate * kChannels * kBits / 8);
  put16(f, kChannels * kBits / 8);
  put16(f, kBits);
  std::fwrite("data", 1, 4, f);
  put32(f, data_bytes);
}

}  // namespace

struct AudioRecorder::State {
  DWORD process_id = 0;
  std::wstring path;
  std::atomic<bool> stop{false};
  std::thread thread;
  // Results, read after the thread ended.
  uint64_t frames = 0, silent_frames = 0, discontinuities = 0;
  ULONGLONG started = 0, ended = 0;
  std::string error;
};

namespace {

void record(AudioRecorder::State* s) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  FILE* file = _wfopen(s->path.c_str(), L"wb");
  if (!file) {
    s->error = "cannot create the file";
    if (SUCCEEDED(com)) CoUninitialize();
    return;
  }
  write_header(file, 0);
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = s->process_id;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT activation{};
  activation.vt = VT_BLOB;
  activation.blob.cbSize = sizeof(params);
  activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
  auto handler = Microsoft::WRL::Make<Activation>();
  IActivateAudioInterfaceAsyncOperation* operation = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation,
                                           handler.Get(), &operation);
  if (SUCCEEDED(hr)) {
    WaitForSingleObject(handler->done(), 10000);
    HRESULT activated = E_FAIL;
    IUnknown* unknown = nullptr;
    hr = operation->GetActivateResult(&activated, &unknown);
    if (SUCCEEDED(hr)) hr = activated;
    if (SUCCEEDED(hr) && unknown) hr = unknown->QueryInterface(IID_PPV_ARGS(&client));
    if (unknown) unknown->Release();
  }
  if (SUCCEEDED(hr)) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = kBits;
    format.nBlockAlign = kChannels * kBits / 8;
    format.nAvgBytesPerSec = kRate * format.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                            200 * 10000, 0, &format, nullptr);
  }
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
  if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    char text[64];
    std::snprintf(text, sizeof(text), "the host refused the capture (0x%08lX)", static_cast<unsigned long>(hr));
    s->error = text;
  }
  uint32_t data_bytes = 0;
  std::vector<uint8_t> zeros;
  s->started = GetTickCount64();
  while (SUCCEEDED(hr) && !s->stop.load()) {
    WaitForSingleObject(ready, 100);
    UINT32 packet = 0;
    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      const size_t bytes = static_cast<size_t>(frames) * kChannels * kBits / 8;
      if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++s->discontinuities;
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        zeros.assign(bytes, 0);
        std::fwrite(zeros.data(), 1, bytes, file);
        s->silent_frames += frames;
      } else {
        std::fwrite(data, 1, bytes, file);
      }
      s->frames += frames;
      data_bytes += static_cast<uint32_t>(bytes);
      capture->ReleaseBuffer(frames);
    }
  }
  s->ended = GetTickCount64();
  if (client) client->Stop();
  write_header(file, data_bytes);
  std::fclose(file);
  if (capture) capture->Release();
  if (client) client->Release();
  if (operation) operation->Release();
  CloseHandle(ready);
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace

AudioRecorder::AudioRecorder(DWORD process_id, std::wstring wav_path) : state_(std::make_unique<State>()) {
  state_->process_id = process_id;
  state_->path = std::move(wav_path);
  state_->thread = std::thread(record, state_.get());
}

AudioRecorder::~AudioRecorder() {
  state_->stop.store(true);
  if (state_->thread.joinable()) state_->thread.join();
  if (!state_->error.empty()) {
    std::fprintf(stderr, "holy: audio not recorded: %s\n", state_->error.c_str());
    return;
  }
  const double wall = static_cast<double>(state_->ended - state_->started) / 1000.0;
  const double audio = static_cast<double>(state_->frames) / kRate;
  std::fprintf(stderr,
               "holy: audio recorded to %ls: %.1f s over %.1f s of wall clock, %.1f s of it flagged silent, %llu "
               "discontinuities\n",
               state_->path.c_str(), audio, wall, static_cast<double>(state_->silent_frames) / kRate,
               static_cast<unsigned long long>(state_->discontinuities));
}

}  // namespace holy::loader
