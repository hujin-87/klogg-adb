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

// This file implements MainWindow. It is responsible for creating and
// managing the menus, the toolbar, and the CrawlerWidget. It also
// load/save the settings on opening/closing of the app

#include "configuration.h"
#include "containers.h"
#include "log.h"
#include <QNetworkReply>
#include <cassert>
#include <exception>

#include <iterator>
#include <qaction.h>
#include <qapplication.h>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif // Q_OS_WIN

#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QListView>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cstring>
#include <QProgressDialog>
#include <QResource>
#include <QScreen>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QWindow>

#include "mainwindow.h"

#include "clipboard.h"
#include "crawlerwidget.h"
#include "decompressor.h"
#include "dispatch_to.h"
#include "downloader.h"
#include "encodings.h"
#include "favoritefiles.h"
#include "highlightersdialog.h"
#include "highlightersmenu.h"
#include "issuereporter.h"
#include "klogg_version.h"
#include "logger.h"
#include "mainwindowtext.h"
#include "openfilehelper.h"
#include "optionsdialog.h"
#include "predefinedfiltersdialog.h"
#include "progress.h"
#include "readablesize.h"
#include "recentfiles.h"
#include "sessioninfo.h"
#include "shortcuts.h"
#include "styles.h"
#include "tabbedcrawlerwidget.h"

namespace {

void signalCrawlerToFollowFile( CrawlerWidget* crawler_widget )
{
    dispatchToMainThread( [ crawler_widget ]() { crawler_widget->followSet( true ); } );
}

static constexpr auto ClipboardMaxTry = 5;

// Return the serial numbers of every authorized ("device" state) target listed
// by `adb devices`. Offline/unauthorized/no-permission entries are skipped.
QStringList adbAuthorizedDevices( const QString& adbExecutable )
{
    QProcess process;
    process.start( adbExecutable, QStringList() << QStringLiteral( "devices" ) );
    if ( !process.waitForFinished( 8000 ) ) {
        return {};
    }
    if ( process.exitCode() != 0 ) {
        return {};
    }
    QStringList serials;
    const auto out = QString::fromUtf8( process.readAllStandardOutput() );
    const auto lines = out.split( QLatin1Char( '\n' ) );
    // Skip the "List of devices attached" header line.
    for ( int i = 1; i < lines.size(); ++i ) {
        const QString line = lines.at( i ).trimmed();
        // Each entry is "<serial>\t<state>"; only "device" is usable.
        const auto fields = line.split( QLatin1Char( '\t' ), Qt::SkipEmptyParts );
        if ( fields.size() >= 2 && fields.at( 1 ).trimmed() == QLatin1String( "device" ) ) {
            serials << fields.at( 0 ).trimmed();
        }
    }
    return serials;
}

// Compile date derived from the __DATE__ macro ("Mmm dd yyyy"), formatted as
// "yy.m.d" without zero padding, e.g. "Jul  2 2026" -> "26.7.2".
QString kloggCompileDate()
{
    const char* d = __DATE__;
    static const char* const months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const QString monthStr = QString::fromLatin1( d, 3 );
    const int monthIndex = QString::fromLatin1( months ).indexOf( monthStr ) / 3 + 1;
    const int day = QString::fromLatin1( d + 4, 2 ).trimmed().toInt();
    const int year = QString::fromLatin1( d + 7, 4 ).toInt() % 100;
    return QStringLiteral( "%1.%2.%3" ).arg( year ).arg( monthIndex ).arg( day );
}

} // namespace

QTranslator MainWindow::mTranslator;
QTranslator MainWindow::mQtTranslator;

MainWindow::MainWindow( WindowSession session )
    : session_( std::move( session ) )
    , mainIcon_()
    , iconLoader_( this )
    , signalMux_()
    , quickFindMux_( session_.getQuickFindPattern() )
    , mainTabWidget_()
    , tempDir_( QDir::temp().filePath( "klogg_temp_" ) )
{
    createActions();
    createMenus();
    createToolBars();

    setAcceptDrops( true );

    // Default geometry
    const QRect geometry = QApplication::primaryScreen()->availableGeometry();
    setGeometry( geometry.x() + 20, geometry.y() + 40, geometry.width() - 140,
                 geometry.height() - 140 );

    mainIcon_.addFile( ":/images/hicolor/16x16/klogg.png" );
    // mainIcon_.addFile( ":/images/hicolor/24x24/klogg.png" );
    mainIcon_.addFile( ":/images/hicolor/32x32/klogg.png" );
    mainIcon_.addFile( ":/images/hicolor/48x48/klogg.png" );

    setWindowIcon( mainIcon_ );
    readSettings();

    createTrayIcon();

    // Connect the signals to the mux (they will be forwarded to the
    // "current" crawlerwidget

    // Send actions to the crawlerwidget
    signalMux_.connect( this, SIGNAL( followSet( bool ) ), SIGNAL( followSet( bool ) ) );
    signalMux_.connect( this, SIGNAL( textWrapSet( bool ) ), SIGNAL( textWrapSet( bool ) ) );
    signalMux_.connect( this, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
    signalMux_.connect( this, SIGNAL( enteringQuickFind() ), SLOT( enteringQuickFind() ) );
    signalMux_.connect( &quickFindWidget_, SIGNAL( close() ), SLOT( exitingQuickFind() ) );

    // Actions from the CrawlerWidget
    signalMux_.connect( SIGNAL( followModeChanged( bool ) ), this,
                        SLOT( changeFollowMode( bool ) ) );
    signalMux_.connect(
        SIGNAL( newSelection( LineNumber, LinesCount, LineColumn, LineLength ) ), this,
        SLOT( lineNumberHandler( LineNumber, LinesCount, LineColumn, LineLength ) ) );
    signalMux_.connect( SIGNAL( saveCurrentSearchAsPredefinedFilter( QString ) ), this,
                        SLOT( newPredefinedFilterHandler( QString ) ) );

    signalMux_.connect( SIGNAL( sendToScratchpad( QString ) ), this,
                        SLOT( sendToScratchpad( QString ) ) );

    signalMux_.connect( SIGNAL( replaceDataInScratchpad( QString ) ), this,
                        SLOT( replaceDataInScratchpad( QString ) ) );

    // Register for progress status bar
    signalMux_.connect( SIGNAL( loadingProgressed( int ) ), this,
                        SLOT( updateLoadingProgress( int ) ) );
    signalMux_.connect( SIGNAL( loadingFinished( LoadingStatus ) ), this,
                        SLOT( handleLoadingFinished( LoadingStatus ) ) );

    signalMux_.connect( SIGNAL( filteredViewChanged() ), this,
                        SLOT( handleFilteredViewChanged() ) );

    // Configure the main tabbed widget
    mainTabWidget_.setDocumentMode( true );
    mainTabWidget_.setMovable( true );
    // mainTabWidget_.setTabShape( QTabWidget::Triangular );
    mainTabWidget_.setTabsClosable( true );

    scratchPad_.setWindowIcon( mainIcon_ );
    scratchPad_.setWindowTitle( tr( "klogg - scratchpad" ) );

    connect( &mainTabWidget_, &TabbedCrawlerWidget::tabCloseRequested, this,
             [ this ]( int index ) { this->closeTab( index, ActionInitiator::User ); } );
    connect( &mainTabWidget_, &TabbedCrawlerWidget::currentChanged, this,
             &MainWindow::currentTabChanged );

    // Establish the QuickFindWidget and mux ( to send requests from the
    // QFWidget to the right window )
    connect( &quickFindWidget_, SIGNAL( patternConfirmed( const QString&, bool, bool ) ),
             &quickFindMux_, SLOT( confirmPattern( const QString&, bool, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( patternUpdated( const QString&, bool, bool ) ),
             &quickFindMux_, SLOT( setNewPattern( const QString&, bool, bool ) ) );
    connect( &quickFindWidget_, SIGNAL( cancelSearch() ), &quickFindMux_, SLOT( cancelSearch() ) );
    connect( &quickFindWidget_, SIGNAL( searchForward() ), &quickFindMux_,
             SLOT( searchForward() ) );
    connect( &quickFindWidget_, SIGNAL( searchBackward() ), &quickFindMux_,
             SLOT( searchBackward() ) );
    connect( &quickFindWidget_, SIGNAL( searchNext() ), &quickFindMux_, SLOT( searchNext() ) );

    // QuickFind changes coming from the views
    connect( &quickFindMux_, SIGNAL( patternChanged( const QString& ) ), this,
             SLOT( changeQFPattern( const QString& ) ) );
    connect( &quickFindMux_, SIGNAL( notify( const QFNotification& ) ), &quickFindWidget_,
             SLOT( notify( const QFNotification& ) ) );
    connect( &quickFindMux_, SIGNAL( clearNotification() ), &quickFindWidget_,
             SLOT( clearNotification() ) );

    // Construct the QuickFind bar
    quickFindWidget_.hide();

    QWidget* central_widget = new QWidget();
    auto* main_layout = new QVBoxLayout();
    main_layout->setContentsMargins( 0, 0, 0, 0 );
    main_layout->addWidget( &mainTabWidget_ );
    main_layout->addWidget( &quickFindWidget_ );
    central_widget->setLayout( main_layout );

    setCentralWidget( central_widget );

    updateTitleBar( "" );
    loadIcons();
    reTranslateUI();
}

void MainWindow::reloadGeometry()
{
    QByteArray geometry;

    session_.restoreGeometry( &geometry );
    restoreGeometry( geometry );
}

void MainWindow::reloadSession()
{
    const auto& config = Configuration::get();
    // Restored files follow by default so live logs tail automatically,
    // regardless of the persisted "follow on load" preference (matches the
    // behaviour of freshly opened files in loadFile()).
    const auto followFileOnLoad = config.anyFileWatchEnabled();

    int current_file_index = -1;
    const auto openedFiles
        = session_.restore( [] { return new CrawlerWidget(); }, &current_file_index );

    for ( const auto& open_file : openedFiles ) {
        QString file_name = { open_file.first };
        auto* crawler_widget = static_cast<CrawlerWidget*>( open_file.second );

        if ( crawler_widget ) {
            mainTabWidget_.addCrawler( crawler_widget, file_name );

            connect( crawler_widget, &CrawlerWidget::colorLabelsChanged, this,
                     &MainWindow::onColorLabelsChanged );

            if ( followFileOnLoad ) {
                signalCrawlerToFollowFile( crawler_widget );
            }
        }
    }

    if ( current_file_index >= 0 ) {
        mainTabWidget_.setCurrentIndex( current_file_index );

        if ( followFileOnLoad ) {
            followAction->setChecked( true );
        }
    }

    updateOpenedFilesMenu();
}

void MainWindow::loadInitialFile( QString fileName, bool followFile )
{
    LOG_DEBUG << "loadInitialFile";

    // Is there a file passed as argument?
    if ( !fileName.isEmpty() ) {
        loadFile( fileName, followFile );
    }
}

void MainWindow::reTranslateUI()
{
    using namespace klogg::mainwindow;
    // menu
    auto transMenu = []( const char* text ) -> auto {
        return QApplication::translate( "klogg::mainwindow::menu", text );
    };
    fileMenu->setTitle( transMenu( menu::fileTitle ) );
    editMenu->setTitle( transMenu( menu::editTitle ) );
    viewMenu->setTitle( transMenu( menu::viewTitle ) );
    openedFilesMenu->setTitle( transMenu( menu::openedFilesTitle ) );
    toolsMenu->setTitle( transMenu( menu::toolsTitle ) );
    highlightersMenu->setTitle( transMenu( menu::highlightersTitle ) );
    favoritesMenu->setTitle( transMenu( menu::favoritesTitle ) );
    helpMenu->setTitle( transMenu( menu::helpTitle ) );

    // toolbar
    toolBar->setToolTip(
        QApplication::translate( "klogg::mainwindow::toolbar", toolbar::toolbarTitle ) );

    // action
    auto transAction = []( const char* text ) -> auto {
        return QApplication::translate( "klogg::mainwindow::action", text );
    };
    newWindowAction->setText( transAction( action::newWindowText ) );
    newWindowAction->setStatusTip( transAction( action::newWindowStatusTip ) );

    openAction->setText( transAction( action::openText ) );
    openAction->setStatusTip( transAction( action::openStatusTip ) );

    recentFilesCleanup->setText( transAction( action::recentFilesCleanupText ) );

    closeAction->setText( transAction( action::closeText ) );
    closeAction->setStatusTip( transAction( action::closeStatusTip ) );

    closeAllAction->setText( transAction( action::closeAllText ) );
    closeAllAction->setStatusTip( transAction( action::closeAllStatusTip ) );

    exitAction->setText( transAction( action::exitText ) );
    exitAction->setStatusTip( transAction( action::exitStatusTip ) );

    copyAction->setText( transAction( action::copyText ) );
    copyAction->setStatusTip( transAction( action::copyStatusTip ) );

    selectAllAction->setText( transAction( action::selectAllText ) );
    selectAllAction->setStatusTip( transAction( action::selectAllStatusTip ) );

    goToLineAction->setText( transAction( action::goToLineText ) );
    goToLineAction->setStatusTip( transAction( action::goToLineStatusTip ) );

    findAction->setText( transAction( action::findText ) );
    findAction->setStatusTip( transAction( action::findStatusTip ) );

    clearLogAction->setText( transAction( action::clearLogText ) );
    clearLogAction->setStatusTip( transAction( action::clearLogStatusTip ) );

    openContainingFolderAction->setText( transAction( action::openContainingFolderText ) );
    openContainingFolderAction->setStatusTip(
        transAction( action::openContainingFolderStatusTip ) );

    openInEditorAction->setText( transAction( action::openInEditorText ) );
    openInEditorAction->setStatusTip( transAction( action::openInEditorStatusTip ) );

    copyPathToClipboardAction->setText( transAction( action::copyPathToClipboardText ) );
    copyPathToClipboardAction->setStatusTip( transAction( action::copyPathToClipboardStatusTip ) );

    openClipboardAction->setText( transAction( action::openClipboardText ) );
    openClipboardAction->setStatusTip( transAction( action::openClipboardStatusTip ) );

    openUrlAction->setText( transAction( action::openUrlText ) );
    openUrlAction->setStatusTip( transAction( action::openUrlStatusTip ) );

    overviewVisibleAction->setText( transAction( action::overviewVisibleText ) );

    lineNumbersVisibleInMainAction->setText( transAction( action::lineNumbersVisibleInMainText ) );
    lineNumbersVisibleInFilteredAction->setText(
        transAction( action::lineNumbersVisibleInFilteredText ) );

    followAction->setText( transAction( action::followText ) );
    textWrapAction->setText( transAction( action::wrapText ) );
    reloadAction->setText( transAction( action::reloadText ) );
    stopAction->setText( transAction( action::stopText ) );

    optionsAction->setText( transAction( action::optionsText ) );
    optionsAction->setStatusTip( transAction( action::optionsStatusTip ) );

    editHighlightersAction->setText( transAction( action::editHighlightersText ) );
    editHighlightersAction->setStatusTip( transAction( action::editHighlightersStatusTip ) );

    showDocumentationAction->setText( transAction( action::showDocumentationText ) );
    showDocumentationAction->setStatusTip( transAction( action::showDocumentationStatusTip ) );

    aboutAction->setText( transAction( action::aboutText ) );
    aboutAction->setStatusTip( transAction( action::aboutStatusTip ) );

    aboutQtAction->setText( transAction( action::aboutQtText ) );
    aboutQtAction->setStatusTip( transAction( action::aboutQtStatusTip ) );

    reportIssueAction->setText( transAction( action::reportIssueText ) );
    reportIssueAction->setStatusTip( transAction( action::reportIssueStatusTip ) );

    joinDiscordAction->setText( transAction( action::joinDiscordText ) );
    joinDiscordAction->setStatusTip( transAction( action::joinDiscordStatusTip ) );

    joinTelegramAction->setText( transAction( action::joinTelegramText ) );
    joinTelegramAction->setStatusTip( transAction( action::joinTelegramStatusTip ) );

    generateDumpAction->setText( transAction( action::generateDumpText ) );
    generateDumpAction->setStatusTip( transAction( action::generateDumpStatusTip ) );

    showScratchPadAction->setText( transAction( action::showScratchPadText ) );
    showScratchPadAction->setStatusTip( transAction( action::showScratchPadStatusTip ) );

    adbLogcatStartAction->setText( transAction( action::adbLogcatStartText ) );
    adbLogcatStartAction->setStatusTip( transAction( action::adbLogcatStartStatusTip ) );
    adbLogcatStopAction->setText( transAction( action::adbLogcatStopText ) );
    adbLogcatStopAction->setStatusTip( transAction( action::adbLogcatStopStatusTip ) );

    auto curFavoritesIconText = addToFavoritesAction->data().toBool()
                                    ? transAction( action::addToFavoritesText )
                                    : transAction( action::removeFromFavoritesText );
    addToFavoritesAction->setText( curFavoritesIconText );
    addToFavoritesMenuAction->setText( transAction( action::addToFavoritesText ) );

    removeFromFavoritesAction->setText( transAction( action::removeFromFavoritesText ) );

    selectOpenFileAction->setText( transAction( action::selectOpenFileText ) );

    predefinedFiltersDialogAction->setText( transAction( action::predefinedFiltersDialogText ) );
    predefinedFiltersDialogAction->setStatusTip(
        transAction( action::predefinedFiltersDialogStatusTip ) );

    // trayIcon
    trayIcon_->setToolTip( QApplication::translate( "klogg::mainwindow::trayicon",
                                                    klogg::mainwindow::trayicon::trayiconTip ) );
}

int MainWindow::installLanguage( QString lang )
{
    if ( lang.isEmpty() ) {
        return -1;
    }

    QApplication::removeTranslator( &mTranslator );
    QApplication::removeTranslator( &mQtTranslator );

    QString qtPath( ":/i18n/qt_" + lang + ".qm" );
    QResource qtTranslations( qtPath );
    if ( !mQtTranslator.load( qtTranslations.data(), (int)qtTranslations.size() ) ) {
        LOG_ERROR << "load fail";
        return -1;
    }
    if ( !QApplication::installTranslator( &mQtTranslator ) ) {
        LOG_ERROR << "install fail";
        return -1;
    }

    QString appPath( ":/i18n/" + lang + ".qm" );
    QResource appTranslations( appPath );
    if ( !mTranslator.load( appTranslations.data(), (int)appTranslations.size() ) ) {
        LOG_ERROR << "load fail";
        return -1;
    }
    if ( !QApplication::installTranslator( &mTranslator ) ) {
        LOG_ERROR << "install fail";
        return -1;
    }

    return 0;
}

// Menu actions
void MainWindow::createActions()
{
    const auto& config = Configuration::get();
    const auto shortcuts = config.shortcuts();

    using namespace klogg::mainwindow;

    newWindowAction = new QAction( tr( action::newWindowText ), this );
    newWindowAction->setStatusTip( tr( action::newWindowStatusTip ) );
    connect( newWindowAction, &QAction::triggered, [ = ] { Q_EMIT newWindow(); } );
    newWindowAction->setVisible( config.allowMultipleWindows() );

    openAction = new QAction( tr( action::openText ), this );
    openAction->setStatusTip( tr( action::openStatusTip ) );
    connect( openAction, &QAction::triggered, [ this ]( auto ) { this->open(); } );

    recentFilesCleanup = new QAction( tr( action::recentFilesCleanupText ), this );
    connect( recentFilesCleanup, &QAction::triggered, this,
             [ this ]( auto ) { this->clearRecentFileActions(); } );

    closeAction = new QAction( tr( action::closeText ), this );
    closeAction->setStatusTip( tr( action::closeStatusTip ) );
    connect( closeAction, &QAction::triggered, this,
             [ this ]( auto ) { this->closeTab( ActionInitiator::User ); } );

    closeAllAction = new QAction( tr( action::closeAllText ), this );
    closeAllAction->setStatusTip( tr( action::closeAllStatusTip ) );
    connect( closeAllAction, &QAction::triggered, this,
             [ this ]( auto ) { this->closeAll( ActionInitiator::User ); } );

    recentFilesGroup = new QActionGroup( this );
    connect( recentFilesGroup, &QActionGroup::triggered, this, &MainWindow::openFileFromRecent );
    for ( auto i = 0u; i < recentFileActions.size(); ++i ) {
        recentFileActions[ i ] = new QAction( this );
        connect( recentFileActions[ i ], &QAction::hovered, [ this, a = recentFileActions[ i ] ]() {
            QToolTip::showText( QCursor::pos(), a->toolTip(), this );
        } );
        recentFileActions[ i ]->setVisible( false );
        recentFileActions[ i ]->setActionGroup( recentFilesGroup );
    }

    exitAction = new QAction( tr( action::exitText ), this );
    exitAction->setStatusTip( tr( action::exitStatusTip ) );
    connect( exitAction, &QAction::triggered, this, &MainWindow::exitRequested );

    copyAction = new QAction( tr( action::copyText ), this );
    copyAction->setStatusTip( tr( action::copyStatusTip ) );
    connect( copyAction, &QAction::triggered, this, [ this ]( auto ) { this->copy(); } );

    selectAllAction = new QAction( tr( action::selectAllText ), this );
    selectAllAction->setStatusTip( tr( action::selectAllStatusTip ) );
    connect( selectAllAction, &QAction::triggered, this, [ this ]( auto ) { this->selectAll(); } );

    goToLineAction = new QAction( tr( action::goToLineText ), this );
    goToLineAction->setStatusTip( tr( action::goToLineStatusTip ) );
    signalMux_.connect( goToLineAction, SIGNAL( triggered() ), SLOT( goToLine() ) );

    findAction = new QAction( tr( action::findText ), this );
    findAction->setStatusTip( tr( action::findStatusTip ) );
    connect( findAction, &QAction::triggered, this, [ this ]( auto ) { this->find(); } );

    clearLogAction = new QAction( tr( action::clearLogText ), this );
    clearLogAction->setStatusTip( tr( action::clearLogStatusTip ) );
    connect( clearLogAction, &QAction::triggered, this, [ this ]( auto ) { this->clearLog(); } );

    openContainingFolderAction = new QAction( tr( action::openContainingFolderText ), this );
    openContainingFolderAction->setStatusTip( tr( action::openContainingFolderStatusTip ) );
    connect( openContainingFolderAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openContainingFolder(); } );

    openInEditorAction = new QAction( tr( action::openInEditorText ), this );
    openInEditorAction->setStatusTip( tr( action::openInEditorStatusTip ) );
    connect( openInEditorAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openInEditor(); } );

    copyPathToClipboardAction = new QAction( tr( action::copyPathToClipboardText ), this );
    copyPathToClipboardAction->setStatusTip( tr( action::copyPathToClipboardStatusTip ) );
    connect( copyPathToClipboardAction, &QAction::triggered, this,
             [ this ]( auto ) { this->copyFullPath(); } );

    openClipboardAction = new QAction( tr( action::openClipboardText ), this );
    openClipboardAction->setStatusTip( tr( action::openClipboardStatusTip ) );
    connect( openClipboardAction, &QAction::triggered, this,
             [ this ]( auto ) { this->openClipboard(); } );

    openUrlAction = new QAction( tr( action::openUrlText ), this );
    openUrlAction->setStatusTip( tr( action::openUrlStatusTip ) );
    connect( openUrlAction, &QAction::triggered, this, [ this ]( auto ) { this->openUrl(); } );

    overviewVisibleAction = new QAction( tr( action::overviewVisibleText ), this );
    overviewVisibleAction->setCheckable( true );
    overviewVisibleAction->setChecked( config.isOverviewVisible() );
    connect( overviewVisibleAction, &QAction::toggled, this,
             &MainWindow::toggleOverviewVisibility );

    lineNumbersVisibleInMainAction
        = new QAction( tr( action::lineNumbersVisibleInMainText ), this );
    lineNumbersVisibleInMainAction->setCheckable( true );
    lineNumbersVisibleInMainAction->setChecked( config.mainLineNumbersVisible() );
    connect( lineNumbersVisibleInMainAction, &QAction::toggled, this,
             &MainWindow::toggleMainLineNumbersVisibility );

    lineNumbersVisibleInFilteredAction
        = new QAction( tr( action::lineNumbersVisibleInFilteredText ), this );
    lineNumbersVisibleInFilteredAction->setCheckable( true );
    lineNumbersVisibleInFilteredAction->setChecked( config.filteredLineNumbersVisible() );
    connect( lineNumbersVisibleInFilteredAction, &QAction::toggled, this,
             &MainWindow::toggleFilteredLineNumbersVisibility );

    followAction = new QAction( tr( action::followText ), this );
    followAction->setCheckable( true );
    followAction->setEnabled( config.anyFileWatchEnabled() );
    connect( followAction, &QAction::toggled, this, &MainWindow::followSet );

    textWrapAction = new QAction( tr( action::wrapText ), this );
    textWrapAction->setCheckable( true );
    textWrapAction->setEnabled( true );
    connect( textWrapAction, &QAction::toggled, this, &MainWindow::textWrapSet );

    reloadAction = new QAction( tr( action::reloadText ), this );
    signalMux_.connect( reloadAction, SIGNAL( triggered() ), SLOT( reload() ) );

    stopAction = new QAction( tr( action::stopText ), this );
    stopAction->setEnabled( true );
    signalMux_.connect( stopAction, SIGNAL( triggered() ), SLOT( stopLoading() ) );

    optionsAction = new QAction( tr( action::optionsText ), this );
    optionsAction->setMenuRole( QAction::NoRole );
    optionsAction->setStatusTip( tr( action::optionsStatusTip ) );
    connect( optionsAction, &QAction::triggered, this, [ this ]( auto ) { this->options(); } );

    editHighlightersAction = new QAction( tr( action::editHighlightersText ), this );
    editHighlightersAction->setMenuRole( QAction::NoRole );
    editHighlightersAction->setStatusTip( tr( action::editHighlightersStatusTip ) );
    connect( editHighlightersAction, &QAction::triggered, this,
             [ this ]( auto ) { this->editHighlighters(); } );

    showDocumentationAction = new QAction( tr( action::showDocumentationText ), this );
    showDocumentationAction->setStatusTip( tr( action::showDocumentationStatusTip ) );
    connect( showDocumentationAction, &QAction::triggered, this,
             [ this ]( auto ) { this->documentation(); } );

    aboutAction = new QAction( tr( action::aboutText ), this );
    aboutAction->setStatusTip( tr( action::aboutStatusTip ) );
    connect( aboutAction, &QAction::triggered, this, [ this ]( auto ) { this->about(); } );

    aboutQtAction = new QAction( tr( action::aboutQtText ), this );
    aboutQtAction->setStatusTip( tr( action::aboutQtStatusTip ) );
    connect( aboutQtAction, &QAction::triggered, this, [ this ]( auto ) { this->aboutQt(); } );

    reportIssueAction = new QAction( tr( action::reportIssueText ), this );
    reportIssueAction->setStatusTip( tr( action::reportIssueStatusTip ) );
    connect( reportIssueAction, &QAction::triggered, this,
             []( auto ) { IssueReporter::reportIssue( IssueTemplate::Bug ); } );

    joinDiscordAction = new QAction( tr( action::joinDiscordText ), this );
    joinDiscordAction->setStatusTip( tr( action::joinDiscordStatusTip ) );
    connect( joinDiscordAction, &QAction::triggered, this, []( auto ) {
        QUrl url( "https://discord.gg/DruNyQftzB" );
        QDesktopServices::openUrl( url );
    } );

    joinTelegramAction = new QAction( tr( action::joinTelegramText ), this );
    joinTelegramAction->setStatusTip( tr( action::joinTelegramStatusTip ) );
    connect( joinTelegramAction, &QAction::triggered, this, []( auto ) {
        QUrl url( "https://t.me/joinchat/JeIBxstIfp4xZTk6" );
        QDesktopServices::openUrl( url );
    } );

    generateDumpAction = new QAction( tr( action::generateDumpText ), this );
    generateDumpAction->setStatusTip( tr( action::generateDumpStatusTip ) );
    connect( generateDumpAction, &QAction::triggered, this,
             [ this ]( auto ) { this->generateDump(); } );

    showScratchPadAction = new QAction( tr( action::showScratchPadText ), this );
    showScratchPadAction->setStatusTip( tr( action::showScratchPadStatusTip ) );
    connect( showScratchPadAction, &QAction::triggered, this,
             [ this ]( auto ) { this->showScratchPad(); } );

    adbLogcatStartAction = new QAction( tr( action::adbLogcatStartText ), this );
    adbLogcatStartAction->setStatusTip( tr( action::adbLogcatStartStatusTip ) );
    adbLogcatStartAction->setShortcut( QKeySequence( Qt::Key_F1 ) );
    connect( adbLogcatStartAction, &QAction::triggered, this,
             [ this ]( auto ) { this->startAdbLogcat(); } );

    adbLogcatStopAction = new QAction( tr( action::adbLogcatStopText ), this );
    adbLogcatStopAction->setStatusTip( tr( action::adbLogcatStopStatusTip ) );
    adbLogcatStopAction->setShortcut( QKeySequence( Qt::Key_F2 ) );
    adbLogcatStopAction->setEnabled( false );
    connect( adbLogcatStopAction, &QAction::triggered, this,
             [ this ]( auto ) { this->stopAdbLogcat(); } );

    adbLogcatQuickSaveAction = new QAction( tr( "Quick Save(F3)" ), this );
    adbLogcatQuickSaveAction->setStatusTip( tr( "Save current log with timestamp" ) );
    adbLogcatQuickSaveAction->setShortcut( QKeySequence( Qt::Key_F3 ) );
    connect( adbLogcatQuickSaveAction, &QAction::triggered, this,
             [ this ]( auto ) { this->quickSaveAdbLogcat(); } );

    adbKillCameraAction = new QAction( tr( "Kill Camera(F4)" ), this );
    adbKillCameraAction->setStatusTip( tr( "Run adb root and kill camera processes" ) );
    adbKillCameraAction->setShortcut( QKeySequence( Qt::Key_F4 ) );
    connect( adbKillCameraAction, &QAction::triggered, this,
             [ this ]( auto ) { this->killCameraAdb(); } );

    adbKmsgStartAction = new QAction( tr( "Kernel Log(F6)" ), this );
    adbKmsgStartAction->setStatusTip(
        tr( "Stop current capture, then capture adb shell cat /dev/kmsg" ) );
    adbKmsgStartAction->setShortcut( QKeySequence( Qt::Key_F6 ) );
    connect( adbKmsgStartAction, &QAction::triggered, this,
             [ this ]( auto ) { this->startAdbKmsg(); } );

    adbOfflineMergeAction = new QAction( tr( "Merge Offline(F9)" ), this );
    adbOfflineMergeAction->setStatusTip(
        tr( "Merge all open files into offline.txt ordered by timestamp" ) );
    adbOfflineMergeAction->setShortcut( QKeySequence( Qt::Key_F9 ) );
    connect( adbOfflineMergeAction, &QAction::triggered, this,
             [ this ]( auto ) { this->mergeOpenFilesOffline(); } );

    // Poll the camera provider PID every 3 seconds and mirror it in the F4 label.
    cameraPidTimer_ = new QTimer( this );
    cameraPidTimer_->setInterval( 3000 );
    connect( cameraPidTimer_, &QTimer::timeout, this,
             [ this ]() { this->updateCameraProviderPid(); } );
    cameraPidTimer_->start();
    updateCameraProviderPid();

    encodingGroup = new QActionGroup( this );
    connect( encodingGroup, &QActionGroup::triggered, this, &MainWindow::encodingChanged );

    favoritesGroup = new QActionGroup( this );
    connect( favoritesGroup, &QActionGroup::triggered, this, &MainWindow::openFileFromFavorites );

    openedFilesGroup = new QActionGroup( this );
    connect( openedFilesGroup, &QActionGroup::triggered, this, &MainWindow::switchToOpenedFile );

    addToFavoritesAction = new QAction( tr( action::addToFavoritesText ), this );
    addToFavoritesAction->setData( true );
    connect( addToFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->addToFavorites(); } );

    addToFavoritesMenuAction = new QAction( tr( action::addToFavoritesText ), this );
    connect( addToFavoritesMenuAction, &QAction::triggered, this,
             [ this ]( auto ) { this->addToFavorites(); } );

    removeFromFavoritesAction = new QAction( tr( action::removeFromFavoritesText ), this );
    connect( removeFromFavoritesAction, &QAction::triggered, this,
             [ this ]( auto ) { this->removeFromFavorites(); } );

    selectOpenFileAction = new QAction( tr( action::selectOpenFileText ), this );
    connect( selectOpenFileAction, &QAction::triggered, this,
             [ this ]( auto ) { this->selectOpenedFile(); } );

    predefinedFiltersDialogAction = new QAction( tr( action::predefinedFiltersDialogText ), this );
    predefinedFiltersDialogAction->setStatusTip( tr( action::predefinedFiltersDialogStatusTip ) );
    connect( predefinedFiltersDialogAction, &QAction::triggered, this,
             [ this ]( auto ) { this->editPredefinedFilters(); } );

    updateShortcuts();
}

void MainWindow::updateShortcuts()
{
    const auto& config = Configuration::get();
    const auto shortcuts = config.shortcuts();

    for ( auto& shortcut : shortcuts_ ) {
        shortcut.second->deleteLater();
    }

    shortcuts_.clear();
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowOpenQfForward,
                                      [ this ] { displayQuickFindBar( QuickFindMux::Forward ); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowOpenQfBackward,
                                      [ this ] { displayQuickFindBar( QuickFindMux::Backward ); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowFocusSearchInput, [ this ] {
                                          if ( auto crawler = currentCrawlerWidget() ) {
                                              crawler->focusSearchEdit();
                                          }
                                      } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowFullScreen,
                                      [ this ] { this->showFullScreen(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowMax,
                                      [ this ] { this->showMaximized(); } );
    ShortcutAction::registerShortcut( shortcuts, shortcuts_, this, Qt::WindowShortcut,
                                      ShortcutAction::MainWindowMin,
                                      [ this ] { this->showMinimized(); } );

    auto setShortcuts = [ &shortcuts ]( auto* action, const auto& actionName ) {
        action->setShortcuts( ShortcutAction::shortcutKeys( actionName, shortcuts ) );
    };

    setShortcuts( newWindowAction, ShortcutAction::MainWindowNewWindow );
    setShortcuts( openAction, ShortcutAction::MainWindowOpenFile );
    setShortcuts( closeAction, ShortcutAction::MainWindowCloseFile );
    setShortcuts( closeAllAction, ShortcutAction::MainWindowCloseAll );
    setShortcuts( exitAction, ShortcutAction::MainWindowQuit );
    setShortcuts( copyAction, ShortcutAction::MainWindowCopy );
    setShortcuts( selectAllAction, ShortcutAction::MainWindowSelectAll );
    setShortcuts( findAction, ShortcutAction::MainWindowOpenQf );
    setShortcuts( clearLogAction, ShortcutAction::MainWindowClearFile );
    setShortcuts( openContainingFolderAction, ShortcutAction::MainWindowOpenContainingFolder );
    setShortcuts( openInEditorAction, ShortcutAction::MainWindowOpenInEditor );
    setShortcuts( copyPathToClipboardAction, ShortcutAction::MainWindowCopyPathToClipboard );
    setShortcuts( openClipboardAction, ShortcutAction::MainWindowOpenFromClipboard );
    setShortcuts( openUrlAction, ShortcutAction::MainWindowOpenFromUrl );
    setShortcuts( followAction, ShortcutAction::MainWindowFollowFile );
    setShortcuts( textWrapAction, ShortcutAction::MainWindowTextWrap );
    setShortcuts( reloadAction, ShortcutAction::MainWindowReload );
    setShortcuts( stopAction, ShortcutAction::MainWindowStop );
    setShortcuts( showScratchPadAction, ShortcutAction::MainWindowScratchpad );
    setShortcuts( selectOpenFileAction, ShortcutAction::MainWindowSelectOpenFile );
    setShortcuts( goToLineAction, ShortcutAction::LogViewJumpToLine );
    setShortcuts( optionsAction, ShortcutAction::MainWindowPreference );
}

void MainWindow::loadIcons()
{
    openAction->setIcon( iconLoader_.load( "icons8-open-file" ) );
    stopAction->setIcon( iconLoader_.load( "icons8-delete" ) );
    reloadAction->setIcon( iconLoader_.load( "icons8-restore-page" ) );
    followAction->setIcon( iconLoader_.load( "icons8-fast-forward" ) );
    showScratchPadAction->setIcon( iconLoader_.load( "icons8-create" ) );
    addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star" ) );
    addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star" ) );
}

void MainWindow::createMenus()
{
    using namespace klogg::mainwindow;

    fileMenu = menuBar()->addMenu( tr( menu::fileTitle ) );
    fileMenu->setToolTipsVisible( true );
    fileMenu->addAction( newWindowAction );
    fileMenu->addAction( openAction );
    fileMenu->addAction( openClipboardAction );
    fileMenu->addAction( openUrlAction );
    recentFilesMenu = fileMenu->addMenu( tr( "Open Recent" ) );
    for ( auto i = 0u; i < recentFileActions.size(); ++i ) {
        recentFilesMenu->addAction( recentFileActions[ i ] );
    }
    recentFilesMenu->addSeparator();
    recentFilesMenu->addAction( recentFilesCleanup );
    recentFilesMenu->setEnabled( false );
    fileMenu->addSeparator();

    fileMenu->addAction( closeAction );
    fileMenu->addAction( closeAllAction );
    fileMenu->addSeparator();

    fileMenu->addAction( optionsAction );
    fileMenu->addSeparator();

    fileMenu->addSeparator();
    fileMenu->addAction( exitAction );

    editMenu = menuBar()->addMenu( tr( menu::editTitle ) );
    editMenu->addAction( copyAction );
    editMenu->addAction( selectAllAction );
    editMenu->addSeparator();
    editMenu->addAction( findAction );
    editMenu->addSeparator();
    editMenu->addAction( goToLineAction );
    editMenu->addSeparator();
    editMenu->addAction( copyPathToClipboardAction );
    editMenu->addAction( openContainingFolderAction );
    editMenu->addSeparator();
    editMenu->addAction( openInEditorAction );
    editMenu->addAction( clearLogAction );
    editMenu->setEnabled( false );

    viewMenu = menuBar()->addMenu( tr( menu::viewTitle ) );
    openedFilesMenu = viewMenu->addMenu( tr( menu::openedFilesTitle ) );
    viewMenu->addSeparator();
    viewMenu->addAction( overviewVisibleAction );
    viewMenu->addSeparator();
    viewMenu->addAction( lineNumbersVisibleInMainAction );
    viewMenu->addAction( lineNumbersVisibleInFilteredAction );
    viewMenu->addSeparator();
    viewMenu->addAction( textWrapAction );
    viewMenu->addSeparator();
    viewMenu->addAction( followAction );
    viewMenu->addSeparator();
    viewMenu->addAction( reloadAction );

    toolsMenu = menuBar()->addMenu( tr( menu::toolsTitle ) );

    highlightersMenu = new HighlightersMenu( tr( menu::highlightersTitle ), menuBar() );
    menuBar()->addMenu( highlightersMenu );
    highlightersMenu->setApplyChange( [ this ]() {
        auto crawler = currentCrawlerWidget();
        if ( crawler != nullptr ) {
            crawler->applyConfiguration();
        }
    } );

    toolsMenu->addAction( predefinedFiltersDialogAction );

    toolsMenu->addSeparator();
    toolsMenu->addAction( adbLogcatStartAction );
    toolsMenu->addAction( adbLogcatStopAction );
    toolsMenu->addAction( adbKmsgStartAction );
    toolsMenu->addAction( adbOfflineMergeAction );
    toolsMenu->addSeparator();
    toolsMenu->addAction( showScratchPadAction );

    menuBar()->addMenu( EncodingMenu::generate( encodingGroup ) );
    menuBar()->addSeparator();

    favoritesMenu = menuBar()->addMenu( tr( menu::favoritesTitle ) );
    favoritesMenu->setToolTipsVisible( true );

    helpMenu = menuBar()->addMenu( tr( menu::helpTitle ) );
    helpMenu->addAction( showDocumentationAction );
    helpMenu->addSeparator();
    helpMenu->addAction( reportIssueAction );
    helpMenu->addAction( joinDiscordAction );
    helpMenu->addAction( joinTelegramAction );
    helpMenu->addSeparator();
    helpMenu->addAction( generateDumpAction );
    helpMenu->addSeparator();
    helpMenu->addAction( aboutQtAction );
    helpMenu->addAction( aboutAction );

    // ADB capture actions in menu bar (right after the last menu)
    menuBar()->addAction( adbLogcatStartAction );
    menuBar()->addAction( adbLogcatStopAction );
    menuBar()->addAction( adbLogcatQuickSaveAction );
    menuBar()->addAction( adbKillCameraAction );
    menuBar()->addAction( adbKmsgStartAction );
    menuBar()->addAction( adbOfflineMergeAction );
}

void MainWindow::createToolBars()
{
    infoLine = new PathLine();
    infoLine->setFrameStyle( QFrame::StyledPanel );
    infoLine->setFrameShadow( QFrame::Sunken );
    infoLine->setLineWidth( 0 );
    infoLine->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );

    sizeField = new QLabel();
    sizeField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    dateField = new QLabel();
    dateField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    encodingField = new QLabel();
    dateField->setAlignment( Qt::AlignHCenter | Qt::AlignVCenter );

    lineNbField = new QLabel();
    lineNbField->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
    lineNbField->setContentsMargins( 2, 0, 2, 0 );

    toolBar = addToolBar( QApplication::translate( "klogg::mainwindow::toolbar",
                                                   klogg::mainwindow::toolbar::toolbarTitle ) );
    toolBar->setIconSize( QSize( 16, 16 ) );
    toolBar->setMovable( false );
    toolBar->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    toolBar->addAction( openAction );
    toolBar->addAction( reloadAction );
    toolBar->addAction( followAction );
    toolBar->addAction( addToFavoritesAction );
    toolBar->addWidget( infoLine );
    toolBar->addAction( stopAction );

    // Highlight the "Follow File" button in green while it is checked, to match
    // the active-state styling of the search toolbar buttons.
    if ( auto* followButton
         = qobject_cast<QToolButton*>( toolBar->widgetForAction( followAction ) ) ) {
        followButton->setStyleSheet(
            QStringLiteral( "QToolButton:checked { background-color: #4caf50; border: 1px "
                            "solid #388e3c; border-radius: 3px; }" ) );
    }

    infoToolbarSeparators.reserve( 5 );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( sizeField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( dateField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( encodingField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addWidget( lineNbField );
    infoToolbarSeparators.push_back( toolBar->addSeparator() );
    toolBar->addAction( showScratchPadAction );

    showInfoLabels( false );
}

void MainWindow::createTrayIcon()
{
    trayIcon_ = new QSystemTrayIcon( this );

    QMenu* trayMenu = new QMenu( this );
    QAction* openWindowAction = new QAction( tr( "Open window" ), this );
    QAction* quitAction = new QAction( tr( "Quit" ), this );

    trayMenu->addAction( openWindowAction );
    trayMenu->addAction( quitAction );

    connect( openWindowAction, &QAction::triggered, this, &QMainWindow::show );
    connect( quitAction, &QAction::triggered, [ this ] {
        this->isCloseFromTray_ = true;
        this->close();
    } );

    trayIcon_->setIcon( mainIcon_ );
    trayIcon_->setToolTip( tr( klogg::mainwindow::trayicon::trayiconTip ) );
    trayIcon_->setContextMenu( trayMenu );

    connect( trayIcon_, &QSystemTrayIcon::activated,
             [ this ]( QSystemTrayIcon::ActivationReason reason ) {
                 switch ( reason ) {
                 case QSystemTrayIcon::Trigger:
                     if ( !this->isVisible() ) {
                         this->show();
                     }
                     else {
                         this->hide();
                     }
                     break;
                 default:
                     break;
                 }
             } );

    if ( Configuration::get().minimizeToTray() ) {
        trayIcon_->show();
    }
}
//
// Q_SLOTS:
//

// Opens the file selection dialog to select a new log file
void MainWindow::open()
{
    QString defaultDir = ".";

    // Default to the path of the current file if there is one
    if ( auto current = currentCrawlerWidget() ) {
        QString current_file = session_.getFilename( current );
        QFileInfo fileInfo = QFileInfo( current_file );
        defaultDir = fileInfo.path();
    }

    const auto selectedFiles = QFileDialog::getOpenFileUrls(
        this, tr( "Open file" ), QUrl::fromLocalFile( defaultDir ), tr( "All files (*)" ) );

    std::vector<QUrl> localFiles;
    std::vector<QUrl> remoteFiles;

    std::partition_copy( selectedFiles.cbegin(), selectedFiles.cend(),
                         std::back_inserter( localFiles ), std::back_inserter( remoteFiles ),
                         []( const QUrl& url ) { return url.isLocalFile(); } );

    for ( const auto& localFile : localFiles ) {
        loadFile( localFile.toLocalFile() );
    }

    for ( const auto& remoteFile : remoteFiles ) {
        openRemoteFile( remoteFile );
    }
}

void MainWindow::openRemoteFile( const QUrl& url )
{
    Downloader downloader;

    QProgressDialog progressDialog;
    progressDialog.setLabelText( tr( "Downloading %1" ).arg( url.toString() ) );

    connect( &downloader, &Downloader::downloadProgress,
             [ &progressDialog ]( qint64 bytesReceived, qint64 bytesTotal ) {
                 const auto progress = calculateProgress( bytesReceived, bytesTotal );
                 progressDialog.setRange( 0, 100 );
                 progressDialog.setValue( progress );
             } );

    connect( &downloader, &Downloader::finished,
             [ &progressDialog ]( bool isOk ) { progressDialog.done( isOk ? 0 : 1 ); } );

    auto tempFile = new QTemporaryFile( tempDir_.filePath( url.fileName() ), this );
    if ( tempFile->open() ) {
        downloader.download( url, tempFile );
        if ( !progressDialog.exec() ) {
            loadFile( tempFile->fileName() );
        }
        else {
            QMessageBox::critical( this, tr( "Klogg - File download" ), downloader.lastError() );
        }
    }
    else {
        QMessageBox::critical( this, tr( "Klogg - File download" ),
                               tr( "Failed to create temp file" ) );
    }
}

void MainWindow::switchToOpenedFile( QAction* action )
{
    if ( !action ) {
        return;
    }

    loadFile( action->data().toString() );
}

void MainWindow::openFileFromRecent( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto filename = action->data().toString();
    if ( QFileInfo{ filename }.isReadable() ) {
        loadFile( filename );
    }
    else {
        const auto userAction = QMessageBox::question(
            this, tr( "klogg - remove from recent" ),
            tr( "Could not read file %1. Remove it from recent files?" ).arg( filename ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            removeFromRecent( filename );
        }
    }
}

void MainWindow::openFileFromFavorites( QAction* action )
{
    if ( !action ) {
        return;
    }

    const auto filename = action->data().toString();
    if ( QFileInfo{ filename }.isReadable() ) {
        loadFile( filename );
    }
    else {
        const auto userAction = QMessageBox::question(
            this, tr( "klogg - remove from favorites" ),
            tr( "Could not read file %1. Remove it from favorites?" ).arg( filename ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

        if ( userAction == QMessageBox::Yes ) {
            removeFromFavorites( filename );
        }
    }
}

// Close current tab
void MainWindow::closeTab( ActionInitiator initiator )
{
    int currentIndex = mainTabWidget_.currentIndex();

    if ( currentIndex >= 0 ) {
        closeTab( currentIndex, initiator );
    }
    else {
        this->close();
    }
}

// Close all tabs
void MainWindow::closeAll( ActionInitiator initiator )
{
    while ( mainTabWidget_.count() ) {
        closeTab( 0, initiator );
    }
}

// Select all the text in the currently selected view
void MainWindow::selectAll()
{
    if ( infoLine->hasFocus() ) {
        infoLine->setSelection( 0, klogg::isize( infoLine->text() ) );
    }
    else if ( auto current = currentCrawlerWidget(); current != nullptr ) {
        current->selectAll();
    }
}

// Copy the currently selected line into the clipboard
void MainWindow::copy()
{
    try {
        if ( infoLine->hasFocus() && infoLine->hasSelectedText() ) {
            sendTextToClipboard( infoLine->selectedText() );
            return;
        }

        if ( auto current = currentCrawlerWidget(); current != nullptr ) {
            auto text = current->getSelectedText();
            text.replace( QChar::Null, QChar::Space );

            sendTextToClipboard( text, true );
        }
    } catch ( std::exception& err ) {
        LOG_ERROR << "failed to copy data to clipboard " << err.what();
    }
}

// Display the QuickFind bar
void MainWindow::find()
{
    displayQuickFindBar( QuickFindMux::Forward );
}

void MainWindow::clearLog()
{
    const auto current_file = session_.getFilename( currentCrawlerWidget() );
    if ( QMessageBox::warning(
             this, tr( "klogg - clear file" ),
             tr( "Clear file %1? File content will be removed from disk, this is irreversible" )
                 .arg( current_file ) )
         == QMessageBox::Yes ) {
        QFile::resize( current_file, 0 );
    }
}

void MainWindow::copyFullPath()
{
    const auto current_file = session_.getFilename( currentCrawlerWidget() );
    sendTextToClipboard( QDir::toNativeSeparators( current_file ) );
}

void MainWindow::openContainingFolder()
{
    showPathInFileExplorer( session_.getFilename( currentCrawlerWidget() ) );
}

void MainWindow::openInEditor()
{
    openFileInDefaultApplication( session_.getFilename( currentCrawlerWidget() ) );
}

void MainWindow::tryOpenClipboard( int tryTimes )
{
    auto clipboard = QGuiApplication::clipboard();
    auto text = clipboard->text();

    if ( text.isEmpty() && tryTimes > 0 ) {
        QTimer::singleShot( 50, [ tryTimes, this ]() { tryOpenClipboard( tryTimes - 1 ); } );
    }
    else {
        auto tempFile = new QTemporaryFile( tempDir_.filePath( "klogg_clipboard" ), this );
        if ( tempFile->open() ) {
            tempFile->write( text.toUtf8() );
            tempFile->flush();

            loadFile( tempFile->fileName() );
        }
    }
}

void MainWindow::openClipboard()
{
    tryOpenClipboard( ClipboardMaxTry );
}

void MainWindow::openUrl()
{
    bool ok;
    const auto urlInClipboard = QUrl::fromUserInput( QApplication::clipboard()->text() );
    const auto selectedUrl = urlInClipboard.isValid() ? urlInClipboard.toString() : QString{};

    QString url
        = QInputDialog::getText( this, tr( "Open URL as log file" ), tr( "URL to download:" ),
                                 QLineEdit::Normal, selectedUrl, &ok );
    if ( ok && !url.isEmpty() ) {
        openRemoteFile( url );
    }
}

// Opens the 'Highlighters' dialog box
void MainWindow::editHighlighters()
{
    HighlightersDialog dialog( this );
    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    connect( &dialog, &HighlightersDialog::optionsChanged,
             [ this ]() { updateHighlightersMenu(); } );

    dialog.exec();
    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

// Opens dialog to configure predefined filters
void MainWindow::editPredefinedFilters( const QString& newFilter )
{
    PredefinedFiltersDialog dialog( newFilter, this );

    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    dialog.exec();
    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

// Opens the 'Options' modal dialog box
void MainWindow::options()
{
    OptionsDialog dialog( this );
    signalMux_.connect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );

    connect( &dialog, &OptionsDialog::optionsChanged, [ this ]() {
        const auto& config = Configuration::get();
        logging::enableFileLogging( config.enableLogging(),
                                    static_cast<logging::LogLevel>( config.loggingLevel() ) );

        newWindowAction->setVisible( config.allowMultipleWindows() );
        followAction->setEnabled( config.anyFileWatchEnabled() );

        updateShortcuts();
        updateRecentFileActions();
    } );
    dialog.exec();

    signalMux_.disconnect( &dialog, SIGNAL( optionsChanged() ), SLOT( applyConfiguration() ) );
}

void MainWindow::about()
{
    QMessageBox::about(
        this, tr( "About klogg" ),
        tr( "<h2>klogg %1</h2>"
            "<p>A fast, advanced log explorer.</p>"
            "<p>Built %2 from %3</p>"
            "<p><a href=\"https://github.com/variar/klogg\">https://github.com/variar/klogg</a></p>"
            "<p>This is fork of glogg</p>"
            "<p><a href=\"http://glogg.bonnefon.org/\">http://glogg.bonnefon.org/</a></p>"
            "<p>Using icons from <a href=\"https://icons8.com\">icons8.com</a> project</p>"
            "<p>Copyright &copy; 2020 Nicolas Bonnefon, Anton Filimonov and other contributors</p>"
            "<p>You may modify and redistribute the program under the terms of the GPL (version 3 "
            "or later).</p>" )
            .arg( kloggVersion(), kloggBuildDate(), kloggCommit() ) );
}

void MainWindow::aboutQt()
{
    QMessageBox::aboutQt( this, tr( "About Qt" ) );
}

void MainWindow::documentation()
{
    QFile doc( ":/documentation.html" );
    if ( doc.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
        const auto text = QString::fromUtf8( doc.readAll() );
        QTextBrowser* tb = new QTextBrowser();
        tb->setOpenExternalLinks( true );
        tb->setHtml( text );
        tb->setWindowFlags( Qt::Window );
        tb->setAttribute( Qt::WA_DeleteOnClose );
        tb->setWindowTitle( tr( "klogg documentation" ) );
        tb->resize( this->width() / 2, this->height() );
        tb->show();
    }
    else {
        LOG_ERROR << "Can't open documentation resource";
    }
}

void MainWindow::showScratchPad()
{
    auto state = scratchPad_.windowState();
    state.setFlag( Qt::WindowMinimized, false );
    scratchPad_.setWindowState( state );

    scratchPad_.show();
    scratchPad_.activateWindow();
}

void MainWindow::sendToScratchpad( QString newData )
{
    scratchPad_.addData( newData );
    showScratchPad();
}

void MainWindow::replaceDataInScratchpad( QString newData )
{
    scratchPad_.replaceData( newData );
    showScratchPad();
}

void MainWindow::encodingChanged( QAction* action )
{
    const auto mibData = action->data();
    std::optional<int> mib;
    if ( mibData.isValid() ) {
        mib = mibData.toInt();
    }

    LOG_DEBUG << "encodingChanged, encoding " << mib.value_or( 0 );
    if ( auto crawler = currentCrawlerWidget() ) {
        crawler->setEncoding( mib );
        updateInfoLine();
    }
}

void MainWindow::toggleOverviewVisibility( bool isVisible )
{
    auto& config = Configuration::get();
    config.setOverviewVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::toggleMainLineNumbersVisibility( bool isVisible )
{
    auto& config = Configuration::get();

    config.setMainLineNumbersVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::toggleFilteredLineNumbersVisibility( bool isVisible )
{
    auto& config = Configuration::get();

    config.setFilteredLineNumbersVisible( isVisible );
    config.save();
    Q_EMIT optionsChanged();
}

void MainWindow::changeFollowMode( bool follow )
{
    auto& config = Configuration::get();
    if ( follow && !( config.nativeFileWatchEnabled() || config.pollingEnabled() ) ) {
        LOG_WARNING << "File watch disabled in settings";
    }

    followAction->setChecked( follow );
}

void MainWindow::lineNumberHandler( LineNumber startLine, LinesCount nLines, LineColumn startCol,
                                    LineLength nSymbols )
{
    // The line number received is the internal (starts at 0)
    uint64_t fileSize{};
    uint64_t fileNbLine{};
    QDateTime lastModified;

    session_.getFileInfo( currentCrawlerWidget(), &fileSize, &fileNbLine, &lastModified );

    if ( fileNbLine != 0 ) {
        if ( nSymbols.get() == 0 ) {
            lineNbField->setText( tr( "Ln:%1/%2" ).arg( startLine.get() + 1 ).arg( fileNbLine ) );
        }
        else {
            if ( nLines.get() == 1 ) {
                // portion selection on one line
                lineNbField->setText( tr( "Ln:%1/%2 Col:%3 Sel:%4|%5" )
                                          .arg( startLine.get() + 1 )
                                          .arg( fileNbLine )
                                          .arg( startCol.get() )
                                          .arg( nSymbols.get() )
                                          .arg( nLines.get() ) );
            }
            else {
                // multiple lines selection
                lineNbField->setText( tr( "Ln:%1/%2 Sel:%4|%5" )
                                          .arg( startLine.get() + 1 )
                                          .arg( fileNbLine )
                                          .arg( nSymbols.get() )
                                          .arg( nLines.get() ) );
            }
        }
    }
    else {
        lineNbField->clear();
    }
}

void MainWindow::newPredefinedFilterHandler( QString newFilter )
{
    editPredefinedFilters( newFilter );
}

void MainWindow::updateLoadingProgress( int progress )
{
    LOG_DEBUG << "Loading progress: " << progress;

    QString current_file
        = QDir::toNativeSeparators( session_.getFilename( currentCrawlerWidget() ) );

    // We ignore 0% and 100% to avoid a flash when the file (or update)
    // is very short.
    if ( progress > 0 && progress < 100 ) {
        infoLine->setText( current_file + tr( " - Indexing lines... (%1 %)" ).arg( progress ) );
        infoLine->displayGauge( progress );

        showInfoLabels( false );

        stopAction->setEnabled( true );
        reloadAction->setEnabled( false );
    }
}

void MainWindow::handleLoadingFinished( LoadingStatus status )
{
    LOG_DEBUG << "handleLoadingFinished success=" << ( status == LoadingStatus::Successful );

    // No file is loading
    loadingFileName.clear();

    if ( status == LoadingStatus::Successful ) {
        updateInfoLine();

        infoLine->hideGauge();
        showInfoLabels( true );
        stopAction->setEnabled( false );
        reloadAction->setEnabled( true );

        lineNumberHandler( 0_lnum, LinesCount( 0 ), LineColumn( 0 ), LineLength( 0 ) );

        // Now everything is ready, we can finally show the file!
        currentCrawlerWidget()->show();
    }
    else {
        if ( status == LoadingStatus::NoMemory ) {
            QMessageBox alertBox;
            alertBox.setText( tr( "Not enough memory." ) );
            alertBox.setInformativeText(
                tr( "The system does not have enough memory to hold the index for this file. The "
                    "file will now be closed." ) );
            alertBox.setIcon( QMessageBox::Critical );
            alertBox.exec();
        }

        closeTab( mainTabWidget_.currentIndex(), ActionInitiator::App );
    }

    // mainTabWidget_.setEnabled( true );
}

void MainWindow::handleFilteredViewChanged()
{
    int currentIndex = mainTabWidget_.currentIndex();
    if ( currentIndex >= 0 ) {
        auto* crawler_widget = static_cast<CrawlerWidget*>( mainTabWidget_.widget( currentIndex ) );
        quickFindMux_.registerSelector( crawler_widget );
    }
}

void MainWindow::closeTab( int index, ActionInitiator initiator )
{
    auto widget = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( index ) );

    assert( widget );

    widget->stopLoading();
    mainTabWidget_.removeCrawler( index );

    if ( initiator == ActionInitiator::User ) {
        addRecentFile( session_.getFilename( widget ) );
    }

    session_.close( widget );

    updateOpenedFilesMenu();

    widget->deleteLater();
}

void MainWindow::currentTabChanged( int index )
{
    LOG_DEBUG << "currentTabChanged";

    if ( index >= 0 ) {
        auto* crawler_widget = static_cast<CrawlerWidget*>( mainTabWidget_.widget( index ) );
        signalMux_.setCurrentDocument( crawler_widget );
        quickFindMux_.registerSelector( crawler_widget );

        // New tab is set up with fonts etc...
        Q_EMIT optionsChanged();

        updateMenuBarFromDocument( crawler_widget );
        updateTitleBar( session_.getFilename( crawler_widget ) );
        updateFavoritesMenu();

        editMenu->setEnabled( true );
    }
    else {
        // No tab left
        signalMux_.setCurrentDocument( nullptr );
        quickFindMux_.registerSelector( nullptr );

        infoLine->hideGauge();
        infoLine->clear();
        showInfoLabels( false );

        updateTitleBar( QString() );

        editMenu->setEnabled( false );
        addToFavoritesAction->setEnabled( false );
        addToFavoritesMenuAction->setEnabled( false );
    }
}

void MainWindow::changeQFPattern( const QString& newPattern )
{
    quickFindWidget_.changeDisplayedPattern( newPattern, true );
}

void MainWindow::loadFileNonInteractive( const QString& file_name )
{
    LOG_DEBUG << "loadFileNonInteractive( " << file_name.toStdString() << " )";

    loadFile( file_name );

    // Try to get the window to the front
    // This is a bit of a hack but has been tested on:
    // Qt 5.3 / Gnome / Linux
    // Qt 5.11 / Win10
#ifdef Q_OS_WIN
    const auto isMaximized = isMaximized_;

    if ( isMaximized ) {
        showMaximized();
    }
    else {
        showNormal();
    }

    activateWindow();
    raise();
#else
    Qt::WindowFlags window_flags = windowFlags();
    window_flags |= Qt::WindowStaysOnTopHint;
    setWindowFlags( window_flags );

    raise();
    activateWindow();

    window_flags = windowFlags();
    window_flags &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags( window_flags );
    show();
#endif

    if ( auto currentCrawler = currentCrawlerWidget() ) {
        currentCrawler->setFocus();
    }
}

//
// Events
//

// Closes the application
void MainWindow::closeEvent( QCloseEvent* event )
{
    if ( !isCloseFromTray_ && this->isVisible() && Configuration::get().minimizeToTray() ) {
        event->ignore();
        trayIcon_->show();
        this->hide();
    }
    else {
        cleanupAdbLogcatProcess();
        cleanupAdbKmsgProcess();

        const auto saveSettings = session_.close();
        if ( saveSettings ) {
            writeSettings();
        }

        closeAll( ActionInitiator::App );
        trayIcon_->hide();
        Q_EMIT windowClosed();

        event->accept();
    }
}

// Minimize handling the application
void MainWindow::changeEvent( QEvent* event )
{
    if ( event->type() == QEvent::WindowStateChange ) {
        isMaximized_ = windowState().testFlag( Qt::WindowMaximized );

        if ( this->windowState() & Qt::WindowMinimized ) {
            if ( Configuration::get().minimizeToTray() ) {
                dispatchToMainThread( [ this ] {
                    trayIcon_->show();
                    this->hide();
                } );
            }
        }
    }
    else if ( event->type() == QEvent::StyleChange ) {
        dispatchToMainThread( [ this ] {
            loadIcons();
            updateOpenedFilesMenu();
            updateFavoritesMenu();
            updateHighlightersMenu();
        } );
    }
    else if ( event->type() == QEvent::LanguageChange ) {
        reTranslateUI();
    }

    QMainWindow::changeEvent( event );
}

// Accepts the drag event if it looks like a filename
void MainWindow::dragEnterEvent( QDragEnterEvent* event )
{
    if ( event->mimeData()->hasFormat( "text/uri-list" ) )
        event->acceptProposedAction();
}

// Tries and loads the file if the URL dropped is local
void MainWindow::dropEvent( QDropEvent* event )
{
    const QList<QUrl> urls = event->mimeData()->urls();

    for ( const auto& url : urls ) {
        auto fileName = url.toLocalFile();
        if ( fileName.isEmpty() )
            continue;

        // Files dropped in should follow (auto-refresh) by default.
        loadFile( fileName, true );
    }
}

bool MainWindow::event( QEvent* event )
{
    if ( event->type() == QEvent::WindowActivate ) {
        Q_EMIT windowActivated();
    }
    else if ( event->type() == QEvent::Show ) {
        if ( this->windowHandle() ) {
            std::call_once( screenChangesConnect_, [ this ]() {
                logScreenInfo( this->windowHandle()->screen() );
                connect( this->windowHandle(), &QWindow::screenChanged,
                         [ this ]( QScreen* screen ) { logScreenInfo( screen ); } );
            } );
        }
    }

    return QMainWindow::event( event );
}

//
// Private functions
//

bool MainWindow::extractAndLoadFile( const QString& fileName )
{
    const auto& config = Configuration::get();

    if ( !config.extractArchives() ) {
        return false;
    }

    if ( !config.extractArchivesAlways() ) {
        const auto userChoice
            = QMessageBox::question( this, tr( "klogg" ), tr( "Extract archive to temp folder?" ) );
        if ( userChoice == QMessageBox::No ) {
            return false;
        }
    }

    const auto decompressAction = Decompressor::action( fileName );

    Decompressor decompressor;
    AtomicFlag decompressInterrupt;

    QProgressDialog progressDialog;
    progressDialog.setLabelText( tr( "Extracting %1" ).arg( fileName ) );
    progressDialog.setRange( 0, 0 );

    connect( &decompressor, &Decompressor::finished,
             [ &progressDialog ]( bool isOk ) { progressDialog.done( isOk ? 0 : 1 ); } );
    connect( &progressDialog, &QProgressDialog::canceled,
             [ &decompressInterrupt, &decompressor ]() {
                 decompressInterrupt.set();
                 decompressor.waitForResult();
             } );

    if ( decompressAction == DecompressAction::Decompress ) {

        auto tempFile = new QTemporaryFile(
            this->tempDir_.filePath( QFileInfo( fileName ).fileName() ), this );

        if ( tempFile->open() && decompressor.decompress( fileName, tempFile, decompressInterrupt )
             && !progressDialog.exec() ) {

            if ( decompressInterrupt ) {
                return false;
            }

            return this->loadFile( tempFile->fileName() );
        }
        else {
            QMessageBox::warning(
                this, tr( "klogg" ),
                tr( "Failed to decompress %1" ).arg( QDir::toNativeSeparators( fileName ) ) );
        }
    }
    else if ( decompressAction == DecompressAction::Extract ) {
        QTemporaryDir archiveDir{ this->tempDir_.filePath( QFileInfo( fileName ).fileName() ) };
        archiveDir.setAutoRemove( false );
        if ( decompressor.extract( fileName, archiveDir.path(), decompressInterrupt )
             && !progressDialog.exec() ) {

            if ( decompressInterrupt ) {
                return false;
            }

            const auto selectedFiles = QFileDialog::getOpenFileNames(
                this, tr( "Open file from archive" ), archiveDir.path(), tr( "All files (*)" ) );

            for ( const auto& extractedFile : selectedFiles ) {
                this->loadFile( extractedFile );
            }

            return true;
        }
        else {
            QMessageBox::warning(
                this, tr( "klogg" ),
                tr( "Failed to extract %1" ).arg( QDir::toNativeSeparators( fileName ) ) );
        }
    }

    return false;
}

// Create a CrawlerWidget for the passed file, start its loading
// and update the title bar.
// The loading is done asynchronously.
bool MainWindow::loadFile( const QString& fileName, bool followFile )
{
    LOG_DEBUG << "loadFile ( " << fileName.toStdString() << " )";

    // First check if the file is already open...
    auto* existing_crawler = static_cast<CrawlerWidget*>( session_.getViewIfOpen( fileName ) );

    if ( existing_crawler ) {
        auto* crawlerWindow = qobject_cast<MainWindow*>( existing_crawler->window() );
        crawlerWindow->mainTabWidget_.setCurrentWidget( existing_crawler );
        crawlerWindow->activateWindow();
        return true;
    }

    const auto decompressAction = Decompressor::action( fileName );

    if ( decompressAction == DecompressAction::None || !Configuration::get().extractArchives() ) {
        // Load the file
        loadingFileName = fileName;

        try {
            const auto previousViewContext = [ &fileName ]() {
                const auto& session = SessionInfo::getSynced();
                const auto& windows = session.windows();
                for ( const auto& windowId : windows ) {
                    const auto openedFiles = session.openFiles( windowId );
                    const auto existingContext
                        = std::find_if( openedFiles.begin(), openedFiles.end(),
                                        [ &fileName ]( const auto& context ) {
                                            return context.fileName == fileName;
                                        } );
                    if ( existingContext != openedFiles.end() ) {
                        return existingContext->viewContext;
                    }
                }
                return QString{};
            }();

            CrawlerWidget* crawler_widget = static_cast<CrawlerWidget*>(
                session_.open( fileName, []() { return new CrawlerWidget(); } ) );

            if ( !crawler_widget ) {
                LOG_ERROR << "Can't create crawler for " << fileName.toStdString();
                return false;
            }

            // We won't show the widget until the file is fully loaded
            crawler_widget->hide();

            if ( !previousViewContext.isEmpty() ) {
                LOG_INFO << "Found existing context";
                crawler_widget->setViewContext( previousViewContext );
            }

            // We disable the tab widget to avoid having someone switch
            // tab during loading. (maybe FIXME)
            // mainTabWidget_.setEnabled( false );

            int index = mainTabWidget_.addCrawler( crawler_widget, fileName );

            // Connect color labels signal for cross-tab sync
            connect( crawler_widget, &CrawlerWidget::colorLabelsChanged, this,
                     &MainWindow::onColorLabelsChanged );

            // Apply global color labels to the new tab
            if ( !globalColorLabels_.empty() ) {
                crawler_widget->restoreColorLabels( globalColorLabels_ );
            }

            // Setting the new tab, the user will see a blank page for the duration
            // of the loading, with no way to switch to another tab
            mainTabWidget_.setCurrentIndex( index );

            addRecentFile( fileName );
            updateOpenedFilesMenu();

            const auto& config = Configuration::get();
            // Newly opened files follow by default so live logs tail
            // automatically, regardless of the persisted "follow on load"
            // preference. (followFile is accepted for API compatibility.)
            Q_UNUSED( followFile )
            if ( config.anyFileWatchEnabled() ) {
                signalCrawlerToFollowFile( crawler_widget );
                followAction->setChecked( true );
            }
        } catch ( ... ) {
            LOG_ERROR << "Can't open file " << fileName.toStdString();
            return false;
        }

        LOG_DEBUG << "Success loading file " << fileName.toStdString();
        return true;
    }
    else {
        return extractAndLoadFile( fileName );
    }
}

// Strips the passed filename from its directory part.
QString MainWindow::strippedName( const QString& fullFileName ) const
{
    return QFileInfo( fullFileName ).fileName();
}

// Return the currently active CrawlerWidget, or NULL if none
CrawlerWidget* MainWindow::currentCrawlerWidget() const
{
    auto current = qobject_cast<CrawlerWidget*>( mainTabWidget_.currentWidget() );

    return current;
}

// Update the title bar.
void MainWindow::updateTitleBar( const QString& file_name )
{
    QString shownName = tr( "Untitled" );
    if ( !file_name.isEmpty() ) {
        shownName = strippedName( file_name );
    }

    QString indexPart = "";
    if ( session_.windowIndex() > 0 ) {
        indexPart = QString( " #%1" ).arg( session_.windowIndex() + 1 );
    }

    setWindowTitle( tr( "%1 - %2%3" ).arg( shownName, tr( "klogg" ), indexPart ) + tr( " (build " )
                    + kloggVersion() + ")-" + kloggCompileDate() );
}

void MainWindow::addRecentFile( const QString& fileName )
{
    auto& recentFiles = RecentFiles::getSynced();
    recentFiles.addRecent( fileName );
    recentFiles.save();
    updateRecentFileActions();
}

// Updates the actions for the recent files.
// Must be called after having added a new name to the list.
void MainWindow::updateRecentFileActions()
{
    auto& recentFiles = RecentFiles::get();
    QStringList recent_files = recentFiles.recentFiles();
    int recent_files_max_items = recentFiles.getNumberItemsToShow();

    if ( recentFiles.recentFiles().count() > 0 ) {
        recentFilesMenu->setEnabled( true );
        for ( auto j = 0; j < MAX_RECENT_FILES; ++j ) {
            const auto actionIndex = static_cast<size_t>( j );
            if ( j < recent_files_max_items ) {
                int key = j + ( ( j < 9 ) ? 0x31 : ( 0x61 - 9 ) ); // shortcuts: 1..9 next a,b...
                QString text
                    = tr( "&%1 %2" ).arg( QChar( key ) ).arg( strippedName( recent_files[ j ] ) );
                recentFileActions[ actionIndex ]->setText( text );
                recentFileActions[ actionIndex ]->setToolTip( recent_files[ j ] );
                recentFileActions[ actionIndex ]->setData( recent_files[ j ] );
                recentFileActions[ actionIndex ]->setVisible( true );
            }
            else {
                recentFileActions[ actionIndex ]->setVisible( false );
            }
        }
    }
    else {
        recentFilesMenu->setEnabled( false );
    }

    // separatorAction->setVisible(!recentFiles.isEmpty());
}

// Clear the list of the recent files
void MainWindow::clearRecentFileActions()
{
    auto& recentFiles = RecentFiles::getSynced();
    recentFiles.removeAll();
    recentFiles.save();
    updateRecentFileActions();
}
// Update our menu bar to match the settings of the crawler
// (used when the tab is changed)
void MainWindow::updateMenuBarFromDocument( const CrawlerWidget* crawler )
{
    const auto encodingMib = crawler->encodingMib();

    auto encodingActions = encodingGroup->actions();
    auto encodingItem = std::find_if( encodingActions.begin(), encodingActions.end(),
                                      [ &encodingMib ]( const auto& action ) {
                                          return ( !encodingMib && !action->data().isValid() )
                                                 || ( encodingMib && action->data().isValid()
                                                      && *encodingMib == action->data().toInt() );
                                      } );

    if ( encodingItem != encodingActions.end() ) {
        ( *encodingItem )->setChecked( true );
    }

    followAction->setChecked( crawler->isFollowEnabled() );
    textWrapAction->setChecked( crawler->isTextWrapEnabled() );
}

// Update the top info line from the session
void MainWindow::updateInfoLine()
{
    QLocale defaultLocale;

    // Following should always work as we will only receive enter
    // this slot if there is a crawler connected.
    QString current_file
        = QDir::toNativeSeparators( session_.getFilename( currentCrawlerWidget() ) );

    uint64_t fileSize;
    uint64_t fileNbLine;
    QDateTime lastModified;

    session_.getFileInfo( currentCrawlerWidget(), &fileSize, &fileNbLine, &lastModified );

    infoLine->setText( current_file );
    infoLine->setPath( current_file );
    sizeField->setText( readableSize( fileSize ) );
    encodingField->setText( currentCrawlerWidget()->encodingText() );

    if ( lastModified.isValid() ) {
        const QString date = defaultLocale.toString( lastModified, QLocale::NarrowFormat );
        dateField->setText( tr( "modified on %1" ).arg( date ) );
        dateField->show();
    }
    else {
        dateField->hide();
    }
}

void MainWindow::updateOpenedFilesMenu()
{
    openedFilesMenu->clear();

    const auto& files = session_.openedFiles();

    openedFilesMenu->setEnabled( !files.empty() );

    openedFilesMenu->addAction( selectOpenFileAction );
    openedFilesMenu->addSeparator();

    for ( const auto& file : files ) {
        const auto displayFile = DisplayFilePath{ file };
        auto action = openedFilesMenu->addAction( displayFile.displayName() );

        action->setActionGroup( openedFilesGroup );
        action->setToolTip( displayFile.nativeFullPath() );
        action->setData( displayFile.fullPath() );
    }

    selectOpenFileAction->setDisabled( files.empty() );
}

void MainWindow::updateHighlightersMenu()
{
    highlightersMenu->clearHighlightersMenu();
    highlightersMenu->createHighlightersMenu();
    highlightersMenu->addAction( editHighlightersAction, true );
    highlightersMenu->populateHighlightersMenu();
}

void MainWindow::updateFavoritesMenu()
{
    favoritesMenu->clear();

    favoritesMenu->addAction( addToFavoritesMenuAction );
    favoritesMenu->addAction( removeFromFavoritesAction );

    addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star" ) );

    using namespace klogg::mainwindow;

    addToFavoritesAction->setText(
        QApplication::translate( "klogg::mainwindow::action", action::addToFavoritesText ) );
    addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star" ) );
    addToFavoritesAction->setData( true );

    const auto& favorites = FavoriteFiles::getSynced().favorites();
    auto crawler = currentCrawlerWidget();

    addToFavoritesAction->setEnabled( crawler != nullptr );
    addToFavoritesMenuAction->setEnabled( crawler != nullptr );
    removeFromFavoritesAction->setEnabled( !favorites.empty() );

    if ( crawler ) {
        const auto path = session_.getFilename( crawler );
        if ( std::any_of( favorites.begin(), favorites.end(), FullPathComparator( path ) ) ) {

            addToFavoritesAction->setText( QApplication::translate(
                "klogg::mainwindow::action", action::removeFromFavoritesText ) );
            addToFavoritesAction->setIcon( iconLoader_.load( "icons8-star-filled" ) );
            addToFavoritesAction->setData( false );

            addToFavoritesMenuAction->setEnabled( false );
            addToFavoritesMenuAction->setIcon( iconLoader_.load( "icons8-star-filled" ) );
        }
    }

    favoritesMenu->addSeparator();

    for ( const auto& file : favorites ) {
        auto action = favoritesMenu->addAction( file.displayName() );

        action->setActionGroup( favoritesGroup );
        action->setToolTip( file.nativeFullPath() );
        action->setData( file.fullPath() );
    }
}

void MainWindow::addToFavorites()
{
    if ( const auto crawler = currentCrawlerWidget() ) {
        auto& favorites = FavoriteFiles::get();
        const auto path = session_.getFilename( crawler );

        if ( addToFavoritesAction->data().toBool() ) {
            favorites.add( path );
        }
        else {
            favorites.remove( path );
        }

        favorites.save();

        updateFavoritesMenu();
    }
}

void MainWindow::removeFromFavorites()
{
    const auto& favoriteFiles = FavoriteFiles::get();
    const auto& favorites = favoriteFiles.favorites();
    QStringList files;
    std::transform( favorites.cbegin(), favorites.cend(), std::back_inserter( files ),
                    []( const auto& f ) { return f.nativeFullPath(); } );

    auto currentIndex = 0;

    if ( const auto crawler = currentCrawlerWidget() ) {
        const auto currentPath = session_.getFilename( crawler );
        const auto currentItem
            = std::find_if( favorites.begin(), favorites.end(), FullPathComparator( currentPath ) );
        if ( currentItem != favorites.end() ) {
            currentIndex = static_cast<int>( std::distance( favorites.begin(), currentItem ) );
        }
    }

    bool ok = false;
    const auto pathToRemove = QInputDialog::getItem( this, tr( "Remove from favorites" ),
                                                     tr( "Select item to remove from favorites" ),
                                                     files, currentIndex, false, &ok );
    if ( ok ) {
        removeFromFavorites( pathToRemove );
    }
}

void MainWindow::removeFromFavorites( const QString& pathToRemove )
{
    auto& favoriteFiles = FavoriteFiles::get();
    const auto& favorites = favoriteFiles.favorites();
    const auto selectedFile = std::find_if( favorites.begin(), favorites.end(),
                                            [ pathToRemove ]( const DisplayFilePath& f ) {
                                                return f.nativeFullPath() == pathToRemove;
                                            } );

    if ( selectedFile != favorites.end() ) {
        favoriteFiles.remove( selectedFile->fullPath() );
        favoriteFiles.save();
        updateFavoritesMenu();
    }
}

void MainWindow::removeFromRecent( const QString& pathToRemove )
{
    auto& recentFiles = RecentFiles::get();
    recentFiles.removeRecent( pathToRemove );
    recentFiles.save();
    updateRecentFileActions();
}

void MainWindow::selectOpenedFile()
{
    auto openedFilesPaths = session_.openedFiles();
    std::vector<DisplayFilePath> openedFiles;
    openedFiles.reserve( openedFilesPaths.size() );
    std::transform( openedFilesPaths.cbegin(), openedFilesPaths.cend(),
                    std::back_inserter( openedFiles ),
                    []( const auto& path ) { return DisplayFilePath{ path }; } );

    QStringList filesToShow;
    std::transform( openedFiles.cbegin(), openedFiles.cend(), std::back_inserter( filesToShow ),
                    []( const auto& f ) { return f.nativeFullPath(); } );

    auto selectFileDialog = std::make_unique<QDialog>( this );
    selectFileDialog->setWindowTitle( tr( "klogg -- switch to file" ) );
    selectFileDialog->setMinimumWidth( 800 );
    selectFileDialog->setMinimumHeight( 600 );

    auto filesModel = std::make_unique<QStringListModel>( filesToShow, selectFileDialog.get() );
    auto filteredModel = std::make_unique<QSortFilterProxyModel>( selectFileDialog.get() );
    filteredModel->setSourceModel( filesModel.get() );

    auto filesView = std::make_unique<QListView>();
    filesView->setModel( filteredModel.get() );
    filesView->setEditTriggers( QAbstractItemView::NoEditTriggers );
    filesView->setSelectionMode( QAbstractItemView::SingleSelection );

    auto filterEdit = std::make_unique<QLineEdit>();
    auto buttonBox
        = std::make_unique<QDialogButtonBox>( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );

    connect( buttonBox.get(), &QDialogButtonBox::accepted, selectFileDialog.get(),
             &QDialog::accept );
    connect( buttonBox.get(), &QDialogButtonBox::rejected, selectFileDialog.get(),
             &QDialog::reject );

    connect( filterEdit.get(), &QLineEdit::textEdited,
             [ model = filteredModel.get(), view = filesView.get() ]( const QString& filter ) {
                 model->setFilterWildcard( filter );
                 model->invalidate();
                 view->selectionModel()->select( model->index( 0, 0 ),
                                                 QItemSelectionModel::SelectCurrent );
             } );

    dispatchToMainThread( [ edit = filterEdit.get() ]() { edit->setFocus(); } );

    connect( selectFileDialog.get(), &QDialog::finished,
             [ this, openedFiles, dialog = selectFileDialog.get(), model = filteredModel.get(),
               view = filesView.get() ]( auto result ) {
                 dialog->deleteLater();
                 if ( result != QDialog::Accepted || !view->selectionModel()->hasSelection() ) {
                     return;
                 }
                 const auto& selectedPath
                     = model->data( view->selectionModel()->selectedIndexes().front() ).toString();
                 const auto selectedFile
                     = std::find_if( openedFiles.begin(), openedFiles.end(),
                                     [ selectedPath ]( const DisplayFilePath& f ) {
                                         return f.nativeFullPath() == selectedPath;
                                     } );

                 if ( selectedFile != openedFiles.end() ) {
                     loadFile( selectedFile->fullPath() );
                 }
             } );

    auto layout = std::make_unique<QVBoxLayout>();
    layout->addWidget( filesView.release() );
    layout->addWidget( filterEdit.release() );
    layout->addWidget( buttonBox.release() );

    selectFileDialog->setLayout( layout.release() );
    selectFileDialog->setModal( true );
    selectFileDialog->open();

    filesModel.release();
    filteredModel.release();
    selectFileDialog.release();
}

void MainWindow::showInfoLabels( bool show )
{
    for ( auto separator : infoToolbarSeparators ) {
        separator->setVisible( show );
    }
    if ( !show ) {
        sizeField->clear();
        dateField->clear();
        encodingField->clear();
        lineNbField->clear();
    }
}

// Write settings to permanent storage
void MainWindow::writeSettings()
{
    // Save the session
    // Generate the ordered list of widgets and their topLine
    std::vector<
        std::tuple<const ViewInterface*, uint64_t, std::shared_ptr<const ViewContextInterface>>>
        widget_list;
    for ( int i = 0; i < mainTabWidget_.count(); ++i ) {
        auto view = qobject_cast<const CrawlerWidget*>( mainTabWidget_.widget( i ) );
        widget_list.emplace_back( view, 0UL, view->context() );
    }
    session_.save( widget_list, saveGeometry() );
}

// Read settings from permanent storage
void MainWindow::readSettings()
{
    // Get and restore the session
    // auto& session = SessionInfo::getSynced();
    /*
     * FIXME: should be in the session
    crawlerWidget->restoreState( session.crawlerState() );
    */

    // History of recent files
    RecentFiles::getSynced();
    updateRecentFileActions();

    FavoriteFiles::getSynced();
    updateFavoritesMenu();

    HighlighterSetCollection::getSynced();
    updateHighlightersMenu();
}

void MainWindow::displayQuickFindBar( QuickFindMux::QFDirection direction )
{
    LOG_DEBUG << "MainWindow::displayQuickFindBar";

    // Warn crawlers so they can save the position of the focus in order
    // to do incremental search in the right view.
    Q_EMIT enteringQuickFind();

    const auto crawler = currentCrawlerWidget();
    if ( crawler != nullptr && crawler->isPartialSelection() ) {
        auto selection = crawler->getSelectedText();
        if ( !selection.isEmpty() ) {
            quickFindWidget_.changeDisplayedPattern( selection, false );
        }
    }

    quickFindMux_.setDirection( direction );
    quickFindWidget_.userActivate();
}

void MainWindow::logScreenInfo( QScreen* screen )
{
    LOG_INFO << "screen changed for " << session_.windowIndex();
    if ( screen == nullptr ) {
        return;
    }

    LOG_INFO << "screen name " << screen->name();
    LOG_INFO << "screen size " << screen->size().width() << "x" << screen->size().height();
    LOG_INFO << "screen ratio " << screen->devicePixelRatio();
    LOG_INFO << "screen logical dpi " << screen->logicalDotsPerInch();
    LOG_INFO << "screen physical dpi " << screen->physicalDotsPerInch();
}

void MainWindow::generateDump()
{
    const auto userAction = QMessageBox::warning(
        this, tr( "klogg - generate crash dump" ),
        tr( "This will shutdown klogg and generate diagnostic crash dump. Continue?" ),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

    if ( userAction == QMessageBox::Yes ) {
        throw std::logic_error( "test dump" );
    }
}

void MainWindow::cleanupAdbLogcatProcess()
{
    if ( adbLogcatProcess_ == nullptr ) {
        return;
    }

    adbLogcatProcess_->kill();
    adbLogcatProcess_->waitForFinished( 500 );
    adbLogcatProcess_->deleteLater();
    adbLogcatProcess_ = nullptr;

    // Capture stopped - reset the tab color back to the default.
    mainTabWidget_.setTabColorForFile( adbLogcatFilePath_, QColor() );

    if ( adbLogcatStartAction != nullptr ) {
        adbLogcatStartAction->setEnabled( true );
    }
    if ( adbLogcatStopAction != nullptr ) {
        adbLogcatStopAction->setEnabled( false );
    }
}

void MainWindow::cleanupAdbKmsgProcess()
{
    if ( adbKmsgProcess_ == nullptr ) {
        return;
    }

    adbKmsgProcess_->kill();
    adbKmsgProcess_->waitForFinished( 500 );
    adbKmsgProcess_->deleteLater();
    adbKmsgProcess_ = nullptr;

    // Capture stopped - reset the tab color back to the default.
    mainTabWidget_.setTabColorForFile( adbKmsgFilePath_, QColor() );
}

void MainWindow::stopAdbLogcat()
{
    if ( adbLogcatProcess_ == nullptr ) {
        return;
    }

    cleanupAdbLogcatProcess();
}

void MainWindow::startAdbLogcat()
{
    startAdbCapture( QDir( QDir::tempPath() ).filePath( QStringLiteral( "klogg_adb_logcat.log" ) ),
                     QStringList() << QStringLiteral( "logcat" ) << QStringLiteral( "-v" )
                                   << QStringLiteral( "threadtime" ),
                     true );
}

void MainWindow::startAdbKmsg()
{
    // F6 toggles kernel-log capture, independent of the F1/F2 logcat capture.
    if ( adbKmsgProcess_ != nullptr ) {
        // Stopping: clean up the kernel-log capture and convert it to logcat time.
        cleanupAdbKmsgProcess();
        convertKmsgToLogcat();
        return;
    }

    startAdbCapture( QDir( QDir::tempPath() ).filePath( QStringLiteral( "klogg_adb_kmsg.log" ) ),
                     QStringList() << QStringLiteral( "shell" ) << QStringLiteral( "cat" )
                                   << QStringLiteral( "/dev/kmsg" ),
                     false, /*requireRoot=*/true, /*useKmsgSlot=*/true );
}

bool MainWindow::queryDeviceBootTime( double& bootEpochSec, int& tzOffsetSec )
{
    const QString adbExecutable = QStandardPaths::findExecutable( QStringLiteral( "adb" ) );
    if ( adbExecutable.isEmpty() || adbAuthorizedDevices( adbExecutable ).isEmpty() ) {
        return false;
    }

    QProcess p;
    p.start( adbExecutable, adbArgs( QStringList()
                                << QStringLiteral( "shell" )
                                << QStringLiteral( "date +%s.%N; cat /proc/uptime; date +%z" ) ) );
    if ( !p.waitForFinished( 15000 ) ) {
        return false;
    }
    const auto lines = QString::fromUtf8( p.readAllStandardOutput() )
                           .split( QLatin1Char( '\n' ), Qt::SkipEmptyParts );
    if ( lines.size() < 3 ) {
        return false;
    }
    const double nowEpochSec = lines[ 0 ].trimmed().toDouble();
    const double uptimeSec = lines[ 1 ].trimmed().section( QLatin1Char( ' ' ), 0, 0 ).toDouble();
    bootEpochSec = nowEpochSec - uptimeSec;

    // Parse timezone like "+0800" / "-0530".
    tzOffsetSec = 0;
    const QString tz = lines[ 2 ].trimmed();
    if ( tz.size() == 5 && ( tz[ 0 ] == QLatin1Char( '+' ) || tz[ 0 ] == QLatin1Char( '-' ) ) ) {
        const int hh = tz.mid( 1, 2 ).toInt();
        const int mm = tz.mid( 3, 2 ).toInt();
        tzOffsetSec = ( hh * 3600 + mm * 60 ) * ( tz[ 0 ] == QLatin1Char( '-' ) ? -1 : 1 );
    }
    return true;
}

bool MainWindow::convertKmsgFile( const QString& srcPath, const QString& dstPath,
                                  double bootEpochSec, int tzOffsetSec )
{
    QFile src( srcPath );
    QFile dst( dstPath );
    if ( !src.open( QIODevice::ReadOnly ) || !dst.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        return false;
    }

    // Map syslog severity (priority % 8) to a logcat level letter.
    const auto levelForPriority = []( int priority ) -> char {
        switch ( priority & 7 ) {
        case 0:
        case 1:
        case 2:
            return 'F';
        case 3:
            return 'E';
        case 4:
            return 'W';
        case 7:
            return 'D';
        default:
            return 'I';
        }
    };

    // Right-justify a non-negative integer into width columns (space padded).
    const auto appendPadded = []( QByteArray& b, int value, int width ) {
        const QByteArray num = QByteArray::number( value );
        for ( int i = num.size(); i < width; ++i ) {
            b += ' ';
        }
        b += num;
    };

    // The "MM-dd HH:mm:ss" part only changes once per second, while kmsg emits
    // many lines per second - cache it and reformat just the milliseconds.
    qint64 cachedSecond = -1;
    QByteArray cachedSecondPrefix;

    QByteArray out;
    out.reserve( 1 << 20 );

    while ( !src.atEnd() ) {
        const QByteArray line = src.readLine(); // keeps the trailing '\n'
        const char* const d = line.constData();
        const int len = line.size();

        // kmsg record: "<prio>,<seq>,<ts_us>,<flags>[,key=val...];<message>".
        // Continuation lines (multi-line records) start with whitespace - keep raw.
        const int semi = line.indexOf( ';' );
        if ( semi < 0 || ( len > 0 && ( d[ 0 ] == ' ' || d[ 0 ] == '\t' ) ) ) {
            out += line;
            if ( len == 0 || d[ len - 1 ] != '\n' ) {
                out += '\n';
            }
            continue;
        }

        // Field boundaries before ';': prio , seq , ts_us , flags ...
        const int c1 = line.indexOf( ',' );
        const int c2 = c1 >= 0 ? line.indexOf( ',', c1 + 1 ) : -1;
        if ( c1 < 0 || c2 < 0 || c2 >= semi ) {
            out += line;
            if ( len == 0 || d[ len - 1 ] != '\n' ) {
                out += '\n';
            }
            continue;
        }
        const int c3 = line.indexOf( ',', c2 + 1 );
        const int tsEnd = ( c3 >= 0 && c3 < semi ) ? c3 : semi;

        const int priority = line.left( c1 ).toInt();
        const qint64 tsUs = line.mid( c2 + 1, tsEnd - ( c2 + 1 ) ).toLongLong();

        // Extract the caller thread id ("caller=T<num>") for the pid/tid columns.
        int tid = 0;
        const int callerAt = line.indexOf( "=T", tsEnd );
        if ( callerAt >= 0 && callerAt < semi ) {
            int p = callerAt + 2;
            while ( p < semi && d[ p ] >= '0' && d[ p ] <= '9' ) {
                tid = tid * 10 + ( d[ p ] - '0' );
                ++p;
            }
        }

        const double lineEpochSec = bootEpochSec + static_cast<double>( tsUs ) / 1e6;
        const qint64 displayMs
            = static_cast<qint64>( ( lineEpochSec + tzOffsetSec ) * 1000.0 + 0.5 );
        const qint64 second = displayMs / 1000;
        if ( second != cachedSecond ) {
            cachedSecond = second;
            cachedSecondPrefix = QDateTime::fromMSecsSinceEpoch( second * 1000, Qt::UTC )
                                     .toString( QStringLiteral( "MM-dd HH:mm:ss" ) )
                                     .toLatin1();
        }
        const int ms = static_cast<int>( displayMs % 1000 );

        out += cachedSecondPrefix;
        out += '.';
        out += static_cast<char>( '0' + ( ms / 100 ) % 10 );
        out += static_cast<char>( '0' + ( ms / 10 ) % 10 );
        out += static_cast<char>( '0' + ms % 10 );
        out += "  ";
        appendPadded( out, tid, 5 );
        out += ' ';
        appendPadded( out, tid, 5 );
        out += ' ';
        out += levelForPriority( priority );
        out += " kernel: ";
        out.append( d + semi + 1, len - ( semi + 1 ) ); // message (keeps its '\n')
        if ( len == 0 || d[ len - 1 ] != '\n' ) {
            out += '\n';
        }

        if ( out.size() >= ( 1 << 20 ) ) {
            dst.write( out );
            out.clear();
        }
    }
    if ( !out.isEmpty() ) {
        dst.write( out );
    }
    return true;
}

void MainWindow::convertKmsgToLogcat()
{
    // Convert the /dev/kmsg capture (monotonic microseconds since boot) into
    // logcat threadtime format with wall-clock timestamps, like `dmesg -T`.
    const QString srcPath
        = QDir( QDir::tempPath() ).filePath( QStringLiteral( "klogg_adb_kmsg.log" ) );
    QFileInfo srcInfo( srcPath );
    if ( !srcInfo.exists() || srcInfo.size() == 0 ) {
        return;
    }

    // Carry the current tab's search text and color labels over to the converted tab.
    QString previousSearchText;
    ColorLabelsManager::QuickHighlightersCollection previousColorLabels;
    if ( auto* currentCrawler = currentCrawlerWidget() ) {
        previousSearchText = currentCrawler->currentSearchText();
        previousColorLabels = currentCrawler->currentColorLabels();
    }

    // Derive the device boot wall-clock time. Requires the (same, not rebooted) device.
    double bootEpochSec = 0.0;
    int tzOffsetSec = 0;
    if ( !queryDeviceBootTime( bootEpochSec, tzOffsetSec ) ) {
        QMessageBox::warning(
            this, tr( "klogg" ),
            tr( "A connected device is required to compute wall-clock time for the kernel log." ) );
        return;
    }

    const QString dstPath = QDir( QDir::tempPath() ).filePath( QStringLiteral( "kmsg.newT.txt" ) );
    if ( !convertKmsgFile( srcPath, dstPath, bootEpochSec, tzOffsetSec ) ) {
        QMessageBox::critical( this, tr( "klogg" ),
                               tr( "Could not convert the kernel log." ) );
        return;
    }

    if ( !loadFile( dstPath ) ) {
        return;
    }

    // Auto-run the search with the carried-over keyword on the converted tab.
    if ( auto* crawler = currentCrawlerWidget() ) {
        crawler->setMatchCase( false );
        crawler->startSearchWithAutoRefresh( previousSearchText );
        if ( !previousColorLabels.empty() ) {
            crawler->restoreColorLabels( previousColorLabels );
        }
    }
}

namespace {
// Logcat threadtime line prefix: "MM-dd HH:mm:ss.zzz" (18 chars). Sorting by this
// text is chronological within a year (lexicographic == time order).
const QRegularExpression& logcatTimestampRegex()
{
    static const QRegularExpression re(
        QStringLiteral( "^\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3}" ) );
    return re;
}
// Raw kmsg line prefix: "<prio>,<seq>,<ts_us>,".
const QRegularExpression& kmsgLineRegex()
{
    static const QRegularExpression re( QStringLiteral( "^\\d+,\\d+,\\d+," ) );
    return re;
}

constexpr int LogcatTimestampLength = 18;

// Fast check whether a raw line begins with a logcat threadtime timestamp
// "MM-dd HH:mm:ss.zzz" (ASCII, evaluated directly on the file bytes).
bool hasLogcatTimestamp( const char* s, int len )
{
    if ( len < LogcatTimestampLength ) {
        return false;
    }
    const auto d = [ s ]( int i ) { return s[ i ] >= '0' && s[ i ] <= '9'; };
    return d( 0 ) && d( 1 ) && s[ 2 ] == '-' && d( 3 ) && d( 4 ) && s[ 5 ] == ' ' && d( 6 )
           && d( 7 ) && s[ 8 ] == ':' && d( 9 ) && d( 10 ) && s[ 11 ] == ':' && d( 12 ) && d( 13 )
           && s[ 14 ] == '.' && d( 15 ) && d( 16 ) && d( 17 );
}
} // namespace

void MainWindow::mergeOpenFilesOffline()
{
    const QString tempDir = QDir::tempPath();
    const QString offlinePath = QDir( tempDir ).filePath( QStringLiteral( "offline.txt" ) );

    const auto openedFiles = session_.openedFiles();

    // Carry the current tab's search text and color labels over to the merged tab.
    QString previousSearchText;
    ColorLabelsManager::QuickHighlightersCollection previousColorLabels;
    if ( auto* currentCrawler = currentCrawlerWidget() ) {
        previousSearchText = currentCrawler->currentSearchText();
        previousColorLabels = currentCrawler->currentColorLabels();
    }

    const auto baseName = []( const QString& p ) { return QFileInfo( p ).fileName(); };

    // If the converted kernel log is present, skip the raw kmsg capture.
    bool hasConvertedKmsg = false;
    for ( const auto& f : openedFiles ) {
        if ( baseName( f ) == QLatin1String( "kmsg.newT.txt" ) ) {
            hasConvertedKmsg = true;
            break;
        }
    }

    QStringList sources;
    for ( const auto& f : openedFiles ) {
        const QString name = baseName( f );
        if ( name == QLatin1String( "offline.txt" ) ) {
            continue; // never merge the target into itself
        }
        if ( hasConvertedKmsg && name == QLatin1String( "klogg_adb_kmsg.log" ) ) {
            continue; // superseded by kmsg.newT.txt
        }
        sources << f;
    }

    if ( sources.isEmpty() ) {
        QMessageBox::information( this, tr( "klogg" ), tr( "No open files to merge." ) );
        return;
    }

    // Resolve each source to a logcat-timestamped file, converting raw kmsg captures.
    bool haveBootTime = false;
    double bootEpochSec = 0.0;
    int tzOffsetSec = 0;
    QStringList resolved;
    QStringList tempFiles;
    for ( const auto& f : sources ) {
        // Sample the first lines to detect the format.
        bool isLogcat = false;
        bool isKmsg = false;
        QFile probe( f );
        if ( probe.open( QIODevice::ReadOnly | QIODevice::Text ) ) {
            QTextStream ps( &probe );
            for ( int i = 0; i < 50 && !ps.atEnd(); ++i ) {
                const QString line = ps.readLine();
                if ( line.isEmpty() ) {
                    continue;
                }
                if ( logcatTimestampRegex().match( line ).hasMatch() ) {
                    isLogcat = true;
                    break;
                }
                if ( kmsgLineRegex().match( line ).hasMatch() ) {
                    isKmsg = true;
                    break;
                }
            }
        }

        if ( isKmsg && !isLogcat ) {
            if ( !haveBootTime ) {
                if ( !queryDeviceBootTime( bootEpochSec, tzOffsetSec ) ) {
                    QMessageBox::warning(
                        this, tr( "klogg" ),
                        tr( "A connected device is required to convert the kernel log \"%1\"; "
                            "skipping it." )
                            .arg( baseName( f ) ) );
                    continue;
                }
                haveBootTime = true;
            }
            const QString tmp = QDir( tempDir ).filePath( baseName( f )
                                                          + QStringLiteral( ".logcatT.tmp" ) );
            if ( convertKmsgFile( f, tmp, bootEpochSec, tzOffsetSec ) ) {
                resolved << tmp;
                tempFiles << tmp;
            }
        }
        else {
            resolved << f; // logcat format (or unknown - included as-is)
        }
    }

    if ( resolved.isEmpty() ) {
        QMessageBox::information( this, tr( "klogg" ), tr( "Nothing to merge." ) );
        return;
    }

    // Read every line from all resolved sources, tagging each with the timestamp
    // of its record: lines without a timestamp inherit the previous line's key.
    // A global stable sort then yields correct chronological order even when an
    // input file is not internally sorted (e.g. logcat printed buffer-by-buffer
    // with "--------- beginning of <buffer>" markers).
    //
    // For efficiency each file is read into a single buffer and lines are stored
    // as lightweight views (pointer + length) into those buffers - no per-line
    // allocation or copy. The buffers must outlive the entries, so all files are
    // read first, then indexed, sorted and written.
    struct LineView {
        const char* keyPtr;
        int keyLen;
        const char* linePtr;
        int lineLen;
    };

    qint64 totalBytes = 0;
    for ( const auto& f : resolved ) {
        totalBytes += QFileInfo( f ).size();
    }

    QProgressDialog progress( tr( "Merging open files into offline.txt..." ), tr( "Cancel" ), 0,
                              100, this );
    progress.setWindowModality( Qt::WindowModal );
    progress.setMinimumDuration( 500 );

    const auto cleanupTemps = [ &tempFiles ]() {
        for ( const auto& tmp : tempFiles ) {
            QFile::remove( tmp );
        }
    };

    // Read all sources fully into buffers (kept alive for the whole merge).
    std::vector<QByteArray> buffers;
    buffers.reserve( static_cast<size_t>( resolved.size() ) );
    qint64 bytesRead = 0;
    for ( const auto& f : resolved ) {
        QFile in( f );
        buffers.push_back( in.open( QIODevice::ReadOnly ) ? in.readAll() : QByteArray() );
        bytesRead += buffers.back().size();
        progress.setValue( totalBytes > 0 ? static_cast<int>( bytesRead * 70 / totalBytes ) : 70 );
        if ( progress.wasCanceled() ) {
            cleanupTemps();
            return;
        }
    }

    // Index every line as a view into its buffer.
    std::vector<LineView> entries;
    for ( const auto& buf : buffers ) {
        const char* const bufData = buf.constData();
        const int size = buf.size();
        const char* keyPtr = bufData;
        int keyLen = 0; // empty key (sorts first) until a timestamped line is seen
        int pos = 0;
        while ( pos < size ) {
            const char* nl = static_cast<const char*>(
                memchr( bufData + pos, '\n', static_cast<size_t>( size - pos ) ) );
            const int lineLen
                = nl ? static_cast<int>( nl - ( bufData + pos ) ) + 1 : ( size - pos );
            if ( hasLogcatTimestamp( bufData + pos, lineLen ) ) {
                keyPtr = bufData + pos;
                keyLen = LogcatTimestampLength;
            }
            entries.push_back( { keyPtr, keyLen, bufData + pos, lineLen } );
            pos += lineLen;
        }
    }

    // Global stable sort by timestamp key ("" sorts first). Stable keeps
    // equal-timestamp lines and continuation lines in their original order.
    progress.setValue( 80 );
    std::stable_sort( entries.begin(), entries.end(), []( const LineView& a, const LineView& b ) {
        const int m = std::min( a.keyLen, b.keyLen );
        const int c = m ? memcmp( a.keyPtr, b.keyPtr, static_cast<size_t>( m ) ) : 0;
        if ( c != 0 ) {
            return c < 0;
        }
        return a.keyLen < b.keyLen;
    } );
    progress.setValue( 90 );

    QFile out( offlinePath );
    if ( !out.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        QMessageBox::critical( this, tr( "klogg" ),
                               tr( "Could not write merged log:\n%1" ).arg( offlinePath ) );
        cleanupTemps();
        return;
    }
    QByteArray outBuf;
    outBuf.reserve( 1 << 20 );
    for ( const auto& e : entries ) {
        outBuf.append( e.linePtr, e.lineLen );
        if ( e.lineLen == 0 || e.linePtr[ e.lineLen - 1 ] != '\n' ) {
            outBuf.append( '\n' );
        }
        if ( outBuf.size() >= ( 1 << 20 ) ) {
            out.write( outBuf );
            outBuf.clear();
        }
    }
    if ( !outBuf.isEmpty() ) {
        out.write( outBuf );
    }
    out.close();
    progress.setValue( 100 );

    cleanupTemps();

    // If offline.txt is already open, close that tab so the fresh content loads.
    if ( auto* existingView
         = static_cast<CrawlerWidget*>( session_.getViewIfOpen( offlinePath ) ) ) {
        const int tabIndex = mainTabWidget_.indexOf( existingView );
        if ( tabIndex >= 0 ) {
            closeTab( tabIndex, ActionInitiator::App );
        }
    }

    if ( !loadFile( offlinePath ) ) {
        return;
    }

    // Auto-run the search with the carried-over keyword on the merged tab.
    if ( auto* crawler = currentCrawlerWidget() ) {
        crawler->setMatchCase( false );
        crawler->startSearchWithAutoRefresh( previousSearchText );
        if ( !previousColorLabels.empty() ) {
            crawler->restoreColorLabels( previousColorLabels );
        }
    }
}


bool MainWindow::ensureAdbDevice( const QString& adbExecutable )
{
    const QStringList devices = adbAuthorizedDevices( adbExecutable );
    if ( devices.isEmpty() ) {
        adbSerial_.clear();
        QMessageBox::warning(
            this, tr( "klogg" ),
            tr( "No authorized device found. Connect a device and check \"adb devices\"." ) );
        return false;
    }

    // Reuse a previous choice while that device is still attached.
    if ( !adbSerial_.isEmpty() && devices.contains( adbSerial_ ) ) {
        return true;
    }

    // A single device needs no `-s`; target it implicitly.
    if ( devices.size() == 1 ) {
        adbSerial_ = devices.first();
        return true;
    }

    // Several devices attached: let the user pick which one to talk to.
    bool ok = false;
    const int current = devices.indexOf( adbSerial_ );
    const QString chosen = QInputDialog::getItem(
        this, tr( "Select ADB device" ),
        tr( "Multiple devices detected. Choose the device for ADB commands:" ), devices,
        current >= 0 ? current : 0, /*editable=*/false, &ok );
    if ( !ok || chosen.isEmpty() ) {
        return false;
    }
    adbSerial_ = chosen;
    return true;
}

QStringList MainWindow::adbArgs( const QStringList& subCommand ) const
{
    QStringList args;
    if ( !adbSerial_.isEmpty() ) {
        args << QStringLiteral( "-s" ) << adbSerial_;
    }
    args << subCommand;
    return args;
}

// One adb command run sequentially by runAdbSequence(). validate() decides whether
// to continue to the next step (true) or abort the whole sequence (false); it also
// pops any error dialog. timedOut signals that the watchdog fired for this step.
struct AdbStep {
    QStringList args;
    int timeoutMs;
    std::function<bool( QProcess&, bool timedOut )> validate;
};

// Heap state kept alive across the async step chain (see runAdbSequence). Owned as
// a raw pointer and deleted exactly once in finishAdbSequence(); the QObject members
// are parented to the MainWindow and torn down via deleteLater().
struct AdbSequenceContext {
    QString adbExecutable;
    std::vector<AdbStep> steps;
    std::function<void()> onSuccess;
    size_t index = 0;
    QProcess* proc = nullptr;
    QTimer* watchdog = nullptr;
    QProgressDialog* dialog = nullptr;
    bool stepHandled = false; // guards finished-vs-watchdog double handling per step
    bool done = false;        // guards finishAdbSequence running more than once
};

void MainWindow::runAdbSequence( const QString& adbExecutable, const QString& busyLabel,
                                 std::vector<AdbStep> steps, std::function<void()> onSuccess )
{
    if ( steps.empty() ) {
        if ( onSuccess ) {
            onSuccess();
        }
        return;
    }

    auto* ctx = new AdbSequenceContext;
    ctx->adbExecutable = adbExecutable;
    ctx->steps = std::move( steps );
    ctx->onSuccess = std::move( onSuccess );
    ctx->proc = new QProcess( this );
    ctx->watchdog = new QTimer( this );
    ctx->watchdog->setSingleShot( true );

    // Busy (0,0) modal dialog: keeps the UI painting/cancelable and blocks a second
    // F1/F4/F6 trigger from starting an overlapping sequence.
    ctx->dialog = new QProgressDialog( busyLabel, tr( "Cancel" ), 0, 0, this );
    ctx->dialog->setWindowModality( Qt::ApplicationModal );
    ctx->dialog->setMinimumDuration( 0 );
    ctx->dialog->setAutoClose( false );
    ctx->dialog->setAutoReset( false );

    // A completed step: stop the watchdog, let validate() decide, then advance.
    const auto handleStep = [ this, ctx ]( bool timedOut ) {
        if ( ctx->stepHandled ) {
            return;
        }
        ctx->stepHandled = true;
        ctx->watchdog->stop();

        const AdbStep& step = ctx->steps[ ctx->index ];
        const bool proceed = step.validate ? step.validate( *ctx->proc, timedOut ) : true;
        if ( !proceed ) {
            finishAdbSequence( ctx, false );
            return;
        }
        ++ctx->index;
        if ( ctx->index >= ctx->steps.size() ) {
            finishAdbSequence( ctx, true );
            return;
        }
        advanceAdbSequence( ctx );
    };

    connect( ctx->proc,
             static_cast<void ( QProcess::* )( int, QProcess::ExitStatus )>( &QProcess::finished ),
             this, [ handleStep ]( int, QProcess::ExitStatus ) { handleStep( false ); } );
    // FailedToStart (no finished signal) also flows here so the chain never hangs.
    connect( ctx->proc, &QProcess::errorOccurred, this,
             [ handleStep ]( QProcess::ProcessError ) { handleStep( false ); } );
    connect( ctx->watchdog, &QTimer::timeout, this, [ this, ctx, handleStep ]() {
        if ( ctx->proc->state() != QProcess::NotRunning ) {
            ctx->proc->kill();
        }
        handleStep( true );
    } );
    connect( ctx->dialog, &QProgressDialog::canceled, this,
             [ this, ctx ]() { finishAdbSequence( ctx, false ); } );

    ctx->dialog->show();
    advanceAdbSequence( ctx );
}

void MainWindow::advanceAdbSequence( AdbSequenceContext* ctx )
{
    ctx->stepHandled = false;
    const AdbStep& step = ctx->steps[ ctx->index ];
    ctx->watchdog->start( step.timeoutMs );
    ctx->proc->start( ctx->adbExecutable, adbArgs( step.args ) );
}

void MainWindow::finishAdbSequence( AdbSequenceContext* ctx, bool ok )
{
    if ( ctx->done ) {
        return;
    }
    ctx->done = true;

    ctx->watchdog->stop();
    ctx->proc->disconnect();
    if ( ctx->proc->state() != QProcess::NotRunning ) {
        ctx->proc->kill();
        ctx->proc->waitForFinished( 200 );
    }
    ctx->proc->deleteLater();
    ctx->watchdog->deleteLater();
    ctx->dialog->deleteLater();

    auto onSuccess = std::move( ctx->onSuccess );
    delete ctx;

    if ( ok && onSuccess ) {
        onSuccess();
    }
}

void MainWindow::startAdbCapture( const QString& logPath, const QStringList& captureArgs,
                                  bool prepareLogcatBuffer, bool requireRoot, bool useKmsgSlot )
{
    // Use the separate F6 kernel-log slot or the F1/F2 logcat slot.
    if ( ( useKmsgSlot ? adbKmsgProcess_ : adbLogcatProcess_ ) != nullptr ) {
        return;
    }

    const QString adbExecutable = QStandardPaths::findExecutable( QStringLiteral( "adb" ) );
    if ( adbExecutable.isEmpty() ) {
        QMessageBox::warning( this, tr( "klogg" ),
                              tr( "Could not find adb in PATH. Install Android platform-tools." ) );
        return;
    }

    if ( !ensureAdbDevice( adbExecutable ) ) {
        return;
    }

    const auto& config = Configuration::get();
    if ( !config.anyFileWatchEnabled() ) {
        QMessageBox::information(
            this, tr( "klogg" ),
            tr( "File change monitoring is disabled in Preferences. "
                "Enable \"Native file watch\" or \"Polling\" so the log view can follow new lines." ) );
    }

    // Save search text and color labels from the current tab so we can carry them to the new tab
    QString previousSearchText;
    ColorLabelsManager::QuickHighlightersCollection previousColorLabels;
    if ( auto* currentCrawler = currentCrawlerWidget() ) {
        previousSearchText = currentCrawler->currentSearchText();
        previousColorLabels = currentCrawler->currentColorLabels();
    }

    // Build the preparation commands. These used to run synchronously with
    // waitForFinished(), freezing the UI for up to ~35s (logcat) / ~40s (kmsg)
    // on a misbehaving device. They now run one at a time via runAdbSequence()
    // behind a cancelable busy dialog; the actual capture starts in onSuccess.
    std::vector<AdbStep> steps;
    if ( requireRoot ) {
        // Restart adbd as root (needed to read /dev/kmsg). Best-effort: a failure
        // or timeout here is not fatal, capture may still work.
        steps.push_back( { QStringList() << QStringLiteral( "root" ), 15000, nullptr } );
        // Restarting adbd drops and re-establishes the transport, so wait for the
        // device to come back before issuing the capture command.
        steps.push_back(
            { QStringList() << QStringLiteral( "wait-for-device" ), 20000,
              [ this ]( QProcess&, bool timedOut ) {
                  if ( timedOut ) {
                      QMessageBox::critical( this, tr( "klogg" ),
                                             tr( "Timed out waiting for device after adb root." ) );
                      return false;
                  }
                  return true;
              } } );
    }

    if ( prepareLogcatBuffer ) {
        // Increase device log buffer size before capturing (best-effort).
        // Some buffers (e.g. kernel) may not be resizable on all devices, which makes
        // adb return non-zero even when the other buffers were enlarged, so we don't
        // treat a failure here as fatal - capture can still proceed.
        steps.push_back( { QStringList() << QStringLiteral( "logcat" ) << QStringLiteral( "-G" )
                                         << QStringLiteral( "512M" ),
                           15000, nullptr } );
        // Clear device log buffer.
        steps.push_back(
            { QStringList() << QStringLiteral( "logcat" ) << QStringLiteral( "-c" ), 15000,
              [ this ]( QProcess& proc, bool timedOut ) {
                  if ( timedOut ) {
                      QMessageBox::critical( this, tr( "klogg" ), tr( "adb logcat -c timed out." ) );
                      return false;
                  }
                  if ( proc.exitCode() != 0 ) {
                      const auto err = QString::fromUtf8( proc.readAllStandardError() );
                      QMessageBox::critical( this, tr( "klogg" ),
                                             tr( "adb logcat -c failed:\n%1" ).arg( err ) );
                      return false;
                  }
                  return true;
              } } );
    }

    // The real capture: unchanged from the previous synchronous implementation,
    // just relocated into the completion callback. adb output is written straight
    // to the file by the kernel (no Qt event loop per line); the tab follows it.
    auto onSuccess = [ this, adbExecutable, logPath, captureArgs, useKmsgSlot, previousSearchText,
                       previousColorLabels ]() {
        QProcess*& proc = useKmsgSlot ? adbKmsgProcess_ : adbLogcatProcess_;
        QString& filePathRef = useKmsgSlot ? adbKmsgFilePath_ : adbLogcatFilePath_;

        // If the file is already open in a tab, close that tab first.
        auto* existingView = static_cast<CrawlerWidget*>( session_.getViewIfOpen( logPath ) );
        if ( existingView ) {
            int tabIndex = mainTabWidget_.indexOf( existingView );
            if ( tabIndex >= 0 ) {
                closeTab( tabIndex, ActionInitiator::App );
            }
        }

        filePathRef = logPath;

        // Start adb process - write directly to file via kernel (no Qt event loop bottleneck)
        proc = new QProcess( this );
        proc->setStandardOutputFile( logPath, QIODevice::Truncate );
        proc->start( adbExecutable, adbArgs( captureArgs ) );
        if ( !proc->waitForStarted( 5000 ) ) {
            QMessageBox::critical( this, tr( "klogg" ), tr( "Could not start adb capture." ) );
            proc->deleteLater();
            proc = nullptr;
            return;
        }

        // Open the file in klogg with follow mode
        if ( !loadFile( logPath, true ) ) {
            if ( useKmsgSlot ) {
                cleanupAdbKmsgProcess();
            }
            else {
                cleanupAdbLogcatProcess();
            }
            return;
        }

        // Set search text from previous tab and enable auto-refresh for real-time filtering
        // Also restore color labels (Ctrl+D highlights) from previous tab
        if ( auto* crawler = currentCrawlerWidget() ) {
            // Always start the capture tab case-insensitive
            crawler->setMatchCase( false );
            crawler->startSearchWithAutoRefresh( previousSearchText );
            if ( !previousColorLabels.empty() ) {
                crawler->restoreColorLabels( previousColorLabels );
            }
        }

        // Colour the capturing file's tab green while the capture is running.
        mainTabWidget_.setTabColorForFile( logPath, QColor( 0x4c, 0xaf, 0x50 ) );

        // Only the F1/F2 logcat capture toggles those actions; F6 stays independent.
        if ( !useKmsgSlot ) {
            adbLogcatStartAction->setEnabled( false );
            adbLogcatStopAction->setEnabled( true );
        }
    };

    // Run the prep sequence (may be empty, in which case onSuccess fires immediately).
    runAdbSequence( adbExecutable, tr( "Preparing adb capture..." ), std::move( steps ),
                    std::move( onSuccess ) );
}

void MainWindow::quickSaveAdbLogcat()
{
    const QString logPath
        = QDir( QDir::tempPath() ).filePath( QStringLiteral( "klogg_adb_logcat.log" ) );
    QFileInfo fileInfo( logPath );
    if ( !fileInfo.exists() || fileInfo.size() == 0 ) {
        QMessageBox::information( this, tr( "klogg" ), tr( "No log file to save." ) );
        return;
    }

    const QString timestamp
        = QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_HHmmss" ) );

#if defined( Q_OS_LINUX )
    // On Linux save into a dedicated /tmp/0_klogg/ directory.
    QDir saveDir( QStringLiteral( "/tmp/0_klogg" ) );
    if ( !saveDir.exists() ) {
        saveDir.mkpath( QStringLiteral( "." ) );
    }
#elif defined( Q_OS_WIN )
    // On Windows save into D:\0_klogg (created if it does not exist).
    QDir saveDir( QStringLiteral( "D:/0_klogg" ) );
    if ( !saveDir.exists() ) {
        saveDir.mkpath( QStringLiteral( "." ) );
    }
#else
    QDir saveDir( QDir::tempPath() );
#endif
    const QString savePath
        = saveDir.filePath( QStringLiteral( "klogg_adb_logcat_%1.log" ).arg( timestamp ) );

    if ( !QFile::copy( logPath, savePath ) ) {
        QMessageBox::warning( this, tr( "klogg" ), tr( "Failed to save log file." ) );
        return;
    }

#if defined( Q_OS_LINUX )
    // Open the containing folder, and show the saved path in a popup that
    // counts down and auto-closes after a few seconds.
    showPathInFileExplorer( savePath );
    showAutoClosingSavedDialog( savePath );
#else
    // Open the containing folder and select the saved file.
    showPathInFileExplorer( savePath );
#endif
}

void MainWindow::killCameraAdb()
{
    const QString adbExecutable = QStandardPaths::findExecutable( QStringLiteral( "adb" ) );
    if ( adbExecutable.isEmpty() ) {
        QMessageBox::warning( this, tr( "klogg" ),
                              tr( "Could not find adb in PATH. Install Android platform-tools." ) );
        return;
    }

    if ( !ensureAdbDevice( adbExecutable ) ) {
        return;
    }

    std::vector<AdbStep> steps;
    // Restart adbd with root permissions (best-effort; may already be root or
    // unsupported on production builds). Not fatal on failure/timeout.
    steps.push_back( { QStringList() << QStringLiteral( "root" ), 15000, nullptr } );
    // Kill any process whose command line matches "camera".
    steps.push_back(
        { QStringList() << QStringLiteral( "shell" ) << QStringLiteral( "pkill" )
                        << QStringLiteral( "-f" ) << QStringLiteral( "camera" ),
          15000, [ this ]( QProcess& proc, bool timedOut ) {
              if ( timedOut ) {
                  QMessageBox::critical( this, tr( "klogg" ), tr( "adb shell pkill timed out." ) );
                  return false;
              }
              // pkill returns non-zero (1) when no process matched, which is not an
              // error worth surfacing; only report genuine execution failures, but
              // still refresh the label afterwards (return true).
              const int exitCode = proc.exitCode();
              if ( exitCode != 0 && exitCode != 1 ) {
                  const auto err = QString::fromUtf8( proc.readAllStandardError() );
                  QMessageBox::critical( this, tr( "klogg" ),
                                         tr( "adb shell pkill -f camera failed:\n%1" ).arg( err ) );
              }
              return true;
          } } );

    // The kill just changed the process table; refresh the F4 label right away.
    runAdbSequence( adbExecutable, tr( "Killing camera processes..." ), std::move( steps ),
                    [ this ]() { updateCameraProviderPid(); } );
}

void MainWindow::updateCameraProviderPid()
{
    // Skip if a previous query is still running to avoid piling up processes.
    if ( cameraPidProcess_ && cameraPidProcess_->state() != QProcess::NotRunning ) {
        return;
    }

    const QString adbExecutable = QStandardPaths::findExecutable( QStringLiteral( "adb" ) );
    if ( adbExecutable.isEmpty() ) {
        applyCameraLabel( QString(), QString() );
        return;
    }

    if ( !cameraPidProcess_ ) {
        cameraPidProcess_ = new QProcess( this );
        connect(
            cameraPidProcess_,
            static_cast<void ( QProcess::* )( int, QProcess::ExitStatus )>( &QProcess::finished ),
            this, [ this ]( int, QProcess::ExitStatus ) {
                const auto output
                    = QString::fromUtf8( cameraPidProcess_->readAllStandardOutput() );
                QString pid;
                const auto lines = output.split( QLatin1Char( '\n' ) );
                for ( const auto& line : lines ) {
                    if ( !line.contains( QStringLiteral( "camera.provider" ),
                                         Qt::CaseInsensitive ) ) {
                        continue;
                    }
                    // ps -ef columns: UID PID PPID ... ; the PID is the 2nd field.
                    const auto fields = line.simplified().split( QLatin1Char( ' ' ) );
                    if ( fields.size() >= 2 ) {
                        pid = fields.at( 1 );
                        break;
                    }
                }
                cameraProviderPid_ = pid;
                // Chain the device-count query so both values land in one label.
                updateCameraDeviceCount();
            } );
    }

    cameraPidProcess_->start( adbExecutable, adbArgs( QStringList() << QStringLiteral( "shell" )
                                                                    << QStringLiteral( "ps" )
                                                                    << QStringLiteral( "-ef" ) ) );
}

void MainWindow::updateCameraDeviceCount()
{
    // Skip if a previous query is still running to avoid piling up processes.
    if ( cameraDeviceProcess_ && cameraDeviceProcess_->state() != QProcess::NotRunning ) {
        return;
    }

    const QString adbExecutable = QStandardPaths::findExecutable( QStringLiteral( "adb" ) );
    if ( adbExecutable.isEmpty() ) {
        applyCameraLabel( cameraProviderPid_, QString() );
        return;
    }

    if ( !cameraDeviceProcess_ ) {
        cameraDeviceProcess_ = new QProcess( this );
        connect(
            cameraDeviceProcess_,
            static_cast<void ( QProcess::* )( int, QProcess::ExitStatus )>( &QProcess::finished ),
            this, [ this ]( int, QProcess::ExitStatus ) {
                const auto output
                    = QString::fromUtf8( cameraDeviceProcess_->readAllStandardOutput() );
                // Expected line: "Number of camera devices: 2". Take the digits
                // after the last colon; anything non-numeric falls back to "xxx".
                QString count;
                const auto lines = output.split( QLatin1Char( '\n' ) );
                for ( const auto& line : lines ) {
                    if ( !line.contains( QStringLiteral( "Number of camera devices" ),
                                         Qt::CaseInsensitive ) ) {
                        continue;
                    }
                    const int colon = line.lastIndexOf( QLatin1Char( ':' ) );
                    if ( colon >= 0 ) {
                        count = line.mid( colon + 1 ).trimmed();
                    }
                    break;
                }
                applyCameraLabel( cameraProviderPid_, count );
            } );
    }

    // Run the pipe on the device's shell as a single argument.
    cameraDeviceProcess_->start(
        adbExecutable,
        adbArgs( QStringList()
                 << QStringLiteral( "shell" )
                 << QStringLiteral(
                        "dumpsys media.camera | grep -iE 'Number of camera devices'" ) ) );
}

void MainWindow::applyCameraLabel( const QString& pid, const QString& deviceCount )
{
    const QString pidShown = pid.isEmpty() ? QStringLiteral( "NULL-" ) : pid;

    // Only accept a plain Arabic-numeral count; otherwise show the literal "xxx".
    bool numeric = !deviceCount.isEmpty();
    for ( const QChar ch : deviceCount ) {
        if ( ch < QLatin1Char( '0' ) || ch > QLatin1Char( '9' ) ) {
            numeric = false;
            break;
        }
    }
    const QString countShown = numeric ? deviceCount : QStringLiteral( "xxx" );

    adbKillCameraAction->setText(
        tr( "Kill Camera(F4) [%1-%2]" ).arg( pidShown, countShown ) );
}

void MainWindow::showAutoClosingSavedDialog( const QString& savePath )
{
    static constexpr int kCountdownSeconds = 2;

    auto* box = new QMessageBox( QMessageBox::Information, tr( "klogg" ),
                                 tr( "Log saved to:\n%1" ).arg( QDir::toNativeSeparators( savePath ) ),
                                 QMessageBox::Ok, this );
    box->setAttribute( Qt::WA_DeleteOnClose );

    const auto updateCountdown = [ box ]( int remaining ) {
        box->setInformativeText(
            tr( "This dialog will close automatically in %1 s" ).arg( remaining ) );
    };
    updateCountdown( kCountdownSeconds );

    auto* timer = new QTimer( box );
    box->setProperty( "remainingSeconds", kCountdownSeconds );
    connect( timer, &QTimer::timeout, box, [ box, timer, updateCountdown ]() {
        const int remaining = box->property( "remainingSeconds" ).toInt() - 1;
        box->setProperty( "remainingSeconds", remaining );
        if ( remaining <= 0 ) {
            timer->stop();
            box->accept();
        }
        else {
            updateCountdown( remaining );
        }
    } );
    timer->start( 1000 );

    box->show();
}

void MainWindow::onColorLabelsChanged(
    const ColorLabelsManager::QuickHighlightersCollection& labels )
{
    globalColorLabels_ = labels;

    // Apply to all other tabs
    auto* senderCrawler = qobject_cast<CrawlerWidget*>( sender() );
    for ( int i = 0; i < mainTabWidget_.count(); ++i ) {
        auto* crawler = qobject_cast<CrawlerWidget*>( mainTabWidget_.widget( i ) );
        if ( crawler && crawler != senderCrawler ) {
            crawler->restoreColorLabels( labels );
        }
    }
}
