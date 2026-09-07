#include "NotebookWindow.hpp"
#include "MainWindow.hpp"
#include "NotebookPrefs.hpp"
#include "TransportBar.hpp"
#include "TimelineWidget.hpp"
#include "WebBrowserPanel.hpp"
#include "EngineController.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <cmath>
#include <memory>

bool NotebookWindow::checkTimedTextForTest(QString* error) {
    const auto run = [this](const QString& script) {
        auto reply = std::make_shared<QVariant>();
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QPointer<QEventLoop> waiting(&loop);
        m_view->page()->runJavaScript(script, [reply, waiting](const QVariant& value) {
            *reply = value;
            if (waiting) waiting->quit();
        });
        deadline.start(5000);
        loop.exec();
        return *reply;
    };
    QElapsedTimer ready;
    ready.start();
    while (ready.elapsed() < 10000 &&
           !run(QStringLiteral("typeof currentNotebookLine==='function' && !!bridge")).toBool())
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!run(QStringLiteral("typeof currentNotebookLine==='function' && !!bridge")).toBool()) {
        if (error) *error = QStringLiteral("notebook WebChannel did not become ready");
        return false;
    }
    const QString original = run(QStringLiteral("editor.innerHTML")).toString();
    const auto originalCues = ui::notebookprefs::timedCues();
    const bool originalEnabled = ui::notebookprefs::timedTextEnabled();
    const double originalPosition = m_controller->positionSeconds();
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition && ok && error) *error = QString::fromUtf8(message);
        ok = ok && condition;
    };
    // WebEngine has a separate font loader: Qt registration alone is not
    // enough, and CSP must allow the embedded resources on this file page.
    run(QStringLiteral(R"JS(
        window.bundledFontsReady=false;
        Promise.all(['400','500','600','700','italic 400'].map(weight=>
            document.fonts.load(weight+' 16px Inter','Audio Аудио ё')
        )).then(faces=>window.bundledFontsReady=faces.every(list=>list.length>0));
    )JS"));
    ready.restart();
    while (ready.elapsed() < 5000 &&
           !run(QStringLiteral("window.bundledFontsReady")).toBool())
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    check(run(QStringLiteral("window.bundledFontsReady && getComputedStyle(editor).fontFamily.includes('Inter')")).toBool(),
          "notebook did not load bundled Inter faces");
    check(run(QStringLiteral(R"JS((()=>{
        editor.textContent='First plain line';editor.focus();
        const r=document.createRange();r.setStart(editor.firstChild,5);r.collapse(true);
        const s=getSelection();s.removeAllRanges();s.addRange(r);rememberSelection();
        return currentNotebookLine();
    })())JS")).toString() == QStringLiteral("First plain line"),
          "bare text in the first notebook line was not captured");
    check(run(QStringLiteral(R"JS((()=>{
        editor.innerHTML='<p><b>First</b><br><i>Second line</i></p>';
        const r=document.createRange();r.setStart(editor.querySelector('i').firstChild,3);r.collapse(true);
        const s=getSelection();s.removeAllRanges();s.addRange(r);rememberSelection();s.removeAllRanges();
        return currentNotebookLine();
    })())JS")).toString() == QStringLiteral("Second line"),
          "the caret line after a soft line break was lost when focus moved");
    check(run(QStringLiteral(R"JS((()=>{
        editor.innerHTML='<p>Selected words in a line</p>';
        const r=document.createRange();r.setStart(editor.firstChild.firstChild,0);r.setEnd(editor.firstChild.firstChild,14);
        const s=getSelection();s.removeAllRanges();s.addRange(r);rememberSelection();
        return currentNotebookLine();
    })())JS")).toString() == QStringLiteral("Selected words"),
          "an explicit notebook text selection did not survive");

    m_cueText->clear();
    m_tabs->setCurrentIndex(1);
    QElapsedTimer lineWait;
    lineWait.start();
    while (m_cueText->text().isEmpty() && lineWait.elapsed() < 2000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    check(m_cueText->text() == QStringLiteral("Selected words"),
          "switching to timed text did not pick up the editor selection");

    ui::notebookprefs::saveTimedCues({});
    reloadTimedTextTable();
    m_timedTextPlaybackButton->setChecked(false);
    m_controller->seekSeconds(12.25);
    m_cueText->setText(QStringLiteral("First cue"));
    QKeyEvent overrideEnter(QEvent::ShortcutOverride, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(m_cueText, &overrideEnter);
    check(overrideEnter.isAccepted(), "the cue field did not reserve Enter from transport shortcuts");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(m_cueText, &enter);
    auto cues = ui::notebookprefs::timedCues();
    check(cues.size() == 1 && std::abs(cues.front().seconds - 12.25) < 0.001 &&
              cues.front().text == QStringLiteral("First cue") && ui::notebookprefs::timedTextEnabled(),
          "binding a line did not save its time and enable timeline display");
    m_controller->seekSeconds(3.5);
    m_cueText->setText(QStringLiteral("Earlier cue"));
    captureCurrentLine();
    cues = ui::notebookprefs::timedCues();
    check(cues.size() == 2 && cues.front().text == QStringLiteral("Earlier cue") &&
              m_timedTextTable->currentRow() == 0 && m_setCueTime->isEnabled(),
          "sorting cues lost the selected line or edit actions");
    m_timedTextTable->item(0, 0)->setText(QStringLiteral("00:04,750"));
    cues = ui::notebookprefs::timedCues();
    check(cues.size() == 2 && std::abs(cues.front().seconds - 4.75) < 0.001,
          "a localized edited cue time did not save");
    m_seekCue->click();
    check(std::abs(m_controller->positionSeconds() - 4.75) < 0.001,
          "Go to time did not move the project playhead");
    m_controller->seekSeconds(14.0);
    updateTimedTextPosition();
    check(m_cuePreview->text().contains(QStringLiteral("First cue")),
          "current-line preview did not follow seeking");
    setSelectedCueToPlayhead();
    cues = ui::notebookprefs::timedCues();
    check(cues.size() == 2 && std::abs(cues.back().seconds - 14.0) < 0.001 &&
              cues.back().text == QStringLiteral("Earlier cue"),
          "updating a cue time did not keep its text after sorting");
    deleteSelectedCue();
    check(ui::notebookprefs::timedCues().size() == 1,
          "deleting a cue did not persist");

    const QString encoded = QString::fromLatin1(original.toUtf8().toBase64());
    run(QStringLiteral("editor.innerHTML=new TextDecoder().decode(Uint8Array.from(atob('%1'),c=>c.charCodeAt(0)));savedRange=null;sendContent();").arg(encoded));
    receiveContent(original);
    saveNow();
    ui::notebookprefs::saveTimedCues(originalCues);
    m_timedTextPlaybackButton->setChecked(originalEnabled);
    m_controller->seekSeconds(originalPosition);
    reloadTimedTextTable();
    updateTimedTextPosition();
    emit timedTextChanged();
    m_tabs->setCurrentIndex(0);
    m_view->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    check(ownsEditorFocus() && m_view->property("dawWebInput").toBool(),
          "the embedded notebook was not recognized as a text input");
    return ok;
}

bool MainWindow::checkNotebookForTest() {
    const bool wasVisible = ui::notebookprefs::visible();
    const bool wasDetached = QSettings().value("notebook/detached", false).toBool();
    setNotebookVisible(true);
    setNotebookDetached(false);
    QApplication::processEvents();
    auto* notebook = m_notebookWindow;
    auto* editor = notebook->findChild<QWebEngineView*>();
    auto* page = editor ? editor->page() : nullptr;
    bool ok = notebook && !notebook->isWindow() && notebook->parentWidget() == m_notebookContainer &&
              m_notebookContainer->isVisible();
    QString error;
    if (ok) ok = notebook->checkTimedTextForTest(&error);
    if (m_webPanel) ok = ok && !m_webPanel->ownsFocus();
    setNotebookDetached(true);
    QApplication::processEvents();
    ok = ok && m_notebookWindow == notebook && m_notebookDetachedWindow->isVisible() &&
         m_notebookContainer->isHidden() && notebook->parentWidget() == m_notebookDetachedWindow;
    m_notebookDetachedWindow->close();
    QApplication::processEvents();
    ok = ok && !ui::notebookprefs::visible() && !notebook->isVisible();
    setNotebookVisible(true);
    setNotebookDetached(false);
    QApplication::processEvents();
    ok = ok && notebook->parentWidget() == m_notebookContainer && notebook->isVisible() &&
         m_notebookDetachedWindow->isHidden() &&
         notebook->findChild<QWebEngineView*>() == editor && editor->page() == page;
    setNotebookVisible(false);
    ok = ok && m_notebookContainer->isHidden();
    if (!ok) std::fprintf(stderr, "Notebook check failed: %s\n", error.toUtf8().constData());
    setNotebookDetached(wasDetached);
    setNotebookVisible(wasVisible);
    return ok;
}

void MainWindow::openNotebookForShot() {
    setNotebookVisible(true, false);
    const QString mode = qEnvironmentVariable("DAW_SHOT_NOTEBOOK");
    if (mode.contains(QLatin1String("detached"))) setNotebookDetached(true);
    if (mode.contains(QLatin1String("timed"))) {
        if (auto* tabs = m_notebookWindow->findChild<QTabBar*>(QStringLiteral("NotebookTabs")))
            tabs->setCurrentIndex(1);
    }
}
