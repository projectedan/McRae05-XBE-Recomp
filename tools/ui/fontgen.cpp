// fontgen fonts_dir out_dir scale family [family...]
// CMR 2005's font sheets rebuilt from a typeface: each character the game's table (.pcf) places on a sheet (.dds) is
// drawn with DirectWrite in the first family, at scale times the sheet's size, sized so capitals match the sheet's cap
// height, on its baseline, centred where the game's letter was. Icons and symbols the family draws much bigger or
// smaller than the game (controller buttons) keep the game's own, enlarged. The sheets go to out_dir under the names
// textures\replacement\ expects. preview.png shows real game text now and with each family. The 3d_lcd digits stay.
// Weights follow the game's names: HNBL Black, hel_60l Light, hel_20m and hec12med Medium (hec condensed), Hev Heavy.
// Build: clang++ -O2 -std=c++17 fontgen.cpp -o fontgen.exe -ld2d1 -ldwrite -lwindowscodecs -lole32
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Gray {
    int w = 0, h = 0;
    std::vector<float> a;  // coverage
    std::vector<uint32_t> rgb;  // 0xRRGGBB, the game's sheet only
    float At(int x, int y) const { return a[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
    float Bilinear(float x, float y) const {
        x -= 0.5f, y -= 0.5f;
        int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        float fx = x - x0, fy = y - y0;
        return (At(x0, y0) * (1 - fx) + At(x0 + 1, y0) * fx) * (1 - fy) + (At(x0, y0 + 1) * (1 - fx) + At(x0 + 1, y0 + 1) * fx) * fy;
    }
};

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// --- the game's font table: per glyph its box on the sheet, its character, its kerning pairs
struct Glyph {
    int x, y, w, h, code, kerns;
    uint32_t kern_at;
};
struct Font {
    int line = 0, spacing = 0;
    std::vector<Glyph> glyphs;
    std::map<int, int> index;               // character code to glyph
    std::map<std::pair<int, int>, int> kern;  // (character, next character) to pixels
};
Font ReadPcf(const fs::path& p) {
    std::vector<uint8_t> d = ReadFile(p);
    auto i32 = [&](size_t o) { int32_t v; std::memcpy(&v, &d[o], 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &d[o], 2); return v; };
    Font f;
    int n = i32(4);
    f.line = i32(8);
    f.spacing = i32(16);
    for (int i = 0; i < n; i++) {
        size_t o = 0x218 + size_t(i) * 16;
        Glyph g{u16(o), u16(o + 2), u16(o + 4), u16(o + 6), d[o + 8], d[o + 9], uint32_t(i32(o + 12))};
        f.index[g.code] = int(f.glyphs.size());
        f.glyphs.push_back(g);
    }
    for (const Glyph& g : f.glyphs)
        for (int k = 0; k < g.kerns && g.kern_at + size_t(k) * 4 + 4 <= d.size(); k++)
            f.kern[{g.code, u16(g.kern_at + k * 4)}] = int16_t(u16(g.kern_at + k * 4 + 2));
    return f;
}

// --- the game's sheet: DXT1/3/5 level 0
Gray ReadDds(const fs::path& p, uint32_t& fourcc_out, std::vector<uint8_t>& level0) {
    std::vector<uint8_t> d = ReadFile(p);
    uint32_t h, w, cc;
    std::memcpy(&h, &d[12], 4);
    std::memcpy(&w, &d[16], 4);
    std::memcpy(&cc, &d[84], 4);
    fourcc_out = cc;
    bool dxt1 = cc == 0x31545844;
    int block = dxt1 ? 8 : 16;
    bool dxt3 = cc == 0x33545844;
    level0.assign(d.begin() + 128, d.begin() + 128 + std::max(1u, w / 4) * std::max(1u, h / 4) * block);
    Gray g{int(w), int(h), std::vector<float>(size_t(w) * h), std::vector<uint32_t>(size_t(w) * h)};
    auto c565 = [](uint16_t c) {
        return std::array<int, 3>{((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
    };
    size_t o = 128;
    for (uint32_t by = 0; by < h; by += 4)
        for (uint32_t bx = 0; bx < w; bx += 4, o += block) {
            const uint8_t* b = &d[o];
            int alpha[16];
            if (dxt1) {
                std::fill(alpha, alpha + 16, 255);
            } else if (dxt3) {
                for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
            } else {
                int a0 = b[0], a1 = b[1], t[8] = {a0, a1};
                for (int i = 0; i < 6; i++) t[2 + i] = a0 > a1 ? ((6 - i) * a0 + (i + 1) * a1) / 7 : i < 4 ? ((4 - i) * a0 + (i + 1) * a1) / 5 : (i == 4 ? 0 : 255);
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = t[(bits >> (3 * i)) & 7];
            }
            const uint8_t* cb = b + block - 8;
            uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
            auto p0 = c565(c0), p1 = c565(c1);
            std::array<int, 3> cols[4] = {p0, p1, {}, {}};
            bool four = c0 > c1 || !dxt1;
            for (int k = 0; k < 3; k++) {
                cols[2][k] = four ? (2 * p0[k] + p1[k]) / 3 : (p0[k] + p1[k]) / 2;
                cols[3][k] = four ? (p0[k] + 2 * p1[k]) / 3 : 0;
            }
            uint32_t idx = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | cb[7] << 24);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx + i % 4, y = by + i / 4;
                if (x >= w || y >= h) continue;
                int ci = (idx >> (2 * i)) & 3;
                int a = dxt1 ? (!four && ci == 3 ? 0 : 255) : alpha[i];
                g.a[size_t(y) * w + x] = a / 255.0f;
                g.rgb[size_t(y) * w + x] = uint32_t(cols[ci][0] << 16 | cols[ci][1] << 8 | cols[ci][2]);
            }
        }
    return g;
}

// --- the port's texture name (gpu/texturedumper.cpp Key with gpu/shaders.cpp Hash)
uint64_t PortHash(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    auto mix = [](uint64_t a, uint64_t b) {
        unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
        return uint64_t(r) ^ uint64_t(r >> 64);
    };
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t seed = h ^ k0;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        std::memcpy(&x, b + i, 8);
        std::memcpy(&y, b + i + 8, 8);
        seed = mix(x ^ k1, y ^ seed);
    }
    if (size_t r = n - i) {
        uint64_t x = 0, y = 0;
        if (r >= 8) {
            std::memcpy(&x, b + i, 8);
            std::memcpy(&y, b + n - 8, 8);
        } else {
            std::memcpy(&x, b + i, r);
        }
        seed = mix(x ^ k1, y ^ seed ^ r);
    }
    return mix(seed ^ k0, uint64_t(n) ^ k1);
}

// --- DirectWrite
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;

struct Ink {
    int x0, x1, y0, y1;
    bool any() const { return x1 > x0; }
};
// text drawn white at em pixels, its coverage and ink box; the draw origin is fixed, so the baseline sits at the same
// row for every character of a family, weight, stretch and size
Gray Draw(const std::wstring& family, int weight, int stretch, float em, const std::wstring& text, Ink& ink, int n = 0) {
    if (!n) n = int(em * 3) + 32;
    IWICBitmap* bmp;
    g_wic->CreateBitmap(n, n, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt;
    g_d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    IDWriteTextFormat* tf;
    g_dw->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT(weight), DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH(stretch), em, L"en-us", &tf);
    ID2D1SolidColorBrush* white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->DrawText(text.c_str(), UINT32(text.size()), tf, D2D1::RectF(16, 8, float(n), float(n)), white);
    rt->EndDraw();
    Gray g{n, n, std::vector<float>(size_t(n) * n)};
    IWICBitmapLock* lock;
    WICRect r{0, 0, n, n};
    bmp->Lock(&r, WICBitmapLockRead, &lock);
    UINT size, stride;
    BYTE* px;
    lock->GetDataPointer(&size, &px);
    lock->GetStride(&stride);
    ink = {n, 0, n, 0};
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float a = px[size_t(y) * stride + x * 4 + 3] / 255.0f;
            g.a[size_t(y) * n + x] = a;
            if (a > 0.5f) ink = {std::min(ink.x0, x), std::max(ink.x1, x + 1), std::min(ink.y0, y), std::max(ink.y1, y + 1)};
        }
    lock->Release();
    white->Release();
    tf->Release();
    rt->Release();
    bmp->Release();
    return g;
}

bool SavePng(const fs::path& path, int w, int h, const std::vector<uint8_t>& bgra) {
    IWICStream* s;
    if (FAILED(g_wic->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE))) return false;
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
    return true;
}

wchar_t CharOf(int code) {
    char c = char(code);
    wchar_t w = 0;
    return MultiByteToWideChar(1252, MB_ERR_INVALID_CHARS, &c, 1, &w, 1) == 1 ? w : 0;
}

// --- one sheet: measure the game's, draw the family's
struct Style {
    int weight = 400, stretch = 5;
};
Style StyleOf(const std::string& name) {
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (n.rfind("hnbl", 0) == 0) return {900, 5};
    if (n.find("hev") != std::string::npos) return {800, 5};
    if (n.rfind("hel_60l", 0) == 0) return {300, 5};
    if (n.rfind("hel_20m", 0) == 0) return {500, 5};
    if (n.rfind("hec", 0) == 0) return {500, 3};
    return {400, 5};
}

struct Sheet {
    std::string name;
    Font font;
    Gray game;          // the game's sheet
    Gray big;           // rebuilt, scale times
    std::vector<uint8_t> level0;
    uint32_t fourcc;
    int kept = 0, drawn = 0;
};

// coverage past on: 0.5 for solid ink (heights, baselines), lower for the faint edge that still shows (spans)
Ink InkOf(const Gray& g, int x0, int y0, int w, int h, float on = 0.5f) {
    Ink k{x0 + w, x0, y0 + h, y0};
    for (int y = y0; y < y0 + h && y < g.h; y++)
        for (int x = x0; x < x0 + w && x < g.w; x++)
            if (g.a[size_t(y) * g.w + x] > on) k = {std::min(k.x0, x), std::max(k.x1, x + 1), std::min(k.y0, y), std::max(k.y1, y + 1)};
    return k;
}
constexpr float kVisible = 0.2f;

float Median(std::vector<float> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void Rebuild(Sheet& s, const std::wstring& family, int scale) {
    const Font& f = s.font;
    Style st = StyleOf(s.name);
    // the game's baseline (rows down a box) and cap height, from flat letters and digits
    std::vector<float> bottoms, caps;
    for (const Glyph& g : f.glyphs) {
        wchar_t c = CharOf(g.code);
        bool flat = std::wcschr(L"HEFILTNMKZ0123456789xzvwunmrs", c) && c;
        Ink k = InkOf(s.game, g.x, g.y, g.w, g.h);
        if (!flat || !k.any()) continue;
        bottoms.push_back(float(k.y1 - g.y));
        if (std::wcschr(L"HEFILTNMKZ0123456789", c)) caps.push_back(float(k.y1 - k.y0));
    }
    float base = Median(bottoms), cap = Median(caps);
    // the family's size: its capitals (or digits) the sheet's height, times scale
    Ink ref;
    Draw(family, st.weight, st.stretch, 100, L"H0", ref);
    Ink h_ink;
    Draw(family, st.weight, st.stretch, 100, L"H", h_ink);
    float em = cap > 0 && h_ink.any() ? 100.0f * cap * scale / float(h_ink.y1 - h_ink.y0) : float(f.line * scale) * 0.8f;
    Ink base_ink;
    Draw(family, st.weight, st.stretch, em, L"H", base_ink);
    int base_row = base_ink.y1;  // the baseline in a drawn bitmap at this size
    // A clear sheet, and a glyph that is not redrawn (an icon) the game's own, enlarged inside its box only, its
    // samples kept within the box: the whole sheet enlarged smeared each letter's edge past its box, and the game,
    // sampling a box's edge, showed those smears as thin lines beside the new letters
    s.big = Gray{s.game.w * scale, s.game.h * scale, std::vector<float>(size_t(s.game.w) * scale * s.game.h * scale),
                 std::vector<uint32_t>(size_t(s.game.w) * scale * s.game.h * scale, 0xFFFFFF)};
    auto keep = [&](const Glyph& g) {
        s.kept++;
        for (int y = g.y * scale; y < (g.y + g.h) * scale && y < s.big.h; y++)
            for (int x = g.x * scale; x < (g.x + g.w) * scale && x < s.big.w; x++) {
                float gx = std::clamp((x + 0.5f) / scale, g.x + 0.5f, g.x + g.w - 0.5f);
                float gy = std::clamp((y + 0.5f) / scale, g.y + 0.5f, g.y + g.h - 0.5f);
                s.big.a[size_t(y) * s.big.w + x] = std::clamp(s.game.Bilinear(gx, gy), 0.0f, 1.0f);
                s.big.rgb[size_t(y) * s.big.w + x] = s.game.rgb[size_t(int(gy)) * s.game.w + int(gx)];
            }
    };
    for (const Glyph& g : f.glyphs) {
        wchar_t c = CharOf(g.code);
        Ink old_ink = InkOf(s.game, g.x, g.y, g.w, g.h);
        if (!c || !old_ink.any()) {
            keep(g);
            continue;
        }
        Ink k;
        Gray d = Draw(family, st.weight, st.stretch, em, std::wstring(1, c), k);
        if (!k.any()) {
            keep(g);
            continue;
        }
        // a symbol the family draws far off the game's size is an icon of the game's own (controller buttons): kept
        float hr = float(k.y1 - k.y0) / scale / float(old_ink.y1 - old_ink.y0);
        float wr = float(k.x1 - k.x0) / scale / float(old_ink.x1 - old_ink.x0);
        if (!iswalnum(c) && (hr > 1.6f || hr < 0.55f || wr > 1.8f || wr < 0.45f)) {
            keep(g);
            continue;
        }
        s.drawn++;
        // clear the box, then the glyph centred on the game's ink, on the game's baseline
        int bx0 = g.x * scale, by0 = g.y * scale, bx1 = (g.x + g.w) * scale, by1 = (g.y + g.h) * scale;
        for (int y = by0; y < by1 && y < s.big.h; y++)
            for (int x = bx0; x < bx1 && x < s.big.w; x++) {
                s.big.a[size_t(y) * s.big.w + x] = 0;
                s.big.rgb[size_t(y) * s.big.w + x] = 0xFFFFFF;
            }
        // the glyph keeps clear of the box's edge: the game samples each box with bilinear filtering, which at the edge
        // reaches across into the next box, and a glyph out to its edge showed as a thin line beside its neighbour
        const int kMargin = std::max(1, scale / 2);
        bx0 += kMargin, by0 += kMargin, bx1 -= kMargin, by1 -= kMargin;
        // across, the glyph spans the game's letter exactly (its width matched within 20%), so the game's spacing and
        // kerning, made for its own letters, fit; down, it sits on the game's baseline at the family's own height
        // spans by what shows, faint edges included: the game's soft letters reach a pixel past their solid ink, and
        // matched to the solid part alone the crisp new ones left gaps beside them ("ST AGE")
        Ink old_span = InkOf(s.game, g.x, g.y, g.w, g.h, kVisible), new_span = InkOf(d, 0, 0, d.w, d.h, kVisible);
        old_ink.x0 = old_span.x0, old_ink.x1 = old_span.x1;
        k.x0 = new_span.x0, k.x1 = new_span.x1;
        float new_w = float(k.x1 - k.x0), old_w = float(old_ink.x1 - old_ink.x0) * scale;
        float sx = std::clamp(old_w / new_w, 0.8f, 1.2f);
        // FONTGEN_DEBUG=name: each glyph of that sheet, the game's ink and the drawn one, in sheet pixels
        if (const char* dbg = std::getenv("FONTGEN_DEBUG"); dbg && s.name == dbg && c < 128)
            std::printf("  %c box %d+%d ink %d..%d (w %d) drawn w %.1f h %.1f vs %d, stretch %.2f\n", char(c), g.x, g.w,
                        old_ink.x0 - g.x, old_ink.x1 - g.x, old_ink.x1 - old_ink.x0, new_w / scale,
                        float(k.y1 - k.y0) / scale, old_ink.y1 - old_ink.y0, sx);
        float left = (old_ink.x0 + old_ink.x1) * 0.5f * scale - new_w * sx * 0.5f;
        int dy = int(std::lround((g.y + base) * scale - base_row));
        for (int ty = std::max(by0, k.y0 - 1 + dy); ty < std::min(by1, k.y1 + 1 + dy); ty++) {
            int y = ty - dy;
            if (y < 0 || y >= d.h) continue;
            for (int tx = std::max(bx0, int(std::floor(left)) - 1); tx < std::min(bx1, int(std::ceil(left + new_w * sx)) + 1); tx++) {
                // the drawn row resampled across: target pixel centre back into the drawn glyph
                float sxp = k.x0 + ((tx + 0.5f) - left) / sx - 0.5f;
                int x0 = int(std::floor(sxp));
                float f = sxp - x0;
                auto at = [&](int x) { return x < 0 || x >= d.w ? 0.0f : d.a[size_t(y) * d.w + x]; };
                float a = at(x0) * (1 - f) + at(x0 + 1) * f;
                float& t = s.big.a[size_t(ty) * s.big.w + tx];
                t = std::max(t, a);
            }
        }
    }
}

// --- preview: game text laid out as the game's table spaces it, drawn from a sheet at 3x
void Text(const Sheet& s, const Gray& sheet, int sheet_scale, const std::string& text, std::vector<float>& img, int iw,
          int ih, int ox, int oy) {
    constexpr int kScale = 3;
    int pen = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int code = uint8_t(text[i]);
        auto it = s.font.index.find(code);
        if (it == s.font.index.end()) {
            pen += std::max(3, s.font.line / 4);
            continue;
        }
        const Glyph& g = s.font.glyphs[size_t(it->second)];
        for (int y = 0; y < g.h * kScale; y++)
            for (int x = 0; x < g.w * kScale; x++) {
                int px = ox + (pen * kScale) + x, py = oy + y;
                if (px < 0 || py < 0 || px >= iw || py >= ih) continue;
                float u = (g.x + (x + 0.5f) / kScale) * sheet_scale, v = (g.y + (y + 0.5f) / kScale) * sheet_scale;
                float a = sheet_scale == 1 ? sheet.Bilinear(u, v) : sheet.Bilinear(u, v);
                float& t = img[size_t(py) * iw + px];
                t = std::max(t, std::clamp(a, 0.0f, 1.0f));
            }
        pen += g.w + s.font.spacing;
        if (i + 1 < text.size()) {
            auto k = s.font.kern.find({code, uint8_t(text[i + 1])});
            if (k != s.font.kern.end()) pen += k->second;
        }
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::printf("fontgen fonts_dir out_dir scale family [family...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    fs::path src = argv[1], out = argv[2];
    int scale = _wtoi(argv[3]);
    std::vector<std::wstring> families(argv + 4, argv + argc);
    fs::create_directories(out);

    std::map<std::string, Sheet> sheets;
    for (auto& e : fs::directory_iterator(src)) {
        if (e.path().extension() != ".pcf") continue;
        std::string name = e.path().stem().string();
        if (name == "3d_lcd") continue;  // the in-car display's own segment digits
        Sheet s;
        s.name = name;
        s.font = ReadPcf(e.path());
        s.game = ReadDds(fs::path(e.path()).replace_extension(".dds"), s.fourcc, s.level0);
        sheets[name] = std::move(s);
    }

    // preview lines: the font and the text, as the game shows them
    const std::pair<const char*, const char*> lines[] = {
        {"HNBL28", "LOADING."},
        {"hel_12pt", "STAGE 1 - SPAIN - SELARDU - MORNING - CLEAR"},
        {"HelNu_16", "Best stage time: 03:43.64"},
        {"hec12med", "P Stafs\xe9n (bel)  Citro\xebn Xsara"},
        {"HelNu_14", "RACE - CAR SELECT - STAGE SELECT - OPTIONS - EXIT"},
        {"HelNu_10", "Race. Press A to randomize car, Y to randomize stage."},
        {"clock_30", "03:43.64"},
        {"40_hev_1", "STAGES."},
    };
    constexpr int kW = 1500, kLabel = 26;
    std::vector<std::vector<float>> strips;
    std::vector<std::wstring> labels;
    auto strip = [&](std::map<std::string, Gray>* rebuilt) {
        int h = 0;
        for (auto& [font, text] : lines) h += (sheets.count(font) ? sheets[font].font.line : 20) * 3 + 12;
        std::vector<float> img(size_t(kW) * h);
        int y = 0;
        for (auto& [font, text] : lines) {
            if (!sheets.count(font)) continue;
            Sheet& s = sheets[font];
            if (rebuilt) Text(s, (*rebuilt)[font], scale, text, img, kW, h, 12, y);
            else Text(s, s.game, 1, text, img, kW, h, 12, y);
            y += s.font.line * 3 + 12;
        }
        return std::make_pair(h, img);
    };
    std::vector<std::pair<int, std::vector<float>>> parts;
    parts.push_back(strip(nullptr));
    labels.push_back(L"now (the game's sheets, 3x)");
    for (size_t fi = 0; fi < families.size(); fi++) {
        std::map<std::string, Gray> rebuilt;
        for (auto& [name, s] : sheets) {
            Rebuild(s, families[fi], scale);
            rebuilt[name] = s.big;
            if (fi == 0) {
                // the first family's sheets, under the names textures\replacement\ expects
                uint32_t code = s.fourcc == 0x31545844 ? 0x0C : s.fourcc == 0x33545844 ? 0x0E : 0x0F;
                uint32_t shape[3] = {code, uint32_t(s.game.w), uint32_t(s.game.h)};
                uint64_t key = PortHash(s.level0.data(), s.level0.size(), PortHash(shape, sizeof shape));
                char fname[96];
                std::snprintf(fname, sizeof fname, "%dx%d_%016llx_%.4s.png", s.game.w, s.game.h,
                              static_cast<unsigned long long>(key ? key : 1), reinterpret_cast<const char*>(&s.fourcc));
                std::vector<uint8_t> bgra(size_t(s.big.w) * s.big.h * 4);
                for (size_t i = 0; i < s.big.a.size(); i++) {
                    uint32_t c = s.big.rgb[i];
                    bgra[i * 4 + 0] = uint8_t(c & 255);
                    bgra[i * 4 + 1] = uint8_t(c >> 8 & 255);
                    bgra[i * 4 + 2] = uint8_t(c >> 16 & 255);
                    bgra[i * 4 + 3] = uint8_t(std::lround(s.big.a[i] * 255));
                }
                SavePng(out / fname, s.big.w, s.big.h, bgra);
                std::printf("%-10s -> %s  (%d redrawn, %d kept)\n", name.c_str(), fname, s.drawn, s.kept);
            }
        }
        parts.push_back(strip(&rebuilt));
        labels.push_back(families[fi]);
    }
    // the preview: each strip under its name, white text over dark grey
    int total = 0;
    for (auto& p : parts) total += p.first + kLabel;
    std::vector<uint8_t> bgra(size_t(kW) * total * 4, 255);
    int oy = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        for (int y = 0; y < kLabel; y++)
            for (int x = 0; x < kW; x++) {
                uint8_t* p = &bgra[(size_t(oy + y) * kW + x) * 4];
                p[0] = p[1] = p[2] = 25;
            }
        Ink ink;
        Gray label = Draw(L"Segoe UI", 600, 5, 16, labels[i], ink, 600);
        for (int y = 0; y < kLabel - 4; y++)
            for (int x = 0; x < 600; x++) {
                float a = label.a[size_t(y) * label.w + x];
                uint8_t* p = &bgra[(size_t(oy + y + 2) * kW + x) * 4];
                p[0] = uint8_t(25 + a * (60 - 25));
                p[1] = uint8_t(25 + a * (190 - 25));
                p[2] = uint8_t(25 + a * (255 - 25));
            }
        oy += kLabel;
        for (int y = 0; y < parts[i].first; y++)
            for (int x = 0; x < kW; x++) {
                float a = parts[i].second[size_t(y) * kW + x];
                uint8_t v = uint8_t(40 + a * 215);
                uint8_t* p = &bgra[(size_t(oy + y) * kW + x) * 4];
                p[0] = p[1] = p[2] = v;
            }
        oy += parts[i].first;
    }
    SavePng(out / "preview.png", kW, total, bgra);
    std::printf("preview: %s\n", (out / "preview.png").string().c_str());
}
