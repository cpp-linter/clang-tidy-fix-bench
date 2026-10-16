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
    // Always QtWebEnginePreview today: selectedPreviewEngine() only returns
    // PreviewEngine::WebView2 when isWebView2Available() is true, which it
    // never is yet. That branch will be added here once the COM backend
    // (a follow-on branch) lands.
    Q_ASSERT(selectedPreviewEngine() == PreviewEngine::QtWebEngine);
    return std::make_unique<QtWebEnginePreview>(parent);
}

} // namespace hungryeditor
