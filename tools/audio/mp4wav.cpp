// mp4wav in.mp4 out.wav: the first audio stream as 16 bit PCM through Media Foundation
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdio>
#include <vector>
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    IMFSourceReader* r = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(argv[1], nullptr, &r))) { printf("open failed\n"); return 1; }
    r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType* t = nullptr; MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t))) { printf("pcm failed\n"); return 1; }
    IMFMediaType* out = nullptr; r->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &out);
    UINT32 rate = 0, ch = 0; out->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); out->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    FILE* f = _wfopen(argv[2], L"wb"); std::vector<char> hdr(44); fwrite(hdr.data(), 1, 44, f);
    size_t bytes = 0;
    for (;;) {
        DWORD flags = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        IMFMediaBuffer* b = nullptr; s->ConvertToContiguousBuffer(&b);
        BYTE* p; DWORD n; b->Lock(&p, nullptr, &n); fwrite(p, 1, n, f); bytes += n; b->Unlock(); b->Release(); s->Release();
    }
    auto w32 = [&](DWORD v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](WORD v) { fwrite(&v, 2, 1, f); };
    fseek(f, 0, SEEK_SET); fwrite("RIFF", 1, 4, f); w32(DWORD(36 + bytes)); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(WORD(ch));
    w32(rate); w32(rate * ch * 2); w16(WORD(ch * 2)); w16(16); fwrite("data", 1, 4, f); w32(DWORD(bytes)); fclose(f);
    printf("%u Hz, %u channels, %.1f s\n", rate, ch, bytes / double(rate * ch * 2));
}
