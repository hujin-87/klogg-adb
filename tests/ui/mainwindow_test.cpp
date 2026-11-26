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

#include <catch2/catch.hpp>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <QToolBar>
#include <QTreeWidget>

#include "test_utils.h"

#include "log.h"
#include "mainwindow.h"
#include "session.h"
#include "workspaceexplorer.h"

SCENARIO( "Main window tests", "[ui]" )
{
    auto appSession = std::make_shared<Session>();
    WindowSession windowSession{ appSession, "Main", 0 };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SafeQSignalSpy> activateSpy;
    std::unique_ptr<SafeQSignalSpy> exitSpy;
    QTimer::singleShot( 0, [&] {
        LOG_INFO << "Initialize main window";
        mainWindow.reset( new MainWindow( windowSession ) );
        exitSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( exitRequested() ) ) );
        activateSpy.reset( new SafeQSignalSpy( mainWindow.get(), SIGNAL( windowActivated() ) ) );
    } );

    QTest::qWait( 100 );
    mainWindow->show();
    QTest::qWait( 100 );
    REQUIRE( activateSpy->safeWait() );

    auto runInUiThread = [uiObject = mainWindow.get()]( auto&& func ) {
        QTimer::singleShot( 0, Qt::VeryCoarseTimer, uiObject,
                            std::forward<decltype( func )>( func ) );
        QTest::qWait( 100 );
    };

    GIVEN( "Opened main window" )
    {
        auto toolBar = mainWindow->findChild<QToolBar*>();
        REQUIRE( toolBar != nullptr );

        auto filePathLabel = toolBar->findChild<PathLine*>();
        REQUIRE( filePathLabel != nullptr );

        auto tabArea = mainWindow->findChild<TabbedCrawlerWidget*>();
        REQUIRE( tabArea != nullptr );

        THEN( "Has no tabs" )
        {
            REQUIRE( tabArea->count() == 0 );
            AND_THEN( "Path label empty" )
            {
                REQUIRE( filePathLabel->text().isEmpty() );
            }
        }

        WHEN( "Exit hotkey pressed" )
        {
            runInUiThread( [&mainWindow] {
                LOG_INFO << "ExitFromMainMenu";
                QTest::keyPress( mainWindow.get(), Qt::Key_Q, Qt::ControlModifier );
            } );

            THEN( "Exit signalled" )
            {
                REQUIRE( exitSpy->safeWait() );
            }
        }

        WHEN( "Load file" )
        {
            runInUiThread( [&mainWindow] {
                LOG_INFO << "Load file";
                mainWindow->loadInitialFile( "klogg.conf", false );
            } );

            THEN( "Path line has file name" )
            {
                REQUIRE(
                    waitUiState( [&] { return filePathLabel->text().contains( "klogg.conf" ); } ) );

                AND_THEN( "Has one tab" )
                {
                    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
                }
            }

            AND_WHEN( "Close tab hotkey pressed" )
            {
                runInUiThread( [&mainWindow] {
                    LOG_INFO << "Close tab";
                    QTest::keyPress( mainWindow.get(), Qt::Key_W, Qt::ControlModifier );
                } );

                THEN( "Has no tabs" )
                {
                    REQUIRE( waitUiState( [&] { return tabArea->count() == 0; } ) );

                    AND_THEN( "Path label empty" )
                    {
                        REQUIRE( waitUiState( [&] { return filePathLabel->text().isEmpty(); } ) );
                    }
                }
            }
        }
        WHEN( "Open a folder as workspace" )
        {
            QTemporaryDir workspaceDir;
            REQUIRE( workspaceDir.isValid() );
            REQUIRE( QFile( workspaceDir.filePath( "a.log" ) ).open( QIODevice::WriteOnly ) );

            runInUiThread( [&mainWindow, dir = workspaceDir.path()] {
                mainWindow->openFolderAsWorkspace( dir );
            } );

            auto* explorer = mainWindow->findChild<WorkspaceExplorer*>();
            REQUIRE( explorer != nullptr );
            auto* tree = explorer->findChild<QTreeWidget*>();
            REQUIRE( tree != nullptr );

            THEN( "Explorer shows the folder as root and opens it" )
            {
                REQUIRE( waitUiState( [&] { return tree->topLevelItemCount() == 1; } ) );
                REQUIRE( tree->topLevelItem( 0 )->text( 0 )
                         == QFileInfo( workspaceDir.path() ).fileName() );
                REQUIRE( tree->topLevelItem( 0 )->isExpanded() );
                REQUIRE( explorer->isVisible() );

                AND_THEN( "Files can be opened from the tree" )
                {
                    // First child may be "a.log"; activating it opens a tab.
                    runInUiThread( [&] {
                        auto* root = tree->topLevelItem( 0 );
                        REQUIRE( root->childCount() == 1 );
                        QTest::mouseClick( tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                                           tree->visualItemRect( root->child( 0 ) ).center() );
                    } );
                    REQUIRE( waitUiState( [&] { return tabArea->count() == 1; } ) );
                }
            }

            AND_WHEN( "Close the workspace" )
            {
                runInUiThread( [&mainWindow] { mainWindow->closeWorkspace(); } );

                THEN( "Explorer is empty again" )
                {
                    REQUIRE( waitUiState( [&] { return tree->topLevelItemCount() == 0; } ) );
                }
            }
        }
    }
}