#include "Input.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{

const char* const kIniKeys[kNumActions] = {
    "A", "B", "Select", "Start", "Direita", "Esquerda", "Cima", "Baixo", "R", "L", "X", "Y",
    "TelaDeToque", "AvancoRapido", "TelaCheia", "Minitela",
    "SalvarEstado", "CarregarEstado", "TrocarEspaco", "MenuSaves",
};

const char* const kLabels[kNumActions] = {
    "A", "B", "Select", "Start", "Direita", "Esquerda", "Cima", "Baixo", "R", "L", "X", "Y",
    "Mostrar tela de toque", "Avanço rápido (segurar)", "Tela cheia", "Mostrar/ocultar minitela",
    "Salvar estado rápido", "Carregar estado rápido", "Trocar espaço do estado", "Menu de saves",
};

const char* const kMinimapIniKey = "MinitelaVisivel";

Binding K(SDL_Scancode s) { return {Binding::Key, s, 0}; }
Binding P(SDL_GameControllerButton b) { return {Binding::Button, b, 0}; }
Binding X(SDL_GameControllerAxis a, int dir) { return {Binding::Axis, a, dir}; }

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Serialize(const Binding& b)
{
    switch (b.type)
    {
    case Binding::Key: return std::string("key:") + SDL_GetScancodeName((SDL_Scancode)b.code);
    case Binding::Button: return std::string("pad:") + SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b.code);
    case Binding::Axis:
        return std::string("axis:") + SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)b.code) + (b.dir > 0 ? "+" : "-");
    }
    return "";
}

bool Parse(const std::string& text, Binding& out)
{
    std::string s = Trim(text);
    if (s.rfind("key:", 0) == 0)
    {
        SDL_Scancode sc = SDL_GetScancodeFromName(s.c_str() + 4);
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        out = K(sc);
        return true;
    }
    if (s.rfind("pad:", 0) == 0)
    {
        auto btn = SDL_GameControllerGetButtonFromString(s.c_str() + 4);
        if (btn == SDL_CONTROLLER_BUTTON_INVALID) return false;
        out = P(btn);
        return true;
    }
    if (s.rfind("axis:", 0) == 0 && s.size() > 6)
    {
        char sign = s.back();
        if (sign != '+' && sign != '-') return false;
        auto axis = SDL_GameControllerGetAxisFromString(s.substr(5, s.size() - 6).c_str());
        if (axis == SDL_CONTROLLER_AXIS_INVALID) return false;
        out = X(axis, sign == '+' ? 1 : -1);
        return true;
    }
    return false;
}

} // namespace

void InputConfig::SetDefaults()
{
    for (auto& b : binds) b.clear();
    auto set = [&](Action a, std::initializer_list<Binding> list) { binds[(int)a] = list; };

    // Controller buttons follow the DS layout by position: the east button is A, south is B...
    set(Action::A, {K(SDL_SCANCODE_X), P(SDL_CONTROLLER_BUTTON_B)});
    set(Action::B, {K(SDL_SCANCODE_Z), P(SDL_CONTROLLER_BUTTON_A)});
    set(Action::X, {K(SDL_SCANCODE_S), P(SDL_CONTROLLER_BUTTON_Y)});
    set(Action::Y, {K(SDL_SCANCODE_A), P(SDL_CONTROLLER_BUTTON_X)});
    set(Action::L, {K(SDL_SCANCODE_Q), P(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)});
    set(Action::R, {K(SDL_SCANCODE_W), P(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)});
    set(Action::Start, {K(SDL_SCANCODE_RETURN), P(SDL_CONTROLLER_BUTTON_RIGHTSTICK)});
    set(Action::Select, {K(SDL_SCANCODE_RSHIFT), K(SDL_SCANCODE_BACKSPACE), P(SDL_CONTROLLER_BUTTON_LEFTSTICK)});
    set(Action::Up, {K(SDL_SCANCODE_UP), P(SDL_CONTROLLER_BUTTON_DPAD_UP)});
    set(Action::Down, {K(SDL_SCANCODE_DOWN), P(SDL_CONTROLLER_BUTTON_DPAD_DOWN)});
    set(Action::Left, {K(SDL_SCANCODE_LEFT), P(SDL_CONTROLLER_BUTTON_DPAD_LEFT)});
    set(Action::Right, {K(SDL_SCANCODE_RIGHT), P(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)});
    set(Action::Overlay, {K(SDL_SCANCODE_TAB), P(SDL_CONTROLLER_BUTTON_START), P(SDL_CONTROLLER_BUTTON_BACK),
                          X(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1), X(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)});
    set(Action::FastForward, {K(SDL_SCANCODE_SPACE)});
    set(Action::Fullscreen, {K(SDL_SCANCODE_F11)});
    set(Action::Minimap, {K(SDL_SCANCODE_M)});
    set(Action::SaveState, {K(SDL_SCANCODE_F5)});
    set(Action::LoadState, {K(SDL_SCANCODE_F8)});
    set(Action::NextSlot, {K(SDL_SCANCODE_F6)});
    set(Action::SavesMenu, {K(SDL_SCANCODE_F2)});
}

bool InputConfig::Load(const std::string& path)
{
    FILE* f = _wfopen(Widen(path).c_str(), L"r");
    if (!f) return false;

    SetDefaults();
    bool seen[kNumActions] = {};
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(s.substr(0, eq));
        std::string value = s.substr(eq + 1);

        if (_stricmp(key.c_str(), kMinimapIniKey) == 0)
        {
            showMinimap = Trim(value) != "0";
            continue;
        }

        for (int a = 0; a < kNumActions; a++)
        {
            if (_stricmp(key.c_str(), kIniKeys[a]) != 0) continue;
            if (!seen[a]) binds[a].clear(); // the file overrides the defaults for listed actions
            seen[a] = true;
            size_t pos = 0;
            while (pos <= value.size())
            {
                size_t comma = value.find(',', pos);
                if (comma == std::string::npos) comma = value.size();
                Binding b;
                if (Parse(value.substr(pos, comma - pos), b) && binds[a].size() < kMaxBindingsPerAction &&
                    std::find(binds[a].begin(), binds[a].end(), b) == binds[a].end())
                    binds[a].push_back(b);
                pos = comma + 1;
            }
        }
    }
    fclose(f);
    return true;
}

bool InputConfig::Save(const std::string& path) const
{
    std::wstring final = Widen(path);
    std::wstring tmp = final + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w");
    if (!f) return false;
    fputs("; Mapeamento de controles. Edite pelo jogo (F1) ou aqui.\n", f);
    fputs("; Formatos: key:<tecla>, pad:<botao>, axis:<eixo>+ ou axis:<eixo>-, separados por virgula.\n", f);
    fputs("[Controles]\n", f);
    for (int a = 0; a < kNumActions; a++)
    {
        std::string line = std::string(kIniKeys[a]) + " = ";
        for (size_t i = 0; i < binds[a].size(); i++)
            line += (i ? ", " : "") + Serialize(binds[a][i]);
        fprintf(f, "%s\n", line.c_str());
    }
    fputs("\n[Opcoes]\n", f);
    fprintf(f, "%s = %d\n", kMinimapIniKey, showMinimap ? 1 : 0);
    bool ok = fflush(f) == 0;
    fclose(f);
    return ok && MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void InputConfig::Add(Action action, const Binding& b)
{
    for (auto& list : binds)
        list.erase(std::remove(list.begin(), list.end(), b), list.end());
    auto& mine = binds[(int)action];
    if (mine.size() >= kMaxBindingsPerAction) mine.erase(mine.begin());
    mine.push_back(b);
}

bool InputConfig::Held(Action action, const Uint8* keyboard, SDL_GameController* pad, bool padOnly) const
{
    for (const Binding& b : binds[(int)action])
    {
        switch (b.type)
        {
        case Binding::Key:
            if (!padOnly && keyboard[b.code]) return true;
            break;
        case Binding::Button:
            if (pad && SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b.code)) return true;
            break;
        case Binding::Axis:
            if (pad && SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)b.code) * b.dir > kAxisPressThreshold)
                return true;
            break;
        }
    }
    return false;
}

const char* ActionLabel(Action action) { return kLabels[(int)action]; }

std::string BindingName(const Binding& b)
{
    if (b.type == Binding::Key)
    {
        switch (b.code)
        {
        case SDL_SCANCODE_UP: return "Seta ↑";
        case SDL_SCANCODE_DOWN: return "Seta ↓";
        case SDL_SCANCODE_LEFT: return "Seta ←";
        case SDL_SCANCODE_RIGHT: return "Seta →";
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_SPACE: return "Espaço";
        case SDL_SCANCODE_RSHIFT: return "Shift dir.";
        case SDL_SCANCODE_LSHIFT: return "Shift esq.";
        case SDL_SCANCODE_RCTRL: return "Ctrl dir.";
        case SDL_SCANCODE_LCTRL: return "Ctrl esq.";
        case SDL_SCANCODE_BACKSPACE: return "Backspace";
        default: break;
        }
        const char* name = SDL_GetScancodeName((SDL_Scancode)b.code);
        return name && *name ? name : "Tecla ?";
    }
    if (b.type == Binding::Button)
    {
        switch (b.code)
        {
        case SDL_CONTROLLER_BUTTON_A: return "Controle: baixo (A/✕)";
        case SDL_CONTROLLER_BUTTON_B: return "Controle: direita (B/○)";
        case SDL_CONTROLLER_BUTTON_X: return "Controle: esquerda (X/□)";
        case SDL_CONTROLLER_BUTTON_Y: return "Controle: cima (Y/△)";
        case SDL_CONTROLLER_BUTTON_BACK: return "Controle: Select/Back";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Controle: Guide";
        case SDL_CONTROLLER_BUTTON_START: return "Controle: Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Controle: L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Controle: R3";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Controle: LB/L1";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Controle: RB/R1";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Controle: D-pad ↑";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Controle: D-pad ↓";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Controle: D-pad ←";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Controle: D-pad →";
        default: return std::string("Controle: ") + SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b.code);
        }
    }
    switch (b.code)
    {
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Controle: LT/L2";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Controle: RT/R2";
    case SDL_CONTROLLER_AXIS_LEFTX: return b.dir > 0 ? "Controle: analógico esq. →" : "Controle: analógico esq. ←";
    case SDL_CONTROLLER_AXIS_LEFTY: return b.dir > 0 ? "Controle: analógico esq. ↓" : "Controle: analógico esq. ↑";
    case SDL_CONTROLLER_AXIS_RIGHTX: return b.dir > 0 ? "Controle: analógico dir. →" : "Controle: analógico dir. ←";
    case SDL_CONTROLLER_AXIS_RIGHTY: return b.dir > 0 ? "Controle: analógico dir. ↓" : "Controle: analógico dir. ↑";
    default: return "Controle: eixo";
    }
}
