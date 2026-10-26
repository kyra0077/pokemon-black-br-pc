#include "SavesMenu.h"

#include <algorithm>
#include <cmath>

namespace
{

const SDL_Color kText{235, 235, 240, 255};
const SDL_Color kDim{150, 155, 165, 255};
const SDL_Color kAccent{120, 200, 255, 255};
const SDL_Color kWarn{255, 210, 120, 255};

// Grid position (0-5 = slots 1-6, 6 = undo slot) <-> slot number.
int SlotToPos(int slot) { return slot == SaveStore::kUndoSlot ? SaveStore::kSlots : slot - 1; }
int PosToSlot(int pos) { return pos == SaveStore::kSlots ? SaveStore::kUndoSlot : pos + 1; }

std::string SizeText(uint64_t bytes)
{
    if (bytes >= 1024 * 1024) return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string((bytes + 1023) / 1024) + " KB";
}

void Fill(SDL_Renderer* r, const SDL_Rect& rc, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca = 255)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_RenderFillRect(r, &rc);
}

} // namespace

void SavesMenu::Open(int slot)
{
    open = true;
    pending = Pending::None;
    quickSlot = std::clamp(slot, 1, SaveStore::kSlots);
    selected = quickSlot;
    tab = kStates;
    scroll = 0;
    status.clear();
    Refresh();
}

void SavesMenu::Close()
{
    open = false;
    pending = Pending::None;
    DropThumbs();
}

void SavesMenu::Refresh()
{
    for (int s = 0; s <= SaveStore::kSlots; s++) slots[s] = store.GetSlot(s);
    backups = store.ListBackups();
    thumbsStale = true;
    if (tab == kBackups) selected = std::clamp(selected, 0, std::max(0, (int)backups.size() - 1));
}

void SavesMenu::DropThumbs()
{
    for (SDL_Texture*& t : thumbs)
    {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
    thumbsStale = true;
}

void SavesMenu::SetStatus(const std::string& msg)
{
    status = msg;
    statusUntil = SDL_GetTicks64() + 4000;
}

int SavesMenu::NumItems() const
{
    return tab == kStates ? SaveStore::kSlots + 1 : (int)backups.size();
}

void SavesMenu::Ask(Pending what, const std::string& q)
{
    pending = what;
    question = q;
}

void SavesMenu::Confirm(bool yes)
{
    Pending what = pending;
    pending = Pending::None;
    if (!yes) return;

    switch (what)
    {
    case Pending::Overwrite:
        if (host.SaveStateTo(selected))
        {
            quickSlot = selected;
            SetStatus("Estado salvo no espaço " + std::to_string(selected) + ".");
        }
        else SetStatus("Não foi possível salvar o estado.");
        Refresh();
        break;
    case Pending::Delete:
        store.DeleteState(selected);
        SetStatus(selected == SaveStore::kUndoSlot ? "Estado de desfazer apagado."
                                                   : "Espaço " + std::to_string(selected) + " apagado.");
        Refresh();
        break;
    case Pending::Restore:
        if (selected < (int)backups.size() && host.RestoreBackup(backups[selected].path))
            Close();
        else
            SetStatus("Não foi possível restaurar esse backup.");
        break;
    case Pending::None:
        break;
    }
}

void SavesMenu::Activate(Button b)
{
    if (b == kNone || (b < kNumButtons && !buttonEnabled[b] && b != kTabStates && b != kTabBackups)) return;
    switch (b)
    {
    case kTabStates:
        if (tab != kStates) { tab = kStates; selected = quickSlot; }
        break;
    case kTabBackups:
        if (tab != kBackups) { tab = kBackups; selected = 0; scroll = 0; Refresh(); }
        break;
    case kSave:
        if (selected == SaveStore::kUndoSlot) break;
        if (slots[selected].exists)
            Ask(Pending::Overwrite, "Substituir o estado do espaço " + std::to_string(selected) + "?");
        else
        {
            pending = Pending::Overwrite; // empty slot: nothing to confirm
            Confirm(true);
        }
        break;
    case kLoad:
        if (!slots[selected].exists) break;
        if (selected != SaveStore::kUndoSlot) quickSlot = selected;
        if (host.LoadStateFrom(selected))
            Close();
        else
            SetStatus("Esse estado não pôde ser carregado (arquivo de outra versão ou corrompido).");
        break;
    case kDelete:
        if (slots[selected].exists)
            Ask(Pending::Delete, selected == SaveStore::kUndoSlot
                                     ? std::string("Apagar o estado de desfazer?")
                                     : "Apagar o estado do espaço " + std::to_string(selected) + "?");
        break;
    case kRestore:
        if (selected < (int)backups.size())
            Ask(Pending::Restore, "Restaurar o save de " + SaveStore::FormatTime(backups[selected].when) +
                                      "? O jogo reinicia; o save atual vira um backup.");
        break;
    case kFolder:
        store.OpenFolder();
        SetStatus("Pasta aberta no Explorador de Arquivos.");
        break;
    case kClose:
        Close();
        break;
    case kConfirmYes:
        Confirm(true);
        break;
    case kConfirmNo:
        Confirm(false);
        break;
    default:
        break;
    }
}

void SavesMenu::Move(int dx, int dy)
{
    if (tab == kBackups)
    {
        if (backups.empty()) return;
        selected = std::clamp(selected + dy + dx, 0, (int)backups.size() - 1);
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visibleRows) scroll = selected - visibleRows + 1;
        return;
    }
    int pos = SlotToPos(selected);
    int row = pos / 3, col = pos % 3;
    if (pos == SaveStore::kSlots) { row = 2; col = 0; }
    row = std::clamp(row + dy, 0, 2);
    if (row < 2) col = std::clamp(col + dx, 0, 2);
    selected = PosToSlot(row == 2 ? SaveStore::kSlots : row * 3 + col);
}

int SavesMenu::ItemAt(int x, int y) const
{
    SDL_Point p{x, y};
    for (const auto& it : itemRects)
        if (SDL_PointInRect(&p, &it.first)) return it.second;
    return -1;
}

SavesMenu::Button SavesMenu::ButtonAt(int x, int y) const
{
    SDL_Point p{x, y};
    // Only the dialog's buttons respond while it is up.
    if (pending != Pending::None)
    {
        if (SDL_PointInRect(&p, &buttonRects[kConfirmYes])) return kConfirmYes;
        if (SDL_PointInRect(&p, &buttonRects[kConfirmNo])) return kConfirmNo;
        return kNone;
    }
    for (int i = 0; i < kConfirmYes; i++)
        if (SDL_PointInRect(&p, &buttonRects[i])) return (Button)i;
    return kNone;
}

bool SavesMenu::HandleEvent(const SDL_Event& e, SDL_Window* window, SDL_Renderer* renderer)
{
    if (!open) return false;

    auto toRenderer = [&](int& x, int& y) {
        int ww, wh, rw, rh;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetRendererOutputSize(renderer, &rw, &rh);
        x = x * rw / std::max(ww, 1);
        y = y * rh / std::max(wh, 1);
    };

    if (pending != Pending::None)
    {
        switch (e.type)
        {
        case SDL_KEYDOWN:
            if (e.key.repeat) return true;
            switch (e.key.keysym.scancode)
            {
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_Y: case SDL_SCANCODE_S:
                Confirm(true); break;
            case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_N: case SDL_SCANCODE_BACKSPACE:
                Confirm(false); break;
            default: break;
            }
            return true;
        case SDL_CONTROLLERBUTTONDOWN:
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) Confirm(true);
            else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) Confirm(false);
            return true;
        case SDL_MOUSEBUTTONDOWN:
        {
            int x = e.button.x, y = e.button.y;
            toRenderer(x, y);
            Button b = ButtonAt(x, y);
            if (b == kConfirmYes || b == kConfirmNo) Activate(b);
            return true;
        }
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
        case SDL_SCANCODE_F2:
            if (!e.key.repeat) Close();
            break;
        case SDL_SCANCODE_TAB:
        case SDL_SCANCODE_Q:
        case SDL_SCANCODE_E:
            if (!e.key.repeat) Activate(tab == kStates ? kTabBackups : kTabStates);
            break;
        case SDL_SCANCODE_UP: Move(0, -1); break;
        case SDL_SCANCODE_DOWN: Move(0, 1); break;
        case SDL_SCANCODE_LEFT: Move(-1, 0); break;
        case SDL_SCANCODE_RIGHT: Move(1, 0); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!e.key.repeat) Activate(tab == kStates ? kLoad : kRestore);
            break;
        case SDL_SCANCODE_S:
            if (!e.key.repeat && tab == kStates) Activate(kSave);
            break;
        case SDL_SCANCODE_DELETE:
            if (!e.key.repeat && tab == kStates) Activate(kDelete);
            break;
        case SDL_SCANCODE_O:
            if (!e.key.repeat) Activate(kFolder);
            break;
        default: break;
        }
        return true;

    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button)
        {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: Move(0, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: Move(0, 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: Move(-1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: Move(1, 0); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: Activate(kTabStates); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: Activate(kTabBackups); break;
        case SDL_CONTROLLER_BUTTON_A: Activate(tab == kStates ? kLoad : kRestore); break;
        case SDL_CONTROLLER_BUTTON_X: if (tab == kStates) Activate(kSave); break;
        case SDL_CONTROLLER_BUTTON_Y: if (tab == kStates) Activate(kDelete); break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_BACK: Close(); break;
        default: break;
        }
        return true;

    case SDL_MOUSEMOTION:
    {
        int x = e.motion.x, y = e.motion.y;
        toRenderer(x, y);
        hoverButton = ButtonAt(x, y);
        return true;
    }

    case SDL_MOUSEBUTTONDOWN:
    {
        if (e.button.button != SDL_BUTTON_LEFT) return true;
        int x = e.button.x, y = e.button.y;
        toRenderer(x, y);
        Button b = ButtonAt(x, y);
        if (b != kNone)
        {
            Activate(b);
            return true;
        }
        int item = ItemAt(x, y);
        if (item >= 0)
        {
            bool again = item == selected;
            selected = item;
            // Double-click (or a second click) on an item uses it.
            if (again && e.button.clicks >= 2) Activate(tab == kStates ? kLoad : kRestore);
        }
        return true;
    }

    case SDL_MOUSEWHEEL:
        if (tab == kBackups && !backups.empty())
            scroll = std::clamp(scroll - e.wheel.y, 0, std::max(0, (int)backups.size() - visibleRows));
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

void SavesMenu::DrawButton(SDL_Renderer* r, TextRenderer& text, Button id, const std::string& label, int x, int y,
                           int w, int h, int textH, bool enabled)
{
    SDL_Rect rc{x, y, w, h};
    buttonRects[id] = rc;
    buttonEnabled[id] = enabled;
    bool hover = enabled && hoverButton == id;
    if (enabled) Fill(r, rc, hover ? 70 : 52, hover ? 105 : 58, hover ? 150 : 72);
    else Fill(r, rc, 38, 40, 48);
    text.Draw(label, x + w / 2, y + (h - textH) / 2 - textH / 10, textH, enabled ? kText : kDim, 1, true);
}

void SavesMenu::Render(SDL_Renderer* r, TextRenderer& text)
{
    if (!open) return;
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);

    if (thumbsStale)
    {
        for (int s = 0; s <= SaveStore::kSlots; s++)
        {
            if (thumbs[s]) SDL_DestroyTexture(thumbs[s]);
            thumbs[s] = nullptr;
            if (!slots[s].exists) continue;
            if (SDL_Surface* surf = SDL_LoadBMP(slots[s].thumbPath.c_str()))
            {
                thumbs[s] = SDL_CreateTextureFromSurface(r, surf);
                SDL_FreeSurface(surf);
            }
        }
        thumbsStale = false;
    }
    itemRects.clear();
    for (SDL_Rect& rc : buttonRects) rc = SDL_Rect{0, 0, 0, 0};

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    Fill(r, SDL_Rect{0, 0, w, h}, 0, 0, 0, 190);

    int u = std::max(16, h / 26);           // layout unit
    int textH = u * 58 / 100;
    int pad = u / 2;
    int panelW = std::min(w - u, u * 34);
    int panelH = std::min(h - u, u * 24);
    int px = (w - panelW) / 2, py = (h - panelH) / 2;
    SDL_Rect panel{px, py, panelW, panelH};
    Fill(r, panel, 24, 26, 32, 245);
    SDL_SetRenderDrawColor(r, 70, 75, 90, 255);
    SDL_RenderDrawRect(r, &panel);

    // Header: title and tabs.
    int y = py + pad;
    text.Draw("Saves", px + pad, y, u * 95 / 100, kText, 0, true);
    int tabW = u * 7, tabH = u * 6 / 5;
    int tx = px + panelW - pad - tabW * 2 - pad / 2;
    for (int t = 0; t < 2; t++)
    {
        Button id = t == 0 ? kTabStates : kTabBackups;
        SDL_Rect rc{tx + t * (tabW + pad / 2), y, tabW, tabH};
        buttonRects[id] = rc;
        bool active = (int)tab == t;
        Fill(r, rc, active ? 45 : 34, active ? 70 : 37, active ? 110 : 46);
        text.Draw(t == 0 ? "Estados salvos" : "Backups do save", rc.x + rc.w / 2, rc.y + (tabH - textH) / 2 - textH / 10,
                  textH, active ? kText : kDim, 1, true);
    }
    y += tabH + pad;

    int contentX = px + pad, contentW = panelW - pad * 2;
    int buttonsH = u * 13 / 10;
    int footerY = py + panelH - pad - textH;
    int buttonsY = footerY - pad / 2 - buttonsH;
    int contentBottom = buttonsY - pad;

    if (tab == kStates)
    {
        // 3x2 grid of slots, then the undo slot as a wide row.
        int undoH = u * 2;
        int gap = pad;
        int labelsH = textH * 26 / 10;
        int availH = contentBottom - y - undoH - gap;
        int cardW = std::min((contentW - gap * 2) / 3, (int)(((availH - gap) / 2 - labelsH) * 4 / 3));
        int cardH = cardW * 3 / 4 + labelsH;
        int gridX = contentX + (contentW - (cardW * 3 + gap * 2)) / 2;

        for (int pos = 0; pos <= SaveStore::kSlots; pos++)
        {
            int slot = PosToSlot(pos);
            bool undo = slot == SaveStore::kUndoSlot;
            SDL_Rect card = undo ? SDL_Rect{gridX, y + (cardH + gap) * 2, cardW * 3 + gap * 2, undoH}
                                 : SDL_Rect{gridX + (pos % 3) * (cardW + gap), y + (pos / 3) * (cardH + gap), cardW, cardH};
            itemRects.push_back({card, slot});
            bool sel = slot == selected;
            Fill(r, card, sel ? 45 : 32, sel ? 70 : 35, sel ? 110 : 44);
            if (sel)
            {
                SDL_SetRenderDrawColor(r, kAccent.r, kAccent.g, kAccent.b, 255);
                SDL_RenderDrawRect(r, &card);
            }

            int thumbH = undo ? undoH - pad : cardW * 3 / 4 - pad / 2;
            int thumbW = thumbH * 4 / 3;
            SDL_Rect th{card.x + (undo ? pad / 2 : (cardW - thumbW) / 2), card.y + pad / 4 + (undo ? pad / 4 : 0), thumbW, thumbH};
            if (thumbs[slot]) SDL_RenderCopy(r, thumbs[slot], nullptr, &th);
            else
            {
                Fill(r, th, 18, 19, 24);
                if (!undo) text.Draw("vazio", th.x + th.w / 2, th.y + th.h / 2 - textH / 2, textH, kDim, 1);
            }

            std::string name = undo ? "Desfazer (antes do último carregamento)" : "Espaço " + std::to_string(slot);
            if (!undo && slot == quickSlot) name += "  · rápido";
            std::string when = slots[slot].exists ? SaveStore::FormatTime(slots[slot].when) : "vazio";
            if (undo)
            {
                int lx = th.x + th.w + pad;
                text.Draw(name, lx, card.y + undoH / 2 - textH - textH / 10, textH, kText, 0, true);
                text.Draw(when, lx, card.y + undoH / 2 + textH / 10, textH, kDim);
            }
            else
            {
                int ly = th.y + th.h + pad / 4;
                text.Draw(name, card.x + cardW / 2, ly, textH, slot == quickSlot ? kAccent : kText, 1, true);
                text.Draw(when, card.x + cardW / 2, ly + textH * 12 / 10, textH * 9 / 10, kDim, 1);
            }
        }

        bool exists = slots[selected].exists;
        int bw = u * 6;
        DrawButton(r, text, kSave, "Salvar aqui (S)", contentX, buttonsY, bw, buttonsH, textH, selected != SaveStore::kUndoSlot);
        DrawButton(r, text, kLoad, "Carregar (Enter)", contentX + bw + pad / 2, buttonsY, bw, buttonsH, textH, exists);
        DrawButton(r, text, kDelete, "Apagar (Del)", contentX + (bw + pad / 2) * 2, buttonsY, bw, buttonsH, textH, exists);
    }
    else
    {
        text.Draw("Cópias automáticas do save do cartucho, feitas cada vez que o jogo grava.", contentX, y, textH, kDim);
        y += textH * 15 / 10;
        int rowH = u * 6 / 5;
        visibleRows = std::max(1, (contentBottom - y) / rowH);
        if (backups.empty())
        {
            text.Draw("Nenhum backup ainda: eles aparecem depois que você salvar no jogo.", contentX + contentW / 2,
                      y + rowH, textH, kText, 1);
        }
        scroll = std::clamp(scroll, 0, std::max(0, (int)backups.size() - visibleRows));
        for (int i = scroll; i < (int)backups.size() && i < scroll + visibleRows; i++)
        {
            SDL_Rect row{contentX, y + (i - scroll) * rowH, contentW, rowH - 2};
            itemRects.push_back({row, i});
            bool sel = i == selected;
            Fill(r, row, sel ? 45 : 32, sel ? 70 : 35, sel ? 110 : 44);
            int ty = row.y + (rowH - textH) / 2 - textH / 10;
            std::string label = SaveStore::FormatTime(backups[i].when) + (i == 0 ? "   (mais recente)" : "");
            text.Draw(label, row.x + pad, ty, textH, sel ? kText : kDim, 0, sel);
            text.Draw(SizeText(backups[i].size), row.x + row.w - pad, ty, textH, kDim, 2);
        }
        if ((int)backups.size() > visibleRows)
            text.Draw(std::to_string(scroll + 1) + "–" + std::to_string(std::min((int)backups.size(), scroll + visibleRows)) +
                          " de " + std::to_string(backups.size()),
                      contentX + contentW, contentBottom - textH / 2, textH * 85 / 100, kDim, 2);
        DrawButton(r, text, kRestore, "Restaurar (Enter)", contentX, buttonsY, u * 7, buttonsH, textH, !backups.empty());
    }

    int bw = u * 6;
    DrawButton(r, text, kClose, "Fechar (Esc)", px + panelW - pad - bw, buttonsY, bw, buttonsH, textH, true);
    DrawButton(r, text, kFolder, "Abrir pasta (O)", px + panelW - pad - bw * 2 - pad / 2, buttonsY, bw, buttonsH, textH, true);

    // Footer: last result, or what the keys do on this tab.
    if (!status.empty() && SDL_GetTicks64() < statusUntil)
        text.Draw(status, px + panelW / 2, footerY, textH, kAccent, 1);
    else if (tab == kStates)
        text.Draw("Carregar um estado também volta o save do jogo para aquele momento.  Tab: trocar de aba",
                  px + panelW / 2, footerY, textH * 85 / 100, kDim, 1);
    else
        text.Draw("Restaurar reinicia o jogo com o save escolhido; o save atual também vira backup.",
                  px + panelW / 2, footerY, textH * 85 / 100, kDim, 1);

    if (pending != Pending::None)
    {
        Fill(r, panel, 0, 0, 0, 140);
        int dw = std::min(panelW - u * 2, std::max(u * 18, text.Measure(question, textH) + u * 2));
        int dh = u * 4;
        SDL_Rect dlg{px + (panelW - dw) / 2, py + (panelH - dh) / 2, dw, dh};
        Fill(r, dlg, 36, 39, 48);
        SDL_SetRenderDrawColor(r, kWarn.r, kWarn.g, kWarn.b, 255);
        SDL_RenderDrawRect(r, &dlg);
        text.Draw(question, dlg.x + dw / 2, dlg.y + pad, textH, kText, 1, true);
        int qb = u * 5;
        DrawButton(r, text, kConfirmYes, "Sim (Enter)", dlg.x + dw / 2 - qb - pad / 2, dlg.y + dh - pad - buttonsH, qb,
                   buttonsH, textH, true);
        DrawButton(r, text, kConfirmNo, "Não (Esc)", dlg.x + dw / 2 + pad / 2, dlg.y + dh - pad - buttonsH, qb, buttonsH,
                   textH, true);
    }
}
