#include "ScrollOverview.hpp"
#include <algorithm>
#include <cmath>
#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#undef protected
#undef private
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

using Render::GL::g_pHyprOpenGL;

void CScrollOverview::clearCurrentRenderTarget(const CHyprColor& color) {
    g_pHyprRenderer->draw(CClearPassElement::SClearData{color});
}

CBox CScrollOverview::shrinkBox(CBox box, const double amount) {
    if (amount <= 0.0 || box.empty())
        return box;

    const double shrinkX = std::min(amount, std::max(0.0, (box.w - 1.0) / 2.0));
    const double shrinkY = std::min(amount, std::max(0.0, (box.h - 1.0) / 2.0));

    box.x += shrinkX;
    box.y += shrinkY;
    box.w -= shrinkX * 2.0;
    box.h -= shrinkY * 2.0;

    return box;
}

std::string CScrollOverview::workspaceAnnotationText(PHLWORKSPACE workspace) const {
    if (!workspace)
        return "";

    const std::string MODE = g_hyprviewConfig.scrolling.workspaceAnnotation;
    if (MODE == "none")
        return "";

    const auto ID      = std::to_string(workspace->m_id);
    const auto NAME    = workspace->m_name.empty() ? ID : workspace->m_name;
    const bool SAME_ID = NAME == ID;

    if (MODE == "name")
        return NAME;
    if (MODE == "id_name")
        return SAME_ID ? ID : ID + ": " + NAME;
    if (MODE == "name_id")
        return SAME_ID ? ID : NAME + " (" + ID + ")";
    if (MODE == "id_windows")
        return ID + " (" + std::to_string(workspace->getWindows()) + ")";

    return ID;
}

SP<Render::ITexture> CScrollOverview::labelTexture(const std::string& text, int64_t color, int fontSize) {
    if (text.empty() || fontSize <= 0 || !pMonitor)
        return nullptr;

    const auto SCALE        = std::max(1.0, sc<double>(pMonitor->m_scale));
    const auto FONT_SIZE_PX = std::max(1, sc<int>(std::round(sc<double>(fontSize) * SCALE)));
    const auto KEY          = std::to_string(color) + ":" + std::to_string(FONT_SIZE_PX) + ":" + text;

    if (const auto IT = labelTextureCache.find(KEY); IT != labelTextureCache.end())
        return IT->second;

    auto TEXTURE = g_pHyprRenderer->renderText(text, CHyprColor{color}, FONT_SIZE_PX, false, "Sans Bold");
    if (!TEXTURE)
        return nullptr;

    TEXTURE->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    TEXTURE->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    labelTextureCache.emplace(KEY, TEXTURE);
    return TEXTURE;
}

Vector2D CScrollOverview::labelTextureSize(SP<Render::ITexture> texture) const {
    if (!texture || !pMonitor)
        return {};

    return texture->m_size / std::max(1.0, sc<double>(pMonitor->m_scale));
}

void CScrollOverview::renderTextureLabel(SP<Render::ITexture> texture, const CBox& box, double alpha) {
    if (!texture || !pMonitor || box.empty())
        return;

    CBox texbox = box;
    texbox.scale(pMonitor->m_scale).round();

    CRegion damage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprOpenGL->renderTextureInternal(texture, texbox, {.damage = &damage, .a = std::clamp(alpha, 0.0, 1.0)});
}

void CScrollOverview::renderWorkspaceAnnotations() {
    if (g_hyprviewConfig.scrolling.workspaceAnnotation == "none")
        return;

    const auto FONT_SIZE = std::max(1, g_hyprviewConfig.scrolling.workspaceAnnotationFontSize);
    const auto PAD       = std::max(3.0, sc<double>(FONT_SIZE) * 0.35);
    const auto MARGIN    = std::max(4.0, sc<double>(FONT_SIZE) * 0.45);

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry || !workspaceEntry->pWorkspace || workspaceEntry->overviewBox.empty())
            continue;

        const auto TEXT    = workspaceAnnotationText(workspaceEntry->pWorkspace);
        const auto TEXTURE = labelTexture(TEXT, g_hyprviewConfig.scrolling.workspaceAnnotationColor, FONT_SIZE);
        const auto SIZE    = labelTextureSize(TEXTURE);
        if (!TEXTURE || SIZE.x <= 0.0 || SIZE.y <= 0.0)
            continue;

        CBox              bgBox = {workspaceEntry->overviewBox.x + MARGIN, workspaceEntry->overviewBox.y + MARGIN, SIZE.x + PAD * 2.0, SIZE.y + PAD * 2.0};

        const std::string POSITION = g_hyprviewConfig.scrolling.workspaceAnnotationPosition;
        if (POSITION.ends_with("right"))
            bgBox.x = workspaceEntry->overviewBox.x + workspaceEntry->overviewBox.w - bgBox.w - MARGIN;
        if (POSITION.starts_with("bottom"))
            bgBox.y = workspaceEntry->overviewBox.y + workspaceEntry->overviewBox.h - bgBox.h - MARGIN;

        if (bgBox.w > workspaceEntry->overviewBox.w || bgBox.h > workspaceEntry->overviewBox.h)
            continue;

        CBox scaledBg = bgBox;
        scaledBg.scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(scaledBg, CHyprColor{g_hyprviewConfig.scrolling.workspaceAnnotationBgColor}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 5});

        renderTextureLabel(TEXTURE, {bgBox.x + PAD, bgBox.y + PAD, SIZE.x, SIZE.y});
    }
}

void CScrollOverview::renderInsertionMarkers() {
    if (inputState.mode != ePointerMode::WINDOW_DRAG)
        return;

    for (const auto& marker : insertionMarkers) {
        if (marker.box.empty())
            continue;

        const bool HOVERED = inputState.dropTarget.type == eDropTargetType::WORKSPACE_INSERTION && inputState.dropTarget.targetWorkspaceID == marker.workspaceID;

        CBox       markerBox = marker.box;
        markerBox.scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(markerBox, CHyprColor{g_hyprviewConfig.scrolling.insertionMarkerColor}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = HOVERED ? 8 : 6});

        if (!g_hyprviewConfig.scrolling.insertionMarkerLabels)
            continue;

        auto     font    = std::max(8, g_hyprviewConfig.scrolling.workspaceAnnotationFontSize - 1);
        auto     TEXTURE = labelTexture(marker.label, g_hyprviewConfig.scrolling.workspaceAnnotationColor, font);
        Vector2D SIZE    = labelTextureSize(TEXTURE);
        while (TEXTURE && font > 7 && (SIZE.x > marker.box.w - 4.0 || SIZE.y > marker.box.h - 2.0)) {
            --font;
            TEXTURE = labelTexture(marker.label, g_hyprviewConfig.scrolling.workspaceAnnotationColor, font);
            SIZE    = labelTextureSize(TEXTURE);
        }

        if (!TEXTURE || SIZE.x <= 0.0 || SIZE.y <= 0.0)
            continue;

        const CBox LABEL_BOX = {marker.box.x + (marker.box.w - SIZE.x) / 2.0, marker.box.y + (marker.box.h - SIZE.y) / 2.0, SIZE.x, SIZE.y};
        renderTextureLabel(TEXTURE, LABEL_BOX);
    }
}
bool CScrollOverview::windowEntryIsSelected(const SP<SWindowEntry>& entry) const {
    if (!entry || !entry->pWindow)
        return false;

    if (!g_hyprviewConfig.keyboard.enabled)
        return entry->highlight;

    return keyboardSelectedWindow && entry->pWindow == keyboardSelectedWindow;
}

bool CScrollOverview::windowEntryIsActive(const SP<SWindowEntry>& entry) const {
    if (!entry || !entry->pWindow)
        return false;

    const auto ACTIVE = Desktop::focusState()->window();
    return ACTIVE && entry->pWindow == ACTIVE;
}

void CScrollOverview::renderFocusIndicator(SP<SWindowEntry> entry) {
    if (!entry || (!entry->highlight && !windowEntryIsSelected(entry)) || entry->overviewBox.empty() || !pMonitor)
        return;

    CBox texbox = entry->overviewBox;
    texbox.scale(pMonitor->m_scale).round();

    const std::string INDICATOR = g_hyprviewConfig.scrolling.focusIndicator;
    if (INDICATOR == "active_border") {
        static auto       PACTIVECOL = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");

        const auto* const ACTIVE_BORDER = sc<Config::CGradientValueData*>(PACTIVECOL.ptr());
        const auto        BORDER_SIZE   = std::clamp(g_hyprviewConfig.scrolling.activeBorderSize, 1, 64);

        g_pHyprOpenGL->renderBorder(texbox, *ACTIVE_BORDER, {.round = 5, .borderSize = BORDER_SIZE});
        return;
    }

    g_pHyprOpenGL->renderRect(texbox, CHyprColor{g_hyprviewConfig.scrolling.hoverColor}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 5});
}

void CScrollOverview::renderActiveWindowIndicator(SP<SWindowEntry> entry) {
    if (!windowEntryIsActive(entry) || entry->overviewBox.empty() || !pMonitor)
        return;

    const std::string MODE = g_hyprviewConfig.scrolling.activeIndicator;
    if (MODE == "none")
        return;

    const auto COLOR = CHyprColor{g_hyprviewConfig.scrolling.activeIndicatorColor};
    if (COLOR.a <= 0.0)
        return;

    auto renderSolid = [this, COLOR](CBox box, int round = 2) {
        if (box.empty())
            return;

        box.scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(box, COLOR, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = round});
    };

    const double THICKNESS = std::clamp(sc<double>(g_hyprviewConfig.scrolling.activeBorderSize) * 0.75, 2.0, 10.0);
    const double MARGIN    = std::max(3.0, THICKNESS);

    if (MODE == "border") {
        renderSolid({entry->overviewBox.x, entry->overviewBox.y, entry->overviewBox.w, THICKNESS});
        renderSolid({entry->overviewBox.x, entry->overviewBox.y + entry->overviewBox.h - THICKNESS, entry->overviewBox.w, THICKNESS});
        renderSolid({entry->overviewBox.x, entry->overviewBox.y, THICKNESS, entry->overviewBox.h});
        renderSolid({entry->overviewBox.x + entry->overviewBox.w - THICKNESS, entry->overviewBox.y, THICKNESS, entry->overviewBox.h});
        return;
    }

    if (MODE == "underline") {
        renderSolid({entry->overviewBox.x + MARGIN, entry->overviewBox.y + entry->overviewBox.h - THICKNESS - MARGIN, entry->overviewBox.w - MARGIN * 2.0, THICKNESS},
                    sc<int>(THICKNESS));
        return;
    }

    if (MODE == "dot") {
        const double DOT_SIZE = std::clamp(std::min(entry->overviewBox.w, entry->overviewBox.h) * 0.08, 6.0, 14.0);
        CBox         dot      = {entry->overviewBox.x + entry->overviewBox.w - DOT_SIZE - MARGIN, entry->overviewBox.y + MARGIN, DOT_SIZE, DOT_SIZE};
        renderSolid(dot, sc<int>(DOT_SIZE));
        return;
    }

    const double LENGTH = std::clamp(std::min(entry->overviewBox.w, entry->overviewBox.h) * 0.18, 16.0, 42.0);
    const double X      = entry->overviewBox.x + MARGIN;
    const double Y      = entry->overviewBox.y + MARGIN;
    const double R      = entry->overviewBox.x + entry->overviewBox.w - MARGIN;
    const double B      = entry->overviewBox.y + entry->overviewBox.h - MARGIN;

    renderSolid({X, Y, LENGTH, THICKNESS});
    renderSolid({X, Y, THICKNESS, LENGTH});
    renderSolid({R - LENGTH, Y, LENGTH, THICKNESS});
    renderSolid({R - THICKNESS, Y, THICKNESS, LENGTH});
    renderSolid({X, B - THICKNESS, LENGTH, THICKNESS});
    renderSolid({X, B - LENGTH, THICKNESS, LENGTH});
    renderSolid({R - LENGTH, B - THICKNESS, LENGTH, THICKNESS});
    renderSolid({R - THICKNESS, B - LENGTH, THICKNESS, LENGTH});
}

void CScrollOverview::renderDropTargetFeedback() {
    if (!hasDropTarget())
        return;

    if (inputState.dropTarget.type == eDropTargetType::WORKSPACE_INSERTION)
        return;

    CBox texbox = shrinkBox(inputState.dropTarget.markerBox, 8.0);
    texbox.scale(pMonitor->m_scale).round();
    g_pHyprOpenGL->renderRect(texbox, CHyprColor{g_hyprviewConfig.scrolling.dropTargetColor}, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 8});
}
CBox CScrollOverview::draggedLiveWindowBox() const {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !inputState.windowDrag)
        return {};

    CBox box = inputState.windowDrag->sourceBox;
    if (const auto ENTRY = renderedWindowEntryForWindow(inputState.windowDrag->window.lock()); ENTRY && !ENTRY->overviewBox.empty())
        box = ENTRY->overviewBox;

    box.x = inputState.lastPosLocal.x - inputState.windowDrag->grabOffsetLocal.x;
    box.y = inputState.lastPosLocal.y - inputState.windowDrag->grabOffsetLocal.y;

    return box;
}

void CScrollOverview::fullRender() {
    renderOverviewLive(Time::steadyNow());
}

void CScrollOverview::renderWorkspaceShadows() {
    const auto STYLE_PROGRESS = overviewStyleProgress();
    const auto SHADOW_SIZE    = std::max(0.0, sc<double>(g_hyprviewConfig.scrolling.workspaceShadowSize) * STYLE_PROGRESS);
    if (SHADOW_SIZE == 0)
        return;

    auto SHADOW_COLOR = CHyprColor{g_hyprviewConfig.scrolling.workspaceShadowColor};
    SHADOW_COLOR.a *= STYLE_PROGRESS;
    if (SHADOW_COLOR.a <= 0.0)
        return;

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry || workspaceEntry->overviewBox.empty())
            continue;

        CBox shadowBox = workspaceEntry->overviewBox;
        shadowBox.x -= SHADOW_SIZE;
        shadowBox.y -= SHADOW_SIZE;
        shadowBox.w += SHADOW_SIZE * 2.0;
        shadowBox.h += SHADOW_SIZE * 2.0;
        shadowBox.scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(shadowBox, SHADOW_COLOR, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 10});
    }
}
