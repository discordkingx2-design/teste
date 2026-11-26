// XYZ Optimizer - interface (Win32 + GDI+, desenho proprio com animacoes)

#include "tweaks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>

namespace G = Gdiplus;

namespace {

// ------------------------------------------------------------------ layout
constexpr int kClientW = 960;
constexpr int kClientH = 580;
constexpr int kSidebarW = 200;
constexpr int kHeaderH = 84;
constexpr int kBottomY = 470;
constexpr int kCardW = 348;
constexpr int kCardH = 112;
constexpr int kGap = 16;
constexpr int kPad = 24;
constexpr int kTabs = 5;

constexpr UINT kMsgDone = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;

struct Rgb {
    int r, g, b;
};

const Rgb kCatColor[kTabs] = {{99, 102, 241}, {244, 63, 94}, {34, 197, 94}, {56, 189, 248}, {250, 204, 21}};
const wchar_t* const kCatGlyph[kTabs] = {L"\u26A1", L"\u2715", L"\u25C9", L"\u25A3", L"\u267B"};
const wchar_t* const kTabName[kTabs] = {L"Desempenho", L"Bloatware", L"Privacidade", L"Roblox", L"Limpeza"};
const wchar_t* const kTabSub[kTabs] = {
    L"Menos latência de entrada e mais resposta do sistema.",
    L"Remova apps inúteis e impeça a volta deles.",
    L"Corte telemetria e processos em segundo plano.",
    L"Gráficos mínimos para o máximo de FPS.",
    L"Libere espaço e remova arquivos inúteis.",
};

struct Rectf {
    float x, y, w, h;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Card {
    xyz::Tweak tweak;
    bool selected = false;
    float hover = 0.f;
    float sel = 0.f;
};

struct Hit {
    int type = 0;  // 0 nada, 1 aba, 2 card, 3 botao
    int index = -1;
};

struct App {
    HWND hwnd = nullptr;
    std::vector<Card> cards;
    int tab = 0;
    float tabAnim = 1.f;
    float scroll = 0.f;
    float scrollTarget = 0.f;
    float navHover[kTabs] = {0, 0, 0, 0, 0};
    float btnHover[4] = {0, 0, 0, 0};
    Hit hover;
    Hit pressed;
    bool restoreToggle = true;
    float restoreAnim = 1.f;
    bool trackingMouse = false;
    bool admin = false;
    std::atomic<bool> busy{false};
    std::atomic<float> progressTarget{0.f};
    std::atomic<bool> needRedraw{true};
    float progressShown = 0.f;
    float shimmer = 0.f;
    std::thread worker;
    std::mutex logMx;
    std::vector<std::wstring> log;
    ULONGLONG lastMs = 0;
};

App gApp;

// ------------------------------------------------------------------ graficos
G::FontFamily* gFam = nullptr;
G::FontFamily* gSym = nullptr;
G::Font* fLogo = nullptr;
G::Font* fTitle = nullptr;
G::Font* fH2 = nullptr;
G::Font* fBody = nullptr;
G::Font* fSmall = nullptr;
G::Font* fBtn = nullptr;
G::Font* fIcon = nullptr;
G::StringFormat* sfLeft = nullptr;
G::StringFormat* sfWrap = nullptr;
G::StringFormat* sfCenter = nullptr;
G::StringFormat* sfRight = nullptr;

float gAlpha = 1.f;  // multiplicador de opacidade (animacao de entrada)

void InitGfx() {
    gFam = new G::FontFamily(L"Segoe UI");
    if (!gFam->IsAvailable()) {
        delete gFam;
        gFam = new G::FontFamily(L"Arial");
    }
    gSym = new G::FontFamily(L"Segoe UI Symbol");
    if (!gSym->IsAvailable()) {
        delete gSym;
        gSym = new G::FontFamily(L"Arial");
    }
    fLogo = new G::Font(gFam, 22.f, G::FontStyleBold, G::UnitPixel);
    fTitle = new G::Font(gFam, 26.f, G::FontStyleBold, G::UnitPixel);
    fH2 = new G::Font(gFam, 15.f, G::FontStyleBold, G::UnitPixel);
    fBody = new G::Font(gFam, 14.f, G::FontStyleRegular, G::UnitPixel);
    fSmall = new G::Font(gFam, 12.f, G::FontStyleRegular, G::UnitPixel);
    fBtn = new G::Font(gFam, 14.f, G::FontStyleBold, G::UnitPixel);
    fIcon = new G::Font(gSym, 20.f, G::FontStyleRegular, G::UnitPixel);

    sfLeft = new G::StringFormat();
    sfLeft->SetFormatFlags(G::StringFormatFlagsNoWrap);
    sfLeft->SetTrimming(G::StringTrimmingEllipsisCharacter);
    sfLeft->SetLineAlignment(G::StringAlignmentNear);

    sfWrap = new G::StringFormat();
    sfWrap->SetFormatFlags(G::StringFormatFlagsLineLimit);
    sfWrap->SetTrimming(G::StringTrimmingEllipsisWord);

    sfCenter = new G::StringFormat();
    sfCenter->SetFormatFlags(G::StringFormatFlagsNoWrap);
    sfCenter->SetAlignment(G::StringAlignmentCenter);
    sfCenter->SetLineAlignment(G::StringAlignmentCenter);

    sfRight = new G::StringFormat();
    sfRight->SetFormatFlags(G::StringFormatFlagsNoWrap);
    sfRight->SetAlignment(G::StringAlignmentFar);
}

void ShutdownGfx() {
    delete sfLeft;
    delete sfWrap;
    delete sfCenter;
    delete sfRight;
    delete fLogo;
    delete fTitle;
    delete fH2;
    delete fBody;
    delete fSmall;
    delete fBtn;
    delete fIcon;
    delete gFam;
    delete gSym;
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }
int LerpI(int a, int b, float t) { return static_cast<int>(static_cast<float>(a) + static_cast<float>(b - a) * t + 0.5f); }
float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

G::Color Col(int r, int gr, int b, int a = 255) {
    int aa = static_cast<int>(static_cast<float>(a) * gAlpha);
    if (aa < 0) aa = 0;
    if (aa > 255) aa = 255;
    return G::Color(static_cast<BYTE>(aa), static_cast<BYTE>(r), static_cast<BYTE>(gr), static_cast<BYTE>(b));
}

void RRPath(G::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2.f;
    p.AddArc(x, y, d, d, 180.f, 90.f);
    p.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    p.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    p.CloseFigure();
}

void FillRR(G::Graphics& gr, const G::Color& c, float x, float y, float w, float h, float r) {
    G::GraphicsPath p;
    RRPath(p, x, y, w, h, r);
    G::SolidBrush b(c);
    gr.FillPath(&b, &p);
}

void StrokeRR(G::Graphics& gr, const G::Color& c, float width, float x, float y, float w, float h, float r) {
    G::GraphicsPath p;
    RRPath(p, x, y, w, h, r);
    G::Pen pen(c, width);
    gr.DrawPath(&pen, &p);
}

void FillRRGrad(G::Graphics& gr, const G::Color& c1, const G::Color& c2, float x, float y, float w, float h, float r) {
    G::GraphicsPath p;
    RRPath(p, x, y, w, h, r);
    G::LinearGradientBrush b(G::RectF(x, y, w, h), c1, c2, G::LinearGradientModeForwardDiagonal);
    gr.FillPath(&b, &p);
}

void Text(G::Graphics& gr, const wchar_t* s, G::Font* f, const G::Color& c, float x, float y, float w, float h,
          G::StringFormat* sf) {
    G::SolidBrush b(c);
    G::RectF r(x, y, w, h);
    gr.DrawString(s, -1, f, r, sf, &b);
}

// ------------------------------------------------------------------ geometria
Rectf NavRect(int i) { return Rectf{16.f, 118.f + static_cast<float>(i) * 48.f, 168.f, 40.f}; }

Rectf BtnRect(int i) {
    switch (i) {
        case 0: return Rectf{static_cast<float>(kClientW - kPad) - 176.f, 26.f, 176.f, 34.f};   // recomendados
        case 1: return Rectf{static_cast<float>(kClientW - kPad) - 200.f, 480.f, 200.f, 40.f};  // aplicar
        case 2: return Rectf{static_cast<float>(kClientW - kPad) - 200.f, 528.f, 200.f, 32.f};  // reverter
        default: return Rectf{16.f, 514.f, 168.f, 40.f};                                        // restauracao
    }
}

std::vector<int> TabCardIdx(int tab) {
    std::vector<int> idx;
    for (size_t i = 0; i < gApp.cards.size(); ++i) {
        if (static_cast<int>(gApp.cards[i].tweak.category) == tab) idx.push_back(static_cast<int>(i));
    }
    return idx;
}

Rectf CardRect(int pos) {
    int col = pos % 2;
    int row = pos / 2;
    float x = static_cast<float>(kSidebarW + kPad + col * (kCardW + kGap));
    float y = static_cast<float>(kHeaderH + row * (kCardH + kGap)) - gApp.scroll + (1.f - gApp.tabAnim) * 18.f;
    return Rectf{x, y, static_cast<float>(kCardW), static_cast<float>(kCardH)};
}

float MaxScroll() {
    size_t n = TabCardIdx(gApp.tab).size();
    int rows = static_cast<int>((n + 1) / 2);
    float content = static_cast<float>(rows * (kCardH + kGap));
    float view = static_cast<float>(kBottomY - kHeaderH);
    float m = content - view;
    return m > 0.f ? m : 0.f;
}

Hit HitTest(float x, float y) {
    Hit h;
    for (int i = 0; i < 4; ++i) {
        if (BtnRect(i).Contains(x, y)) {
            h.type = 3;
            h.index = i;
            return h;
        }
    }
    for (int i = 0; i < kTabs; ++i) {
        if (NavRect(i).Contains(x, y)) {
            h.type = 1;
            h.index = i;
            return h;
        }
    }
    if (x >= static_cast<float>(kSidebarW) && y >= static_cast<float>(kHeaderH - 8) && y < static_cast<float>(kBottomY)) {
        std::vector<int> idx = TabCardIdx(gApp.tab);
        for (size_t k = 0; k < idx.size(); ++k) {
            if (CardRect(static_cast<int>(k)).Contains(x, y)) {
                h.type = 2;
                h.index = idx[k];
                return h;
            }
        }
    }
    return h;
}

bool SameHit(const Hit& a, const Hit& b) { return a.type == b.type && a.index == b.index; }

// ------------------------------------------------------------------ log
void AddLog(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(gApp.logMx);
    gApp.log.push_back(s);
    if (gApp.log.size() > 300) gApp.log.erase(gApp.log.begin(), gApp.log.begin() + 100);
    gApp.needRedraw = true;
}

// ------------------------------------------------------------------ desenho
void DrawToggle(G::Graphics& gr, float x, float y, float t, const Rgb& c) {
    const float w = 44.f;
    const float h = 24.f;
    FillRR(gr, Col(LerpI(58, c.r, t), LerpI(64, c.g, t), LerpI(80, c.b, t)), x, y, w, h, 12.f);
    float kx = x + 3.f + (w - 6.f - 18.f) * t;
    G::SolidBrush wb(Col(255, 255, 255));
    gr.FillEllipse(&wb, kx, y + 3.f, 18.f, 18.f);
}

void DrawButton(G::Graphics& gr, const Rectf& r, const wchar_t* label, bool primary, float hover, bool enabled) {
    float saved = gAlpha;
    if (!enabled) gAlpha *= 0.45f;
    float lift = -1.5f * hover;
    int hv = static_cast<int>(20.f * hover);
    if (primary) {
        FillRR(gr, Col(99, 102, 241, static_cast<int>(40.f + 50.f * hover)), r.x - 2.f, r.y + lift + 1.f, r.w + 4.f,
               r.h + 4.f, 12.f);
        FillRRGrad(gr, Col(99 + hv, 102 + hv, 241), Col(168, 85 + hv, 247), r.x, r.y + lift, r.w, r.h, 10.f);
    } else {
        FillRR(gr, Col(30 + hv / 2, 34 + hv / 2, 46 + hv / 2), r.x, r.y + lift, r.w, r.h, 10.f);
        StrokeRR(gr, Col(70, 78, 98, 200), 1.f, r.x, r.y + lift, r.w, r.h, 10.f);
    }
    Text(gr, label, fBtn, Col(255, 255, 255), r.x, r.y + lift, r.w, r.h, sfCenter);
    gAlpha = saved;
}

void DrawCard(G::Graphics& gr, const Card& c, const Rectf& r) {
    const Rgb& col = kCatColor[static_cast<int>(c.tweak.category)];
    float yy = r.y - 3.f * c.hover;
    float x = r.x;
    float w = r.w;
    float h = r.h;

    for (int i = 4; i >= 1; --i) {
        FillRR(gr, Col(0, 0, 0, 14 + static_cast<int>(10.f * c.hover)), x - static_cast<float>(i),
               yy - static_cast<float>(i) + 5.f, w + static_cast<float>(2 * i), h + static_cast<float>(2 * i),
               14.f + static_cast<float>(i));
    }
    int base = 27 + static_cast<int>(8.f * c.hover);
    FillRR(gr, Col(base, base + 4, base + 16), x, yy, w, h, 14.f);
    if (c.sel > 0.01f) FillRR(gr, Col(col.r, col.g, col.b, static_cast<int>(24.f * c.sel)), x, yy, w, h, 14.f);

    int borderA = static_cast<int>(Lerp(30.f, 70.f, c.hover) + 130.f * c.sel);
    if (borderA > 255) borderA = 255;
    StrokeRR(gr, Col(col.r, col.g, col.b, borderA), 1.2f, x, yy, w, h, 14.f);

    // icone da categoria
    FillRRGrad(gr, Col(col.r, col.g, col.b), Col(col.r / 2, col.g / 2, col.b / 2 + 40), x + 16.f, yy + 16.f, 44.f,
               44.f, 12.f);
    Text(gr, kCatGlyph[static_cast<int>(c.tweak.category)], fIcon, Col(255, 255, 255), x + 16.f, yy + 16.f, 44.f, 44.f,
         sfCenter);

    // textos
    Text(gr, c.tweak.title.c_str(), fH2, Col(240, 243, 250), x + 72.f, yy + 14.f, w - 72.f - 76.f, 22.f, sfLeft);
    Text(gr, c.tweak.description.c_str(), fSmall, Col(150, 158, 176), x + 72.f, yy + 38.f, w - 72.f - 16.f, 50.f,
         sfWrap);

    // etiqueta reversivel / permanente
    if (c.tweak.revertible) {
        Text(gr, L"\u25CF Reversível", fSmall, Col(74, 222, 128), x + 72.f, yy + 90.f, 140.f, 16.f, sfLeft);
    } else {
        Text(gr, L"\u25CF Permanente", fSmall, Col(251, 191, 36), x + 72.f, yy + 90.f, 140.f, 16.f, sfLeft);
    }

    DrawToggle(gr, x + w - 16.f - 44.f, yy + 16.f, c.sel, col);
}

void DrawAll(G::Graphics& gr) {
    gr.SetSmoothingMode(G::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(G::TextRenderingHintAntiAliasGridFit);
    gAlpha = 1.f;
    gr.Clear(Col(13, 15, 21));

    // brilho suave no topo da area principal
    {
        G::LinearGradientBrush glow(G::RectF(static_cast<float>(kSidebarW), 0.f,
                                             static_cast<float>(kClientW - kSidebarW), 150.f),
                                    Col(99, 102, 241, 34), Col(13, 15, 21, 0), G::LinearGradientModeVertical);
        gr.FillRectangle(&glow, static_cast<float>(kSidebarW), 0.f, static_cast<float>(kClientW - kSidebarW), 150.f);
    }

    // ---- barra lateral
    {
        G::SolidBrush sb(Col(18, 21, 29));
        gr.FillRectangle(&sb, 0.f, 0.f, static_cast<float>(kSidebarW), static_cast<float>(kClientH));
        G::Pen line(Col(34, 38, 52), 1.f);
        gr.DrawLine(&line, static_cast<float>(kSidebarW) - 0.5f, 0.f, static_cast<float>(kSidebarW) - 0.5f,
                    static_cast<float>(kClientH));
    }
    FillRRGrad(gr, Col(99, 102, 241), Col(168, 85, 247), 20.f, 24.f, 42.f, 42.f, 12.f);
    Text(gr, L"X", fLogo, Col(255, 255, 255), 20.f, 24.f, 42.f, 42.f, sfCenter);
    Text(gr, L"XYZ", fLogo, Col(240, 243, 250), 72.f, 20.f, 120.f, 28.f, sfLeft);
    Text(gr, L"OPTIMIZER", fSmall, Col(120, 128, 150), 73.f, 46.f, 120.f, 16.f, sfLeft);

    for (int i = 0; i < kTabs; ++i) {
        Rectf r = NavRect(i);
        const Rgb& c = kCatColor[i];
        bool active = (i == gApp.tab);
        float hv = gApp.navHover[i];
        if (active) {
            FillRR(gr, Col(c.r, c.g, c.b, 34), r.x, r.y, r.w, r.h, 10.f);
            FillRR(gr, Col(c.r, c.g, c.b), r.x, r.y + 10.f, 3.f, r.h - 20.f, 1.5f);
        } else if (hv > 0.01f) {
            FillRR(gr, Col(255, 255, 255, static_cast<int>(12.f * hv)), r.x, r.y, r.w, r.h, 10.f);
        }
        Text(gr, kCatGlyph[i], fIcon, Col(c.r, c.g, c.b), r.x + 8.f, r.y, 30.f, r.h, sfCenter);
        Text(gr, kTabName[i], fBody, active ? Col(245, 247, 252) : Col(160, 168, 186), r.x + 46.f, r.y + 10.f,
             r.w - 50.f, 22.f, sfLeft);
    }

    // chip de administrador
    FillRR(gr, Col(26, 30, 41), 16.f, 470.f, 168.f, 30.f, 8.f);
    if (gApp.admin) {
        Text(gr, L"\u25CF Administrador", fSmall, Col(74, 222, 128), 28.f, 470.f, 150.f, 30.f, sfLeft);
    } else {
        Text(gr, L"\u25CF Sem permissão de admin", fSmall, Col(248, 113, 113), 28.f, 470.f, 150.f, 30.f, sfLeft);
    }

    // alternador do ponto de restauracao
    {
        Rectf r = BtnRect(3);
        float hv = gApp.btnHover[3];
        FillRR(gr, Col(26 + static_cast<int>(8.f * hv), 30 + static_cast<int>(8.f * hv), 41 + static_cast<int>(8.f * hv)),
               r.x, r.y, r.w, r.h, 10.f);
        Text(gr, L"Ponto de restauração", fSmall, Col(180, 188, 205), r.x + 12.f, r.y + 4.f, 100.f, 32.f, sfWrap);
        DrawToggle(gr, r.x + r.w - 56.f, r.y + 8.f, gApp.restoreAnim, kCatColor[0]);
    }

    // ---- cabecalho
    Text(gr, kTabName[gApp.tab], fTitle, Col(245, 247, 252), static_cast<float>(kSidebarW + kPad), 16.f, 420.f, 36.f,
         sfLeft);
    Text(gr, kTabSub[gApp.tab], fBody, Col(140, 148, 168), static_cast<float>(kSidebarW + kPad), 54.f, 420.f, 22.f,
         sfLeft);
    {
        std::vector<int> idx = TabCardIdx(gApp.tab);
        int selected = 0;
        for (int i : idx) {
            if (gApp.cards[static_cast<size_t>(i)].selected) ++selected;
        }
        std::wstring counter = std::to_wstring(selected) + L" de " + std::to_wstring(idx.size()) + L" selecionados";
        Text(gr, counter.c_str(), fSmall, Col(120, 128, 150), static_cast<float>(kClientW - kPad) - 176.f - 170.f, 33.f,
             160.f, 18.f, sfRight);
    }
    DrawButton(gr, BtnRect(0), L"Marcar recomendados", false, gApp.btnHover[0], !gApp.busy.load());

    // ---- cards (area com recorte)
    gr.SetClip(G::RectF(static_cast<float>(kSidebarW), static_cast<float>(kHeaderH - 8),
                        static_cast<float>(kClientW - kSidebarW), static_cast<float>(kBottomY - kHeaderH + 8)),
               G::CombineModeReplace);
    gAlpha = Clamp01(gApp.tabAnim);
    {
        std::vector<int> idx = TabCardIdx(gApp.tab);
        for (size_t k = 0; k < idx.size(); ++k) {
            DrawCard(gr, gApp.cards[static_cast<size_t>(idx[k])], CardRect(static_cast<int>(k)));
        }
    }
    gAlpha = 1.f;
    gr.ResetClip();

    // ---- painel inferior
    {
        G::SolidBrush pb(Col(16, 18, 25));
        gr.FillRectangle(&pb, static_cast<float>(kSidebarW), static_cast<float>(kBottomY),
                         static_cast<float>(kClientW - kSidebarW), static_cast<float>(kClientH - kBottomY));
        G::Pen line(Col(34, 38, 52), 1.f);
        gr.DrawLine(&line, static_cast<float>(kSidebarW), static_cast<float>(kBottomY), static_cast<float>(kClientW),
                    static_cast<float>(kBottomY));
    }
    {
        float bx = static_cast<float>(kSidebarW + kPad);
        float by = static_cast<float>(kBottomY) + 16.f;
        float bw = 470.f;
        float bh = 8.f;
        FillRR(gr, Col(38, 43, 58), bx, by, bw, bh, 4.f);
        float pw = bw * Clamp01(gApp.progressShown);
        if (pw >= 8.f) {
            FillRRGrad(gr, Col(99, 102, 241), Col(56, 189, 248), bx, by, pw, bh, 4.f);
            if (gApp.busy.load()) {
                G::GraphicsPath clipPath;
                RRPath(clipPath, bx, by, pw, bh, 4.f);
                gr.SetClip(&clipPath, G::CombineModeReplace);
                float sx = bx + std::fmod(gApp.shimmer * 240.f, bw + 80.f) - 60.f;
                G::SolidBrush sh(Col(255, 255, 255, 70));
                gr.FillRectangle(&sh, sx, by, 50.f, bh);
                gr.ResetClip();
            }
        }
        int pct = static_cast<int>(Clamp01(gApp.progressShown) * 100.f + 0.5f);
        std::wstring ptxt = std::to_wstring(pct) + L"%";
        Text(gr, ptxt.c_str(), fSmall, Col(150, 158, 176), bx + bw + 10.f, by - 4.f, 50.f, 16.f, sfLeft);

        std::vector<std::wstring> lines;
        {
            std::lock_guard<std::mutex> lk(gApp.logMx);
            size_t n = gApp.log.size();
            size_t start = n > 4 ? n - 4 : 0;
            lines.assign(gApp.log.begin() + static_cast<std::ptrdiff_t>(start), gApp.log.end());
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::wstring& s = lines[i];
            bool last = (i + 1 == lines.size());
            G::Color c = last ? Col(205, 211, 224) : Col(110, 118, 138);
            if (s.compare(0, 4, L"[OK]") == 0) c = Col(74, 222, 128);
            if (s.compare(0, 3, L"[X]") == 0) c = Col(248, 113, 113);
            Text(gr, s.c_str(), fSmall, c, bx, static_cast<float>(kBottomY) + 36.f + static_cast<float>(i) * 17.f,
                 static_cast<float>(kClientW - kSidebarW - 2 * kPad - 200 - 16), 16.f, sfLeft);
        }
    }
    DrawButton(gr, BtnRect(1), gApp.busy.load() ? L"Trabalhando..." : L"Aplicar selecionados", true,
               gApp.btnHover[1], !gApp.busy.load());
    DrawButton(gr, BtnRect(2), L"Reverter selecionados", false, gApp.btnHover[2], !gApp.busy.load());
}

void Paint(HDC hdc) {
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, kClientW, kClientH);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        G::Graphics gr(mem);
        DrawAll(gr);
    }
    BitBlt(hdc, 0, 0, kClientW, kClientH, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ------------------------------------------------------------------ animacao
bool Smooth(float& v, float target, float dt, float rate) {
    float old = v;
    v += (target - v) * (1.f - std::exp(-rate * dt));
    if (std::fabs(target - v) < 0.002f) v = target;
    return v != old;
}

bool Tick(float dt) {
    bool changed = false;
    for (size_t i = 0; i < gApp.cards.size(); ++i) {
        Card& c = gApp.cards[i];
        float ht = (gApp.hover.type == 2 && gApp.hover.index == static_cast<int>(i)) ? 1.f : 0.f;
        changed = Smooth(c.hover, ht, dt, 14.f) || changed;
        changed = Smooth(c.sel, c.selected ? 1.f : 0.f, dt, 16.f) || changed;
    }
    for (int i = 0; i < kTabs; ++i) {
        float t = (gApp.hover.type == 1 && gApp.hover.index == i) ? 1.f : 0.f;
        changed = Smooth(gApp.navHover[i], t, dt, 14.f) || changed;
    }
    for (int i = 0; i < 4; ++i) {
        float t = (gApp.hover.type == 3 && gApp.hover.index == i) ? 1.f : 0.f;
        changed = Smooth(gApp.btnHover[i], t, dt, 14.f) || changed;
    }
    changed = Smooth(gApp.restoreAnim, gApp.restoreToggle ? 1.f : 0.f, dt, 16.f) || changed;
    changed = Smooth(gApp.tabAnim, 1.f, dt, 9.f) || changed;
    changed = Smooth(gApp.scroll, gApp.scrollTarget, dt, 16.f) || changed;
    changed = Smooth(gApp.progressShown, gApp.progressTarget.load(), dt, 8.f) || changed;
    if (gApp.busy.load()) {
        gApp.shimmer += dt;
        changed = true;
    }
    return changed;
}

// ------------------------------------------------------------------ acoes
void SelectRecommended() {
    for (auto& c : gApp.cards) c.selected = c.tweak.recommended;
    AddLog(L"Itens recomendados marcados em todas as abas.");
}

void StartWorker(bool revert) {
    if (gApp.busy.load()) return;
    std::vector<xyz::Tweak> list;
    for (const auto& c : gApp.cards) {
        if (c.selected) list.push_back(c.tweak);
    }
    if (list.empty()) {
        AddLog(L"Selecione ao menos um card antes de continuar.");
        return;
    }
    bool restorePoint = gApp.restoreToggle && !revert;
    if (gApp.worker.joinable()) gApp.worker.join();
    gApp.busy = true;
    gApp.progressShown = 0.f;
    gApp.progressTarget = 0.f;
    HWND hwnd = gApp.hwnd;

    gApp.worker = std::thread([list, revert, restorePoint, hwnd]() {
        xyz::LogFn log = [](const std::wstring& s) { AddLog(s); };
        size_t total = list.size() + (restorePoint ? 1u : 0u);
        size_t done = 0;
        int okCount = 0;
        int failCount = 0;
        int skipCount = 0;
        auto step = [&]() {
            ++done;
            gApp.progressTarget = static_cast<float>(done) / static_cast<float>(total);
        };

        if (restorePoint) {
            AddLog(L"Criando ponto de restauração...");
            bool ok = xyz::CreateRestorePoint(log);
            AddLog(ok ? L"[OK] Ponto de restauração criado" : L"[X] Ponto de restauração não criado");
            step();
        }
        for (const auto& t : list) {
            AddLog(std::wstring(revert ? L"Revertendo: " : L"Aplicando: ") + t.title);
            bool ok = false;
            if (revert) {
                if (!t.revertible || !t.revert) {
                    AddLog(L"  este item é permanente e não pode ser revertido.");
                    ++skipCount;
                    step();
                    continue;
                }
                ok = t.revert(log);
            } else {
                ok = t.apply ? t.apply(log) : false;
            }
            if (ok) {
                AddLog(L"[OK] " + t.title);
                ++okCount;
            } else {
                AddLog(L"[X] " + t.title);
                ++failCount;
            }
            step();
        }
        AddLog(L"Concluído: " + std::to_wstring(okCount) + L" ok, " + std::to_wstring(failCount) + L" falhas, " +
               std::to_wstring(skipCount) + L" ignorados.");
        PostMessageW(hwnd, kMsgDone, 0, 0);
    });
}

void Activate(const Hit& h) {
    if (h.type == 1) {
        if (h.index != gApp.tab) {
            gApp.tab = h.index;
            gApp.tabAnim = 0.f;
            gApp.scroll = 0.f;
            gApp.scrollTarget = 0.f;
        }
    } else if (h.type == 2) {
        if (!gApp.busy.load()) {
            Card& c = gApp.cards[static_cast<size_t>(h.index)];
            c.selected = !c.selected;
        }
    } else if (h.type == 3) {
        switch (h.index) {
            case 0:
                if (!gApp.busy.load()) SelectRecommended();
                break;
            case 1: StartWorker(false); break;
            case 2: StartWorker(true); break;
            default:
                if (!gApp.busy.load()) gApp.restoreToggle = !gApp.restoreToggle;
                break;
        }
    }
    gApp.needRedraw = true;
}

// ------------------------------------------------------------------ janela
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            gApp.hwnd = hwnd;
            gApp.lastMs = GetTickCount64();
            SetTimer(hwnd, kTimerId, 16, nullptr);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER: {
            ULONGLONG now = GetTickCount64();
            float dt = static_cast<float>(now - gApp.lastMs) / 1000.f;
            gApp.lastMs = now;
            if (dt > 0.05f) dt = 0.05f;
            bool changed = Tick(dt);
            if (gApp.needRedraw.exchange(false) || changed) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!gApp.trackingMouse) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                gApp.trackingMouse = true;
            }
            float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
            float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
            gApp.hover = HitTest(x, y);
            return 0;
        }

        case WM_MOUSELEAVE:
            gApp.trackingMouse = false;
            gApp.hover = Hit();
            return 0;

        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
            float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
            gApp.pressed = HitTest(x, y);
            return 0;
        }

        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
            float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
            Hit h = HitTest(x, y);
            if (h.type != 0 && SameHit(h, gApp.pressed)) Activate(h);
            gApp.pressed = Hit();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            gApp.scrollTarget -= static_cast<float>(delta) * 0.5f;
            float m = MaxScroll();
            if (gApp.scrollTarget < 0.f) gApp.scrollTarget = 0.f;
            if (gApp.scrollTarget > m) gApp.scrollTarget = m;
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && gApp.hover.type != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;

        case kMsgDone:
            if (gApp.worker.joinable()) gApp.worker.join();
            gApp.busy = false;
            gApp.needRedraw = true;
            return 0;

        case WM_CLOSE:
            if (gApp.busy.load()) {
                MessageBoxW(hwnd, L"Aguarde as otimizações terminarem antes de fechar.", L"XYZ Optimizer",
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            if (gApp.worker.joinable()) gApp.worker.join();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    G::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (G::GdiplusStartup(&token, &startup, nullptr) != G::Ok) return 1;
    InitGfx();

    gApp.admin = xyz::IsAdministrator();
    {
        std::vector<xyz::Tweak> tweaks = xyz::BuildTweaks();
        for (auto& t : tweaks) {
            Card c;
            c.tweak = t;
            c.selected = t.recommended;
            c.sel = c.selected ? 1.f : 0.f;
            gApp.cards.push_back(std::move(c));
        }
    }
    AddLog(gApp.admin ? L"Pronto. Marque os cards e clique em Aplicar selecionados."
                      : L"[X] Execute como administrador para aplicar as otimizações.");

    const wchar_t* kClass = L"XYZOptimizerWindow";
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, kClientW, kClientH};
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int px = (sx - w) / 2;
    int py = (sy - h) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    HWND hwnd = CreateWindowExW(0, kClass, L"XYZ Optimizer", style, px, py, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        ShutdownGfx();
        G::GdiplusShutdown(token);
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    COLORREF caption = RGB(18, 21, 29);
    DwmSetWindowAttribute(hwnd, 35, &caption, sizeof(caption));

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownGfx();
    G::GdiplusShutdown(token);
    return static_cast<int>(msg.wParam);
}
