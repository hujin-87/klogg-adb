/*
 * Copyright (C) 2009, 2010, 2011, 2013, 2014 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTemporaryDir>

#include <QTranslator>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QFile;
class QProcess;
class QTimer;
struct AdbStep;
struct AdbSequenceContext;

#include "configuration.h"
#include "crawlerwidget.h"
#include "downloader.h"
#include "iconloader.h"
#include "pathline.h"
#include "quickfindmux.h"
#include "quickfindwidget.h"
#include "session.h"
#include "signalmux.h"
#include "tabbedcrawlerwidget.h"
#include "tabbedscratchpad.h"

class QAction;
class QActionGroup;
class Session;
class RecentFiles;
class HighlightersMenu;

// Main window of the application, creates menus, toolbar and
// the CrawlerWidget
class MainWindow : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow( WindowSession session );

    // Re-install the geometry stored in config file
    // (should be done before 'Widget::show()')
    void reloadGeometry();
    // Re-load the files from the previous session
    void reloadSession();
    // Loads the initial file (parameter passed or from config file)
    void loadInitialFile( QString fileName, bool followFile );

    void reTranslateUI();

    static int installLanguage( QString lang );

  public Q_SLOTS:
    // Load a file in a new tab (non-interactive)
    // (for use from e.g. IPC)
    void loadFileNonInteractive( const QString& file_name );

  protected:
    void closeEvent( QCloseEvent* event ) override;
    void changeEvent( QEvent* event ) override;

    // Drag and drop support
    void dragEnterEvent( QDragEnterEvent* event ) override;
    void dropEvent( QDropEvent* event ) override;

    bool event( QEvent* event ) override;

  private:
    enum class ActionInitiator { User, App };

  private Q_SLOTS:
    void open();
    void openFileFromRecent( QAction* action );
    void openFileFromFavorites( QAction* action );
    void switchToOpenedFile( QAction* action );
    void closeTab( ActionInitiator initiator );
    void closeAll( ActionInitiator initiator );
    void selectAll();
    void copy();
    void find();
    void clearLog();
    void copyFullPath();
    void openContainingFolder();
    void openInEditor();
    void openClipboard();
    void openUrl();
    void editHighlighters();
    void editPredefinedFilters( const QString& newFilter = {} );
    void options();
    void about();
    void aboutQt();
    void documentation();
    void showScratchPad();
    void sendToScratchpad( QString );
    void replaceDataInScratchpad( QString );
    void encodingChanged( QAction* action );
    void addToFavorites();
    void removeFromFavorites();
    void selectOpenedFile();
    void generateDump();
    void startAdbLogcat();
    void startAdbKmsg();
    void mergeOpenFilesOffline();
    void stopAdbLogcat();
    void quickSaveAdbLogcat();
    void killCameraAdb();
    void updateCameraProviderPid();
    void updateCameraDeviceCount();
    void applyCameraLabel( const QString& pid, const QString& deviceCount );
    void showAutoClosingSavedDialog( const QString& savePath );
    void onColorLabelsChanged( const ColorLabelsManager::QuickHighlightersCollection& labels );

    // Change the view settings
    void toggleOverviewVisibility( bool isVisible );
    void toggleMainLineNumbersVisibility( bool isVisible );
    void toggleFilteredLineNumbersVisibility( bool isVisible );

    // Change the follow mode checkbox and send the followSet signal down
    void changeFollowMode( bool follow );

    // Update the selection information displayed in the status bar.
    // Must be passed as the internal (starts at 0) line number.
    void lineNumberHandler( LineNumber startLine, LinesCount nLines, LineColumn startCol,
                            LineLength nSymbols );

    // Save current search in line edit as predefined filter.
    // Opens dialog with new entry.
    void newPredefinedFilterHandler( QString newFilter );

    // Instructs the widget to update the loading progress gauge
    void updateLoadingProgress( int progress );
    // Instructs the widget to display the 'normal' status bar,
    // without the progress gauge and with file info
    // or an error recovery when loading is finished
    void handleLoadingFinished( LoadingStatus status );

    // Update quick find searchable
    void handleFilteredViewChanged();

    // Close the tab with the passed index
    void closeTab( int index, ActionInitiator initiator );
    // Setup the tab with current index for view
    void currentTabChanged( int index );

    // Instructs the widget to change the pattern in the QuickFind widget
    // and confirm it.
    void changeQFPattern( const QString& newPattern );

  Q_SIGNALS:
    // Is emitted when new settings must be used
    void optionsChanged();
    // Is emitted when the 'follow' option is enabled/disabled
    void followSet( bool checked );
    // Is emitted when the 'text wrap' option is enabled/disabled
    void textWrapSet( bool checked );
    // Is emitted before the QuickFind box is activated,
    // to allow crawlers to get search in the right view.
    void enteringQuickFind();
    // Emitted when the quickfind bar is closed.
    void exitingQuickFind();

    void newWindow();
    void windowActivated();
    void windowClosed();
    void exitRequested();

  private:
    void createActions();
    void loadIcons();
    void createMenus();
    void createToolBars();
    void createTrayIcon();
    void readSettings();
    void writeSettings();
    bool loadFile( const QString& fileName, bool followFile = false );
    bool extractAndLoadFile( const QString& fileName );
    void openRemoteFile( const QUrl& url );
    void updateTitleBar( const QString& fileName );
    void addRecentFile( const QString& fileName );
    void updateRecentFileActions();
    void clearRecentFileActions();
    void updateFavoritesMenu();
    void updateOpenedFilesMenu();
    void updateHighlightersMenu();
    QString strippedName( const QString& fullFileName ) const;
    CrawlerWidget* currentCrawlerWidget() const;
    void displayQuickFindBar( QuickFindMux::QFDirection direction );
    void updateMenuBarFromDocument( const CrawlerWidget* crawler );
    void updateInfoLine();
    void showInfoLabels( bool show );
    void logScreenInfo( QScreen* screen );
    void removeFromFavorites( const QString& pathToRemove );
    void removeFromRecent( const QString& pathToRemove );
    void tryOpenClipboard( int tryTimes );
    void updateShortcuts();
    void cleanupAdbLogcatProcess();
    // Stop and clean up the F6 kernel-log capture. Independent of the F1/F2
    // logcat capture - does not touch the logcat process or its actions.
    void cleanupAdbKmsgProcess();
    // Shared capture routine for the ADB actions: runs `adb <captureArgs>` into
    // logPath and opens it in a follow-mode tab. When prepareLogcatBuffer is true
    // the device logcat ring buffer is enlarged and cleared beforehand. When
    // requireRoot is true, adbd is restarted as root first (needed to read
    // e.g. /dev/kmsg). When useKmsgSlot is true the capture uses the separate
    // F6 kernel-log process slot and does not touch the F1/F2 actions.
    void startAdbCapture( const QString& logPath, const QStringList& captureArgs,
                          bool prepareLogcatBuffer, bool requireRoot = false,
                          bool useKmsgSlot = false );
    // Convert the captured /dev/kmsg log (monotonic microseconds since boot) into
    // logcat threadtime format with wall-clock timestamps, write it to
    // kmsg.newT.txt and open it.
    void convertKmsgToLogcat();
    // Query the connected device for its boot wall-clock time (epoch seconds) and
    // timezone offset (seconds). Returns false if no device / query failed.
    bool queryDeviceBootTime( double& bootEpochSec, int& tzOffsetSec );
    // Ensure a target device is selected for the ADB commands. If several devices
    // are authorized and none is currently selected (or the selection is gone),
    // the user is prompted to pick one; the choice is remembered in adbSerial_ and
    // passed as `-s <serial>` to subsequent adb calls. Returns false when no device
    // is available or the user cancels the selection.
    bool ensureAdbDevice( const QString& adbExecutable );
    // Build an adb argument list, prepending `-s <serial>` when a target device
    // has been selected so the command is directed at that specific device.
    QStringList adbArgs( const QStringList& subCommand ) const;
    // Run a sequence of adb preparation commands asynchronously (one at a time),
    // showing a modal, cancelable busy dialog so the UI stays responsive instead
    // of blocking on waitForFinished(). Each step's validate() decides whether to
    // proceed; onSuccess() runs only if every step succeeds and the user does not
    // cancel. See AdbStep in mainwindow.cpp.
    void runAdbSequence( const QString& adbExecutable, const QString& busyLabel,
                         std::vector<AdbStep> steps, std::function<void()> onSuccess );
    // Internal helpers driving runAdbSequence's state machine over a heap context.
    void advanceAdbSequence( AdbSequenceContext* ctx );
    void finishAdbSequence( AdbSequenceContext* ctx, bool ok );
    // Convert a raw /dev/kmsg capture at srcPath into logcat threadtime format at
    // dstPath using the given boot time. Returns false on I/O error.
    bool convertKmsgFile( const QString& srcPath, const QString& dstPath, double bootEpochSec,
                          int tzOffsetSec );

    WindowSession session_;
    QString loadingFileName;

    std::array<QAction*, MAX_RECENT_FILES> recentFileActions;
    QActionGroup* recentFilesGroup;

    QMenu* fileMenu;
    QMenu* recentFilesMenu;
    QMenu* editMenu;
    QMenu* viewMenu;
    QMenu* toolsMenu;
    QMenu* favoritesMenu;
    HighlightersMenu* highlightersMenu;
    QMenu* openedFilesMenu;
    QMenu* helpMenu;

    PathLine* infoLine;
    QLabel* lineNbField;
    QLabel* sizeField;
    QLabel* dateField;
    QLabel* encodingField;
    std::vector<QAction*> infoToolbarSeparators;

    QToolBar* toolBar;

    QAction* newWindowAction;
    QAction* openAction;
    QAction* closeAction;
    QAction* closeAllAction;
    QAction* exitAction;
    QAction* copyAction;
    QAction* selectAllAction;
    QAction* goToLineAction;
    QAction* findAction;
    QAction* clearLogAction;
    QAction* copyPathToClipboardAction;
    QAction* openContainingFolderAction;
    QAction* openInEditorAction;
    QAction* openClipboardAction;
    QAction* openUrlAction;
    QAction* overviewVisibleAction;
    QAction* lineNumbersVisibleInMainAction;
    QAction* lineNumbersVisibleInFilteredAction;
    QAction* followAction;
    QAction* textWrapAction;
    QAction* reloadAction;
    QAction* stopAction;
    QAction* editHighlightersAction;
    QAction* optionsAction;
    QAction* showScratchPadAction;
    QAction* showDocumentationAction;
    QAction* aboutAction;
    QAction* aboutQtAction;
    QAction* predefinedFiltersDialogAction;
    QAction* reportIssueAction;
    QAction* joinDiscordAction;
    QAction* joinTelegramAction;
    QAction* generateDumpAction;
    QAction* adbLogcatStartAction;
    QAction* adbKmsgStartAction;
    QAction* adbOfflineMergeAction;
    QAction* adbLogcatStopAction;
    QAction* adbLogcatQuickSaveAction;
    QAction* adbKillCameraAction;
    QActionGroup* encodingGroup;
    QAction* addToFavoritesAction;
    QAction* addToFavoritesMenuAction;
    QAction* removeFromFavoritesAction;
    QAction* selectOpenFileAction;
    QAction* recentFilesCleanup;
    QActionGroup* favoritesGroup;
    QActionGroup* openedFilesGroup;
    QActionGroup* highlightersActionGroup = nullptr;

    std::map<QString, QShortcut*> shortcuts_;

    QSystemTrayIcon* trayIcon_;

    QIcon mainIcon_;

    IconLoader iconLoader_;

    // Multiplex signals to any of the CrawlerWidgets
    SignalMux signalMux_;

    static QTranslator mTranslator;
    static QTranslator mQtTranslator;

    // QuickFind widget
    QuickFindWidget quickFindWidget_;

    // Multiplex signals to/from the QuickFindWidget
    QuickFindMux quickFindMux_;

    // The main widget
    TabbedCrawlerWidget mainTabWidget_;

    TabbedScratchPad scratchPad_;

    QTemporaryDir tempDir_;

    QProcess* adbLogcatProcess_ = nullptr;
    QString adbLogcatFilePath_;
    QProcess* adbKmsgProcess_ = nullptr;
    QString adbKmsgFilePath_;
    // Serial of the device chosen when multiple are attached; empty means "use
    // the single/default device" (no `-s` flag). See ensureAdbDevice/adbArgs.
    QString adbSerial_;

    QProcess* cameraPidProcess_ = nullptr;
    QProcess* cameraDeviceProcess_ = nullptr;
    QTimer* cameraPidTimer_ = nullptr;

    // Latest camera.provider PID, held between the ps query and the follow-up
    // "dumpsys media.camera" device-count query so both land in one label.
    QString cameraProviderPid_;

    ColorLabelsManager::QuickHighlightersCollection globalColorLabels_;

    bool isMaximized_ = false;
    bool isCloseFromTray_ = false;

    std::once_flag screenChangesConnect_;
};

#endif
