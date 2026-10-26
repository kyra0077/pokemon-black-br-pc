#include "ControlsMenu.h"

#include <algorithm>
#include <cstdlib>

namespace
{

// Display order: face buttons, shoulders, Start/Select, directions, then hotkeys.
const Action kRowOrder[kNumActions] = {
    Action::A, Action::B, Action::X, Action::Y, Action::L, Action::R, Action::Start, Action::Select,
    Action::Up, Action::Down, Action::Left, Action::Right,
    Action::Overlay, Action::FastForward, Action::Fullscreen, Action::Minimap,
    Action::SaveState, Action::LoadState, Action::NextSlot, Action::SavesMenu,
};

constexpr int kAxisCapture = 20000;
constexpr int kAxisRelease = 8000;

const SDL_Color kText{235, 235, 240, 255};
const SDL_Color kDim{150, 155, 165, 255};
const SDL_Color kAccent{120, 200, 255, 255};
const SDL_Color kWarn{255, 210, 120, 255};

bool gAxisBlocked[SDL_CONTROLLER_AXIS_MAX] = {};

} // namespace

Action ControlsMenu::RowAction(int row) const { return kRowOrder[row]; }

void ControlsMenu::Open()
{
    open = true;
    capturing = false;
    selected = 0;
    hover = -1;
    status.clear();
}

void ControlsMenu::Close()
{
    open = false;
    capturing = false;
    cfg.Save(path);
}

void ControlsMenu::SetStatus(const std::string& msg)
{
    status = msg;
    statusUntil = SDL_GetTicks64() + 4000;
}

void ControlsMenu::Activate(int row)
{
    selected = row;
    if (row == kRowRestore)
    {
        cfg.SetDefaults();
        SetStatus("Controles restaurados para o padrão.");
    }
    else if (row == kRowBack)
    {
        Close();
    }
    else
    {
        capturing = true;
        for (bool& b : gAxisBlocked) b = true; // ignore sticks/triggers until they pass through rest
    }
}

void ControlsMenu::ClearRow(int row)
{
    if (row >= kNumActions) return;
    cfg.Clear(RowAction(row));
    SetStatus(std::string("Nada ligado a \"") + ActionLabel(RowAction(row)) + "\" agora.");
}

void ControlsMenu::Capture(const Binding& b)
{
    Action target = RowAction(selected);
    std::string taken;
    for (int a = 0; a < kNumActions; a++)
    {
        if ((Action)a == target) continue;
        if (std::find(cfg.binds[a].begin(), cfg.binds[a].end(), b) != cfg.binds[a].end())
            taken = ActionLabel((Action)a);
    }
    cfg.Add(target, b);
    capturing = false;
    if (!taken.empty())
        SetStatus(BindingName(b) + " saiu de \"" + taken + "\" e agora faz \"" + ActionLabel(target) + "\".");
    else
        SetStatus(BindingName(b) + " agora faz \"" + ActionLabel(target) + "\".");
}

void ControlsMenu::ToRenderer(SDL_Window* window, SDL_Renderer* renderer, int& x, int& y) const
{
    int ww, wh, rw, rh;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetRendererOutputSize(renderer, &rw, &rh);
    x = x * rw / std::max(ww, 1);
    y = y * rh / std::max(wh, 1);
}

int ControlsMenu::RowAt(int x, int y) const
{
    SDL_Point p{x, y};
    for (int i = 0; i < kNumRows; i++)
        if (SDL_PointInRect(&p, &rowRects[i])) return i;
    return -1;
}

bool ControlsMenu::HandleEvent(const SDL_Event& e, SDL_Window* window, SDL_Renderer* renderer)
{
    if (!open) return false;

    if (capturing)
    {
        switch (e.type)
        {
        case SDL_KEYDOWN:
            if (e.key.repeat) return true;
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE || e.key.keysym.scancode == SDL_SCANCODE_F1)
            {
                capturing = false;
                SetStatus("Cancelado.");
            }
            else
            {
                Capture({Binding::Key, e.key.keysym.scancode, 0});
            }
            return true;
        case SDL_CONTROLLERBUTTONDOWN:
            Capture({Binding::Button, e.cbutton.button, 0});
            return true;
        case SDL_CONTROLLERAXISMOTION:
        {
            int axis = e.caxis.axis;
            if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return true;
            if (std::abs(e.caxis.value) < kAxisRelease) gAxisBlocked[axis] = false;
            else if (!gAxisBlocked[axis] && std::abs(e.caxis.value) > kAxisCapture)
                Capture({Binding::Axis, axis, e.caxis.value > 0 ? 1 : -1});
            return true;
        }
        case SDL_MOUSEBUTTONDOWN:
            capturing = false;
            SetStatus("Cancelado.");
            return true;
        default:
            return e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT;
        }
    }

    switch (e.type)
    {
    case SDL_KEYDOWN:
        switch (e.key.keysym.scancode)
        {
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_F1:
            if (!e.key.repeat) Close();
            break;
        case SDL_SCANCODE_UP: selected = (selected + kNumRows - 1) % kNumRows; break;
        case SDL_SCANCODE_DOWN: selected = (selected + 1) % kNumRows; break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            if (!e.key.repeat) Activate(selected);
            break;
        case SDL_SCANCODE_DELETE:
        case SDL_SCANCODE_BACKSPACE:
            ClearRow(selected);
            break;
        default: break;
        }
        return true;

    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button)
        {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: selected = (selected + kNumRows - 1) % kNumRows; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: selected = (selected + 1) % kNumRows; break;
        case SDL_CONTROLLER_BUTTON_A: Activate(selected); break;
        case SDL_CONTROLLER_BUTTON_X: ClearRow(selected); break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_GUIDE: Close(); break;
        default: break;
        }
        return true;

    case SDL_MOUSEMOTION:
    {
        int x = e.motion.x, y = e.motion.y;
        ToRenderer(window, renderer, x, y);
        hover = RowAt(x, y);
        return true;
    }

    case SDL_MOUSEBUTTONDOWN:
    {
        int x = e.button.x, y = e.button.y;
        ToRenderer(window, renderer, x, y);
        int row = RowAt(x, y);
        if (row < 0) return true;
        if (e.button.button == SDL_BUTTON_LEFT) Activate(row);
        else if (e.button.button == SDL_BUTTON_RIGHT) { selected = row; ClearRow(row); }
        return true;
    }

    case SDL_MOUSEWHEEL:
        selected = std::clamp(selected - e.wheel.y, 0, kNumRows - 1);
        return true;

    case SDL_KEYUP:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
    case SDL_MOUSEBUTTONUP:
        return true;

    default:
        return false;
    }
}

void ControlsMenu::Render(SDL_Renderer* r, TextRenderer& text)
{
    if (!open) return;
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 190);
    SDL_Rect all{0, 0, w, h};
    SDL_RenderFillRect(r, &all);

    // Layout scales with the window height.
    // All rows plus title, spacing and footer must fit the window height.
    int rowH = std::max(14, std::min(h / 26, h / (kNumRows + 6)));
    int textH = rowH * 58 / 100;
    int titleH = rowH * 95 / 100;
    int panelW = std::min(w - 2 * rowH / 2, rowH * 30);
    int panelH = rowH * (kNumRows + 1) + titleH * 2 + rowH * 3;
    int px = (w - panelW) / 2;
    int py = std::max(0, (h - panelH) / 2);

    SDL_Rect panel{px, py, panelW, panelH};
    SDL_SetRenderDrawColor(r, 24, 26, 32, 245);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, 70, 75, 90, 255);
    SDL_RenderDrawRect(r, &panel);

    int pad = rowH / 2;
    int y = py + pad;
    text.Draw("Controles", px + pad, y, titleH, kText, 0, true);
    text.Draw("F1 ou Esc fecha e salva", px + panelW - pad, y + (titleH - textH) / 2, textH, kDim, 2);
    y += titleH + pad;

    int labelW = panelW * 34 / 100;
    for (int row = 0; row < kNumRows; row++)
    {
        if (row == kRowRestore) y += rowH / 2;
        SDL_Rect rr{px + pad / 2, y, panelW - pad, rowH};
        rowRects[row] = rr;

        bool sel = row == selected;
        if (sel || row == hover)
        {
            SDL_SetRenderDrawColor(r, sel ? 45 : 35, sel ? 70 : 40, sel ? 110 : 50, 255);
            SDL_RenderFillRect(r, &rr);
        }
        int ty = y + (rowH - textH) / 2 - textH / 10;

        if (row < kNumActions)
        {
            Action a = RowAction(row);
            text.Draw(ActionLabel(a), rr.x + pad / 2, ty, textH, kText, 0, true);
            int bx = rr.x + labelW;
            if (sel && capturing)
            {
                text.Draw("Pressione uma tecla ou botão…  (Esc cancela)", bx, ty, textH, kWarn);
            }
            else if (cfg.binds[(int)a].empty())
            {
                text.Draw("— nada —", bx, ty, textH, kDim);
            }
            else
            {
                int maxX = rr.x + rr.w - pad / 2;
                for (size_t i = 0; i < cfg.binds[(int)a].size(); i++)
                {
                    std::string name = BindingName(cfg.binds[(int)a][i]);
                    int cw = text.Measure(name, textH) + pad;
                    if (bx + cw > maxX)
                    {
                        text.Draw("…", bx, ty, textH, kDim);
                        break;
                    }
                    SDL_Rect chip{bx, y + rowH / 8, cw - pad / 3, rowH - rowH / 4};
                    SDL_SetRenderDrawColor(r, 55, 60, 75, 255);
                    SDL_RenderFillRect(r, &chip);
                    text.Draw(name, bx + pad / 3, ty, textH, kText);
                    bx += cw;
                }
            }
        }
        else
        {
            const char* label = row == kRowRestore ? "Restaurar padrões" : "Salvar e voltar ao jogo";
            text.Draw(label, rr.x + rr.w / 2, ty, textH, sel ? kAccent : kText, 1, true);
        }
        y += rowH;
    }

    y += pad / 2;
    bool showStatus = !status.empty() && SDL_GetTicks64() < statusUntil;
    if (showStatus)
        text.Draw(status, px + panelW / 2, y, textH, kAccent, 1);
    else
        text.Draw("Enter / clique / Ⓐ: adicionar   ·   Del / botão direito / Ⓧ: limpar   ·   ↑↓: escolher",
                  px + panelW / 2, y, textH * 85 / 100, kDim, 1);
}
