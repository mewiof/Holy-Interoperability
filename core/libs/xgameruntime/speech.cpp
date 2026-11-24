// Speech synthesis (text to speech).
//
// Observed: interface {d722b373-8c4d-4692-9e51-d6ad9b37aa7d}, provider
// {3e241977-6237-41e9-8559-102c2d9983f1}; the client library's thunks pass
// the arguments of the documented functions unchanged:
//
//    8 XSpeechSynthesizerEnumerateInstalledVoices(context, callback)
//    9 XSpeechSynthesizerCreate(XSpeechSynthesizerHandle*)
//   10 XSpeechSynthesizerCloseHandle(handle)
//   11 XSpeechSynthesizerSetDefaultVoice(handle)
//   12 XSpeechSynthesizerSetCustomVoice(handle, const char* voice_id)
//   13 XSpeechSynthesizerCreateStreamFromText(handle, const char*, stream*)
//   14 XSpeechSynthesizerCloseStreamHandle(stream)
//   15 XSpeechSynthesizerGetStreamDataSize(stream, size_t*)
//   16 XSpeechSynthesizerGetStreamData(stream, size_t, void*, size_t*)
//   21 XSpeechSynthesizerCreateStreamFromSsml(handle, const char*, stream*)
//
// (slots from the thunks' vtable offsets and argument counts; behaviour as
// documented). The host's own speech engine (SAPI 5, part of Windows) does
// the work: voices are its installed voices, streams are its synthesis
// output in a WAVE container (documented: audio/wav). Synthesis runs when a
// stream is created. Errors of the host engine are returned as they are; a
// voice id the host does not know, and handles this library did not hand
// out, are answered with HRESULT_FROM_WIN32(ERROR_NOT_FOUND) (documented for
// invalid stream handles; assumed for the others).
#include <windows.h>
#include <sapi.h>

#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"

namespace holy::xgr {
namespace {

const HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

// Documented public layout.
struct VoiceInformation {
  const char* description;
  const char* display_name;
  uint32_t gender;  // 0 female, 1 male
  const char* voice_id;
  const char* language;  // BCP-47
};
using VoicesCallback = bool(__cdecl*)(const VoiceInformation*, void*);

struct Voice {
  std::string description, display_name, id, language;
  uint32_t gender = 0;
};

constexpr uint32_t kSynthesizerMagic = 0x53505448, kStreamMagic = 0x53545248;

struct Synthesizer {
  uint32_t magic = kSynthesizerMagic;
  std::wstring voice_id;  // empty: the system default
};

struct Stream {
  uint32_t magic = kStreamMagic;
  std::vector<uint8_t> wave;
};

std::string utf8(const wchar_t* text) {
  if (!text) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring wide(const char* text) {
  if (!text) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (size <= 1) return {};
  std::wstring out(static_cast<size_t>(size - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
  return out;
}

// COM for the calling thread for the duration of a call; a thread already
// in an apartment keeps it.
class ComScope {
 public:
  ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(result_)) CoUninitialize();
  }

 private:
  HRESULT result_;
};

std::wstring attribute(ISpObjectToken* token, const wchar_t* name) {
  ISpDataKey* attributes = nullptr;
  std::wstring value;
  if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
    wchar_t* text = nullptr;
    if (SUCCEEDED(attributes->GetStringValue(name, &text)) && text) {
      value = text;
      CoTaskMemFree(text);
    }
    attributes->Release();
  }
  return value;
}

// The host's installed voices.
HRESULT host_voices(std::vector<Voice>* voices) {
  ISpObjectTokenCategory* category = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category));
  if (FAILED(hr)) return hr;
  IEnumSpObjectTokens* tokens = nullptr;
  hr = category->SetId(SPCAT_VOICES, FALSE);
  if (SUCCEEDED(hr)) hr = category->EnumTokens(nullptr, nullptr, &tokens);
  category->Release();
  if (FAILED(hr)) return hr;
  ISpObjectToken* token = nullptr;
  while (tokens->Next(1, &token, nullptr) == S_OK) {
    Voice voice;
    wchar_t* text = nullptr;
    if (SUCCEEDED(token->GetId(&text)) && text) {
      voice.id = utf8(text);
      CoTaskMemFree(text);
    }
    if (SUCCEEDED(token->GetStringValue(nullptr, &text)) && text) {
      voice.description = utf8(text);
      CoTaskMemFree(text);
    }
    voice.display_name = utf8(attribute(token, L"Name").c_str());
    voice.gender = _wcsicmp(attribute(token, L"Gender").c_str(), L"Male") == 0 ? 1 : 0;
    // SAPI: hexadecimal LCIDs separated by semicolons, the first preferred.
    const std::wstring lcids = attribute(token, L"Language");
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (!lcids.empty() &&
        LCIDToLocaleName(static_cast<LCID>(wcstoul(lcids.c_str(), nullptr, 16)), locale, LOCALE_NAME_MAX_LENGTH, 0)) {
      voice.language = utf8(locale);
    }
    if (voice.description.empty()) voice.description = voice.display_name + " - " + voice.language;
    token->Release();
    if (!voice.id.empty()) voices->push_back(std::move(voice));
  }
  tokens->Release();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE enumerate_installed_voices(Object*, void* context, VoicesCallback callback) {
  if (!callback) return E_INVALIDARG;
  ComScope com;
  // The strings handed to callbacks stay valid for the process: voices
  // found once are kept (in a list, whose elements never move).
  static std::mutex lock;
  static auto* known = new std::list<Voice>();
  std::vector<Voice> found;
  const HRESULT hr = host_voices(&found);
  LIB_LOG(holy::lib::kDebug, "xspeech", "%zu host voices (0x%08lx)", found.size(), static_cast<unsigned long>(hr));
  if (FAILED(hr)) return hr;
  std::vector<VoiceInformation> list;
  {
    std::lock_guard<std::mutex> guard(lock);
    for (const Voice& voice : found) {
      const Voice* kept = nullptr;
      for (const Voice& old : *known) {
        if (old.id == voice.id) kept = &old;
      }
      if (!kept) kept = &known->emplace_back(voice);
      list.push_back({kept->description.c_str(), kept->display_name.c_str(), kept->gender, kept->id.c_str(),
                      kept->language.c_str()});
    }
  }
  for (const VoiceInformation& information : list) {
    if (!callback(&information, context)) break;
  }
  return S_OK;
}

Synthesizer* synthesizer_of(void* handle) {
  auto* synthesizer = static_cast<Synthesizer*>(handle);
  return synthesizer && synthesizer->magic == kSynthesizerMagic ? synthesizer : nullptr;
}

Stream* stream_of(void* handle) {
  auto* stream = static_cast<Stream*>(handle);
  return stream && stream->magic == kStreamMagic ? stream : nullptr;
}

HRESULT STDMETHODCALLTYPE create(Object*, void** handle) {
  if (!handle) return E_POINTER;
  *handle = new Synthesizer();
  return S_OK;
}

void STDMETHODCALLTYPE close_handle(Object*, void* handle) {
  Synthesizer* synthesizer = synthesizer_of(handle);
  if (!synthesizer) return;
  synthesizer->magic = 0;
  delete synthesizer;
}

HRESULT STDMETHODCALLTYPE set_default_voice(Object*, void* handle) {
  Synthesizer* synthesizer = synthesizer_of(handle);
  if (!synthesizer) return kNotFound;
  synthesizer->voice_id.clear();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE set_custom_voice(Object*, void* handle, const char* voice_id) {
  Synthesizer* synthesizer = synthesizer_of(handle);
  if (!synthesizer) return kNotFound;
  if (!voice_id) return E_INVALIDARG;
  ComScope com;
  std::vector<Voice> voices;
  const HRESULT hr = host_voices(&voices);
  if (FAILED(hr)) return hr;
  for (const Voice& voice : voices) {
    if (voice.id == voice_id) {
      synthesizer->voice_id = wide(voice_id);
      return S_OK;
    }
  }
  return kNotFound;
}

// Speaks `text` with the synthesizer's voice into a WAVE container.
HRESULT synthesize(const Synthesizer& synthesizer, const std::wstring& text, DWORD flags, std::vector<uint8_t>* wave) {
  ComScope com;
  ISpVoice* voice = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_ISpVoice,
                                reinterpret_cast<void**>(&voice));
  if (FAILED(hr)) return hr;
  if (!synthesizer.voice_id.empty()) {
    ISpObjectToken* token = nullptr;
    hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_INPROC_SERVER, IID_ISpObjectToken,
                          reinterpret_cast<void**>(&token));
    if (SUCCEEDED(hr)) hr = token->SetId(nullptr, synthesizer.voice_id.c_str(), FALSE);
    if (SUCCEEDED(hr)) hr = voice->SetVoice(token);
    if (token) token->Release();
  }
  // 16-bit mono PCM at 22.05 kHz.
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 1;
  format.nSamplesPerSec = 22050;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 2;
  format.nAvgBytesPerSec = 44100;
  IStream* memory = nullptr;
  ISpStream* output = nullptr;
  if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &memory);
  if (SUCCEEDED(hr)) {
    hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER, IID_ISpStream,
                          reinterpret_cast<void**>(&output));
  }
  if (SUCCEEDED(hr)) hr = output->SetBaseStream(memory, SPDFID_WaveFormatEx, &format);
  if (SUCCEEDED(hr)) hr = voice->SetOutput(output, TRUE);
  if (SUCCEEDED(hr)) hr = voice->Speak(text.c_str(), flags, nullptr);
  if (SUCCEEDED(hr)) hr = voice->WaitUntilDone(INFINITE);
  std::vector<uint8_t> pcm;
  if (SUCCEEDED(hr)) {
    STATSTG stat{};
    hr = memory->Stat(&stat, STATFLAG_NONAME);
    if (SUCCEEDED(hr) && stat.cbSize.QuadPart > 0xFFFFFFFFull - 44) hr = E_BOUNDS;
    if (SUCCEEDED(hr)) {
      pcm.resize(static_cast<size_t>(stat.cbSize.QuadPart));
      const LARGE_INTEGER start{};
      ULONG read = 0;
      hr = memory->Seek(start, STREAM_SEEK_SET, nullptr);
      if (SUCCEEDED(hr) && !pcm.empty()) hr = memory->Read(pcm.data(), static_cast<ULONG>(pcm.size()), &read);
      pcm.resize(read);
    }
  }
  if (output) output->Release();
  if (memory) memory->Release();
  voice->Release();
  if (FAILED(hr)) return hr;
  // RIFF WAVE: "fmt " with the PCM format, then "data".
  auto put32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) wave->push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  auto put16 = [&](uint16_t v) {
    wave->push_back(static_cast<uint8_t>(v));
    wave->push_back(static_cast<uint8_t>(v >> 8));
  };
  auto tag = [&](const char* t) { wave->insert(wave->end(), t, t + 4); };
  wave->clear();
  tag("RIFF");
  put32(static_cast<uint32_t>(36 + pcm.size()));
  tag("WAVE");
  tag("fmt ");
  put32(16);
  put16(format.wFormatTag);
  put16(format.nChannels);
  put32(format.nSamplesPerSec);
  put32(format.nAvgBytesPerSec);
  put16(format.nBlockAlign);
  put16(format.wBitsPerSample);
  tag("data");
  put32(static_cast<uint32_t>(pcm.size()));
  wave->insert(wave->end(), pcm.begin(), pcm.end());
  return S_OK;
}

HRESULT create_stream(void* handle, const char* text, DWORD flags, void** stream_handle) {
  if (!stream_handle) return E_POINTER;
  *stream_handle = nullptr;
  Synthesizer* synthesizer = synthesizer_of(handle);
  if (!synthesizer) return kNotFound;
  if (!text) return E_INVALIDARG;
  auto* stream = new Stream();
  const HRESULT hr = synthesize(*synthesizer, wide(text), flags, &stream->wave);
  LIB_LOG(FAILED(hr) ? holy::lib::kWarn : holy::lib::kDebug, "xspeech",
          "synthesis of %zu bytes of text: %zu bytes of audio (0x%08lx)", std::strlen(text), stream->wave.size(),
          static_cast<unsigned long>(hr));
  if (FAILED(hr)) {
    delete stream;
    return hr;
  }
  *stream_handle = stream;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE create_stream_from_text(Object*, void* handle, const char* text, void** stream) {
  return create_stream(handle, text, SPF_IS_NOT_XML, stream);
}

HRESULT STDMETHODCALLTYPE create_stream_from_ssml(Object*, void* handle, const char* ssml, void** stream) {
  return create_stream(handle, ssml, SPF_IS_XML | SPF_PARSE_SSML, stream);
}

void STDMETHODCALLTYPE close_stream_handle(Object*, void* handle) {
  Stream* stream = stream_of(handle);
  if (!stream) return;
  stream->magic = 0;
  delete stream;
}

HRESULT STDMETHODCALLTYPE get_stream_data_size(Object*, void* handle, size_t* size) {
  Stream* stream = stream_of(handle);
  if (!stream) return kNotFound;
  if (!size) return E_POINTER;
  *size = stream->wave.size();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_stream_data(Object*, void* handle, size_t size, void* buffer, size_t* used) {
  Stream* stream = stream_of(handle);
  if (!stream) return kNotFound;
  if (used) *used = 0;
  if (size < stream->wave.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  if (!buffer && !stream->wave.empty()) return E_POINTER;
  if (!stream->wave.empty()) std::memcpy(buffer, stream->wave.data(), stream->wave.size());
  if (used) *used = stream->wave.size();
  return S_OK;
}

}  // namespace

void register_speech() {
  // {d722b373-8c4d-4692-9e51-d6ad9b37aa7d}
  constexpr GUID kSpeech = {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};
  Interface& speech = define_interface(kSpeech, "speech synthesizer");
  set_method(speech, 8, "XSpeechSynthesizerEnumerateInstalledVoices", &enumerate_installed_voices);
  set_method(speech, 9, "XSpeechSynthesizerCreate", &create);
  set_method(speech, 10, "XSpeechSynthesizerCloseHandle", &close_handle);
  set_method(speech, 11, "XSpeechSynthesizerSetDefaultVoice", &set_default_voice);
  set_method(speech, 12, "XSpeechSynthesizerSetCustomVoice", &set_custom_voice);
  set_method(speech, 13, "XSpeechSynthesizerCreateStreamFromText", &create_stream_from_text);
  set_method(speech, 14, "XSpeechSynthesizerCloseStreamHandle", &close_stream_handle);
  set_method(speech, 15, "XSpeechSynthesizerGetStreamDataSize", &get_stream_data_size);
  set_method(speech, 16, "XSpeechSynthesizerGetStreamData", &get_stream_data);
  set_method(speech, 21, "XSpeechSynthesizerCreateStreamFromSsml", &create_stream_from_ssml);
}

}  // namespace holy::xgr
