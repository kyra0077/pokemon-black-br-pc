#include "TextRenderer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{

constexpr size_t kMaxCachedStrings = 400;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

} // namespace

const TextRenderer::Entry* TextRenderer::Get(const std::string& text, int height, bool bold)
{
    if (text.empty() || height <= 0) return nullptr;
    std::string key = std::to_string(height) + (bold ? "b|" : "|") + text;
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;
    if (cache.size() >= kMaxCachedStrings) Clear();

    std::wstring wide = Widen(text);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);

    SIZE size{};
    GetTextExtentPoint32W(dc, wide.c_str(), (int)wide.size(), &size);
    int w = std::max<int>(size.cx, 1) + 2, h = std::max<int>(size.cy, 1);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 1, 0, wide.c_str(), (int)wide.size());
    GdiFlush();

    // GDI renders grayscale coverage into RGB; turn it into white with alpha.
    std::vector<Uint32> pixels((size_t)w * h);
    const Uint32* src = (const Uint32*)bits;
    for (size_t i = 0; i < pixels.size(); i++)
    {
        Uint32 g = (src[i] >> 8) & 0xFF;
        pixels[i] = (g << 24) | 0x00FFFFFF;
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return nullptr;
    SDL_UpdateTexture(tex, nullptr, pixels.data(), w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    return &(cache[key] = Entry{tex, w, h});
}

int TextRenderer::Draw(const std::string& text, int x, int y, int height, SDL_Color color, int align, bool bold)
{
    const Entry* e = Get(text, height, bold);
    if (!e) return 0;
    if (align == 1) x -= e->w / 2;
    else if (align == 2) x -= e->w;
    SDL_SetTextureColorMod(e->tex, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(e->tex, color.a);
    SDL_Rect dst{x, y, e->w, e->h};
    SDL_RenderCopy(renderer, e->tex, nullptr, &dst);
    return e->w;
}

int TextRenderer::Measure(const std::string& text, int height, bool bold)
{
    const Entry* e = Get(text, height, bold);
    return e ? e->w : 0;
}

void TextRenderer::Clear()
{
    for (auto& kv : cache) SDL_DestroyTexture(kv.second.tex);
    cache.clear();
}
