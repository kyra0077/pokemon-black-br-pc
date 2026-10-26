#pragma once

#include <map>
#include <string>

#include <SDL.h>

// Draws UTF-8 text with the Windows system font (GDI), cached as SDL textures.
// White glyphs on transparent; tint with the color argument.
class TextRenderer
{
public:
    explicit TextRenderer(SDL_Renderer* r) : renderer(r) {}
    ~TextRenderer() { Clear(); }

    // Returns the drawn width. align: 0 left, 1 center, 2 right (relative to x).
    int Draw(const std::string& text, int x, int y, int height, SDL_Color color, int align = 0, bool bold = false);
    int Measure(const std::string& text, int height, bool bold = false);
    void Clear();

private:
    struct Entry { SDL_Texture* tex; int w, h; };
    const Entry* Get(const std::string& text, int height, bool bold);

    SDL_Renderer* renderer;
    std::map<std::string, Entry> cache;
};
