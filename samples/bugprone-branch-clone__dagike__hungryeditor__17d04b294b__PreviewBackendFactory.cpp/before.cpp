#include "preview/PreviewBackendFactory.h"

#include <QString>

#include "preview/QtWebEnginePreview.h"

namespace hungryeditor {

bool isWebView2Available()
{
    return false;
}

PreviewEngine selectedPreviewEngine()
{
    const QString override = qEnvironmentVariable("HUNGRYEDITOR_PREVIEW_ENGINE");
    if (override == QStringLiteral("webview2") && isWebView2Available()) {
        return PreviewEngine::WebView2;
    }
    return PreviewEngine::QtWebEngine;
}

std::unique_ptr<PreviewBackend> createPreviewBackend(QObject* parent)
{
    switch (selectedPreviewEngine()) {
    case PreviewEngine::WebView2:
        // Unreachable today: selectedPreviewEngine() only returns this when
        // isWebView2Available() is true, which it never is yet.
        break;
    case PreviewEngine::QtWebEngine:
        break;
    }
    return std::make_unique<QtWebEnginePreview>(parent);
}

} // namespace hungryeditor
