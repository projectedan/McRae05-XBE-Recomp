// rebuild_font sheet.bmp rows.txt out.png weight family [family...]
// Previews a game font sheet rebuilt from a clean typeface: each mapped glyph is redrawn at 4x with DirectWrite in the
// original's place (its ink centred on the original's, bottoms aligned, one size for the sheet fitted to the
// originals' heights); unmapped glyphs (icons) keep the original, enlarged. Writes strips of the rows drawn at the
// port's 3x: now (bilinear), sharper sampling (Catmull-Rom), then each family. rows.txt: "y0 y1 glyphs" per line in
// UTF-8, one character per inked run of the row, '~' for a glyph to keep.
// Build: clang++ -O2 -std=c++17 rebuild_font.cpp -o rebuild_font.exe -ld2d1 -ldwrite -lwindowscodecs -lole32
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

constexpr int kUp = 4, kScale = 3, kLabel = 190;

struct Gray {
    int w = 0, h = 0;
    std::vector<float> v;
    float At(int x, int y) const { return v[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
    float Bilinear(float x, float y) const {
        x -= 0.5f, y -= 0.5f;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float fx = x - x0, fy = y - y0;
        return (At(x0, y0) * (1 - fx) + At(x0 + 1, y0) * fx) * (1 - fy) + (At(x0, y0 + 1) * (1 - fx) + At(x0 + 1, y0 + 1) * fx) * fy;
    }
    float Cubic(float x, float y) const {
        auto k = [](float t) {
            t = std::fabs(t);
            return t < 1 ? 1.5f * t * t * t - 2.5f * t * t + 1 : t < 2 ? -0.5f * t * t * t + 2.5f * t * t - 4 * t + 2 : 0.0f;
        };
        x -= 0.5f, y -= 0.5f;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float s = 0;
        for (int j = -1; j <= 2; j++)
            for (int i = -1; i <= 2; i++) s += At(x0 + i, y0 + j) * k(x - (x0 + i)) * k(y - (y0 + j));
        return std::clamp(s, 0.0f, 1.0f);
    }
};

Gray LoadAlpha(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    uint32_t off;
    int32_t w, h;
    memcpy(&off, &d[10], 4);
    memcpy(&w, &d[18], 4);
    memcpy(&h, &d[22], 4);
    bool down = h < 0;
    h = std::abs(h);
    Gray g{w, h, std::vector<float>(size_t(w) * h)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g.v[size_t(y) * w + x] = uint8_t(d[off + (size_t(down ? y : h - 1 - y) * w + x) * 4 + 3]) / 255.0f;
    return g;
}

struct Box {
    int x0, x1, y0, y1;  // ink, half open
};

ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;

// a glyph's coverage drawn white at em size, with its ink box
Gray DrawGlyph(const std::wstring& family, int weight, float em, wchar_t c, Box& ink) {
    const int n = int(em * 2) + 16;
    IWICBitmap* bmp;
    g_wic->CreateBitmap(n, n, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_d2d->CreateWicBitmapRenderTarget(bmp, p, &rt);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    IDWriteTextFormat* tf;
    g_dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, em, L"en-us", &tf);
    ID2D1SolidColorBrush* white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->DrawText(&c, 1, tf, D2D1::RectF(8, 4, float(n), float(n)), white);
    rt->EndDraw();
    Gray g{n, n, std::vector<float>(size_t(n) * n)};
    IWICBitmapLock* lock;
    WICRect r{0, 0, n, n};
    bmp->Lock(&r, WICBitmapLockRead, &lock);
    UINT size;
    BYTE* px;
    UINT stride;
    lock->GetDataPointer(&size, &px);
    lock->GetStride(&stride);
    ink = {n, 0, n, 0};
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float a = px[size_t(y) * stride + x * 4 + 3] / 255.0f;
            g.v[size_t(y) * n + x] = a;
            if (a > 0.15f) ink = {std::min(ink.x0, x), std::max(ink.x1, x + 1), std::min(ink.y0, y), std::max(ink.y1, y + 1)};
        }
    lock->Release();
    white->Release();
    tf->Release();
    rt->Release();
    bmp->Release();
    return g;
}

struct Glyph {
    wchar_t c;
    Box box;
};

std::vector<Glyph> Segment(const Gray& a, int y0, int y1, const std::wstring& chars) {
    std::vector<Box> runs;
    for (int x = 0; x < a.w;) {
        auto inked = [&](int xx) {
            for (int y = y0; y < y1; y++)
                if (a.At(xx, y) > 0.15f) return true;
            return false;
        };
        if (!inked(x)) {
            x++;
            continue;
        }
        Box b{x, x, y1, y0};
        while (x < a.w && inked(x)) x++;
        b.x1 = x;
        for (int y = y0; y < y1; y++)
            for (int xx = b.x0; xx < b.x1; xx++)
                if (a.At(xx, y) > 0.15f) b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y + 1);
        runs.push_back(b);
    }
    if (runs.size() != chars.size())
        fprintf(stderr, "row %d-%d: %zu runs, %zu characters given\n", y0, y1, runs.size(), chars.size());
    std::vector<Glyph> out;
    for (size_t i = 0; i < runs.size() && i < chars.size(); i++) out.push_back({chars[i], runs[i]});
    return out;
}

void SavePng(const wchar_t* path, int w, int h, const std::vector<uint8_t>& bgra) {
    IWICStream* s;
    g_wic->CreateStream(&s);
    s->InitializeFromFilename(path, GENERIC_WRITE);
    IWICBitmapEncoder* e;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e);
    e->Initialize(s, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* fr;
    e->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(h, w * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data()));
    fr->Commit();
    e->Commit();
    fr->Release();
    e->Release();
    s->Release();
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    char sheet[512], rows_path[512];
    wcstombs(sheet, argv[1], 512);
    wcstombs(rows_path, argv[2], 512);
    Gray a = LoadAlpha(sheet);
    int weight = _wtoi(argv[4]);
    std::vector<std::wstring> families(argv + 5, argv + argc);

    struct Row {
        int y0, y1;
        std::vector<Glyph> glyphs;
    };
    std::vector<Row> rows;
    std::ifstream rf(rows_path);
    for (std::string line; std::getline(rf, line);) {
        std::istringstream ls(line);
        int y0, y1;
        std::string s;
        ls >> y0 >> y1 >> s;
        std::wstring w(s.size(), 0);
        w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size())));
        rows.push_back({y0, y1, Segment(a, y0, y1, w)});
    }

    // strips: label column, then the rows at 3x
    std::vector<std::pair<std::wstring, Gray>> strips;
    auto strip = [&](auto sample) {
        int rh = 0;
        for (auto& r : rows) rh += (r.y1 - r.y0 + 4) * kScale;
        Gray s{a.w * kScale, rh, std::vector<float>(size_t(a.w) * kScale * rh)};
        int oy = 0;
        for (auto& r : rows) {
            for (int y = 0; y < (r.y1 - r.y0 + 4) * kScale; y++)
                for (int x = 0; x < a.w * kScale; x++)
                    s.v[size_t(oy + y) * s.w + x] = sample((x + 0.5f) / kScale, r.y0 - 2 + (y + 0.5f) / kScale);
            oy += (r.y1 - r.y0 + 4) * kScale;
        }
        return s;
    };
    strips.push_back({L"now", strip([&](float x, float y) { return std::clamp(a.Bilinear(x, y), 0.0f, 1.0f); })});
    strips.push_back({L"sharper sampling", strip([&](float x, float y) { return a.Cubic(x, y); })});

    for (const std::wstring& fam : families) {
        // one em size for the sheet: the originals' ink heights against the family's, letters and digits only
        std::vector<float> ratios;
        for (auto& r : rows)
            for (auto& g : r.glyphs) {
                if (g.c == L'~' || !iswalnum(g.c)) continue;
                Box ink;
                DrawGlyph(fam, weight, 100, g.c, ink);
                if (ink.y1 > ink.y0) ratios.push_back(float(g.box.y1 - g.box.y0) / float(ink.y1 - ink.y0));
            }
        std::sort(ratios.begin(), ratios.end());
        float em = 100 * ratios[ratios.size() / 2] * kUp;
        // the rebuilt sheet at 4x: the original enlarged, each mapped glyph cleared and redrawn
        Gray big{a.w * kUp, a.h * kUp, std::vector<float>(size_t(a.w) * kUp * a.h * kUp)};
        for (int y = 0; y < big.h; y++)
            for (int x = 0; x < big.w; x++) big.v[size_t(y) * big.w + x] = a.Cubic((x + 0.5f) / kUp, (y + 0.5f) / kUp);
        for (auto& r : rows)
            for (auto& g : r.glyphs) {
                if (g.c == L'~') continue;
                Box ink;
                Gray gl = DrawGlyph(fam, weight, em, g.c, ink);
                if (ink.x1 <= ink.x0) continue;
                for (int y = (g.box.y0 - 1) * kUp; y < (g.box.y1 + 1) * kUp; y++)
                    for (int x = (g.box.x0 - 1) * kUp; x < (g.box.x1 + 1) * kUp; x++)
                        if (x >= 0 && y >= 0 && x < big.w && y < big.h) big.v[size_t(y) * big.w + x] = 0;
                float cx = (g.box.x0 + g.box.x1) * 0.5f * kUp, bottom = float(g.box.y1 * kUp);
                int dx = int(std::lround(cx - (ink.x0 + ink.x1) * 0.5f)), dy = int(std::lround(bottom - ink.y1));
                for (int y = ink.y0; y < ink.y1; y++)
                    for (int x = ink.x0; x < ink.x1; x++) {
                        int tx = x + dx, ty = y + dy;
                        if (tx >= 0 && ty >= 0 && tx < big.w && ty < big.h)
                            big.v[size_t(ty) * big.w + tx] = std::max(big.v[size_t(ty) * big.w + tx], gl.v[size_t(y) * gl.w + x]);
                    }
            }
        strips.push_back({fam + (weight >= 600 ? L" bold" : L""),
                          strip([&](float x, float y) { return std::clamp(big.Bilinear(x * kUp, y * kUp), 0.0f, 1.0f); })});
    }

    // the strips stacked, each with its name in the label column, white over dark grey
    int w = kLabel + a.w * kScale, h = 0;
    for (auto& s : strips) h += s.second.h + 10;
    std::vector<uint8_t> out(size_t(w) * h * 4, 255);
    int oy = 0;
    for (auto& [name, s] : strips) {
        for (int y = 0; y < s.h + 10; y++)
            for (int x = 0; x < w; x++) {
                float c = (x >= kLabel && y < s.h) ? s.v[size_t(y) * s.w + (x - kLabel)] : 0;
                uint8_t v = uint8_t(40 + 215 * c);
                uint8_t* p = &out[(size_t(oy + y) * w + x) * 4];
                p[0] = p[1] = p[2] = v, p[3] = 255;
            }
        Box ink;
        float lem = 22;
        for (size_t i = 0; i < name.size(); i++) (void)i;
        // label: drawn into a small coverage image with Segoe UI
        IWICBitmap* bmp;
        g_wic->CreateBitmap(kLabel, 40, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        ID2D1RenderTarget* rt;
        g_d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt);
        IDWriteTextFormat* tf;
        g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 17, L"en-us", &tf);
        ID2D1SolidColorBrush* br;
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.75f, 0.3f, 1), &br);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt->DrawText(name.c_str(), UINT32(name.size()), tf, D2D1::RectF(6, 6, float(kLabel), 40), br);
        rt->EndDraw();
        IWICBitmapLock* lock;
        WICRect r{0, 0, kLabel, 40};
        bmp->Lock(&r, WICBitmapLockRead, &lock);
        UINT size, stride;
        BYTE* px;
        lock->GetDataPointer(&size, &px);
        lock->GetStride(&stride);
        for (int y = 0; y < 40 && y < s.h; y++)
            for (int x = 0; x < kLabel; x++) {
                BYTE* q = &px[size_t(y) * stride + x * 4];
                uint8_t* p = &out[(size_t(oy + y) * w + x) * 4];
                float al = q[3] / 255.0f;
                for (int k = 0; k < 3; k++) p[k] = uint8_t(p[k] * (1 - al) + q[k]);
            }
        lock->Release();
        br->Release();
        tf->Release();
        rt->Release();
        bmp->Release();
        (void)ink, (void)lem;
        oy += s.h + 10;
    }
    SavePng(argv[3], w, h, out);
    printf("wrote %d x %d, %zu strips\n", w, h, strips.size());
}
