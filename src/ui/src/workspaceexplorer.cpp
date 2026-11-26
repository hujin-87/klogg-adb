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

#include "workspaceexplorer.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "clipboard.h"
#include "openfilehelper.h"

namespace {

QTreeWidgetItem* makePlaceholder( QTreeWidgetItem* parent )
{
    auto* placeholder = new QTreeWidgetItem( parent );
    placeholder->setFlags( Qt::NoItemFlags );
    placeholder->setText( 0, QStringLiteral( "..." ) );
    return placeholder;
}

} // namespace

WorkspaceExplorer::WorkspaceExplorer( QWidget* parent )
    : QWidget( parent )
    , tree_( new QTreeWidget( this ) )
    , emptyLabel_( new QLabel( this ) )
{
    tree_->setHeaderHidden( true );
    tree_->setColumnCount( 1 );
    tree_->setUniformRowHeights( true );
    tree_->setExpandsOnDoubleClick( true );
    tree_->setContextMenuPolicy( Qt::CustomContextMenu );
    tree_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    tree_->header()->setStretchLastSection( false );
    tree_->header()->setSectionResizeMode( QHeaderView::ResizeToContents );
    tree_->setHorizontalScrollBarPolicy( Qt::ScrollBarAsNeeded );

    emptyLabel_->setAlignment( Qt::AlignCenter );
    emptyLabel_->setWordWrap( true );
    emptyLabel_->setTextFormat( Qt::RichText );
    emptyLabel_->setTextInteractionFlags( Qt::TextBrowserInteraction );
    emptyLabel_->setOpenExternalLinks( false );
    connect( emptyLabel_, &QLabel::linkActivated, this, [ this ]( const QString& link ) {
        if ( link == QLatin1String( "open-folder" ) ) {
            Q_EMIT openFolderRequested();
        }
        else if ( link == QLatin1String( "open-workspace" ) ) {
            Q_EMIT openWorkspaceFileRequested();
        }
    } );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 0, 0, 0, 0 );
    layout->addWidget( emptyLabel_ );
    layout->addWidget( tree_ );

    connect( tree_, &QTreeWidget::itemExpanded, this, [ this ]( QTreeWidgetItem* item ) {
        if ( item->data( 0, IsDirRole ).toBool() && !item->data( 0, PopulatedRole ).toBool() ) {
            populate( item );
        }
    } );

    // Single click (VS Code behaviour) or Enter opens a file; folders toggle.
    connect( tree_, &QTreeWidget::itemClicked, this, [ this ]( QTreeWidgetItem* item, int ) {
        if ( !item->data( 0, IsDirRole ).toBool() ) {
            Q_EMIT fileActivated( item->data( 0, PathRole ).toString() );
        }
    } );
    connect( tree_, &QTreeWidget::itemActivated, this, [ this ]( QTreeWidgetItem* item, int ) {
        if ( item->data( 0, IsDirRole ).toBool() ) {
            item->setExpanded( !item->isExpanded() );
        }
        else {
            Q_EMIT fileActivated( item->data( 0, PathRole ).toString() );
        }
    } );

    connect( tree_, &QTreeWidget::customContextMenuRequested, this,
             &WorkspaceExplorer::showContextMenu );

    reTranslateUI();
    updateEmptyState();
}

void WorkspaceExplorer::reTranslateUI()
{
    emptyLabel_->setText(
        tr( "You have not yet opened a folder.<br/><br/>"
            "<a href=\"open-folder\">Open Folder</a><br/><br/>"
            "<a href=\"open-workspace\">Open Workspace from File</a>" ) );
}

void WorkspaceExplorer::setWorkspace( const Workspace& workspace )
{
    QSet<QString> expandedRoots;
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        auto* root = tree_->topLevelItem( i );
        if ( root->isExpanded() ) {
            expandedRoots.insert( root->data( 0, PathRole ).toString() );
        }
    }

    tree_->clear();
    hasFolders_ = !workspace.isEmpty();

    for ( const auto& folder : workspace.folders() ) {
        auto* root = new QTreeWidgetItem( tree_ );
        root->setText( 0, folder.displayName() );
        root->setToolTip( 0, QDir::toNativeSeparators( folder.path ) );
        root->setData( 0, PathRole, folder.path );
        root->setData( 0, IsDirRole, true );
        root->setData( 0, IsRootRole, true );
        root->setData( 0, PopulatedRole, false );

        const QFileInfo info( folder.path );
        root->setIcon( 0, iconProvider_.icon( info.exists() ? QFileIconProvider::Folder
                                                            : QFileIconProvider::Trashcan ) );
        if ( !info.exists() ) {
            root->setForeground( 0, palette().brush( QPalette::Disabled, QPalette::Text ) );
            root->setToolTip( 0, tr( "%1 (not found)" )
                                     .arg( QDir::toNativeSeparators( folder.path ) ) );
        }
        else {
            makePlaceholder( root );
        }

        auto font = root->font( 0 );
        font.setBold( true );
        root->setFont( 0, font );

        // Mirror VS Code: a single-folder workspace opens expanded.
        if ( info.exists()
             && ( workspace.folders().size() == 1 || expandedRoots.contains( folder.path ) ) ) {
            root->setExpanded( true );
        }
    }

    updateEmptyState();
}

void WorkspaceExplorer::refresh()
{
    for ( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
        repopulate( tree_->topLevelItem( i ) );
    }
}

void WorkspaceExplorer::repopulate( QTreeWidgetItem* parent )
{
    if ( !parent->data( 0, PopulatedRole ).toBool() ) {
        return;
    }

    // Remember which sub-directories are expanded so refresh is non-destructive.
    QSet<QString> expanded;
    for ( int i = 0; i < parent->childCount(); ++i ) {
        auto* child = parent->child( i );
        if ( child->isExpanded() ) {
            expanded.insert( child->data( 0, PathRole ).toString() );
        }
    }

    populate( parent );

    for ( int i = 0; i < parent->childCount(); ++i ) {
        auto* child = parent->child( i );
        if ( expanded.contains( child->data( 0, PathRole ).toString() ) ) {
            child->setExpanded( true );
            repopulate( child );
        }
    }
}

void WorkspaceExplorer::populate( QTreeWidgetItem* parent )
{
    const auto dirPath = parent->data( 0, PathRole ).toString();

    // Remove placeholder / stale children.
    while ( parent->childCount() > 0 ) {
        delete parent->takeChild( 0 );
    }

    QDir dir( dirPath );
    dir.setFilter( QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden );
    dir.setSorting( QDir::DirsFirst | QDir::Name | QDir::IgnoreCase | QDir::LocaleAware );

    const auto entries = dir.entryInfoList();
    tree_->setUpdatesEnabled( false );
    for ( const auto& info : entries ) {
        makeItem( info, parent );
    }
    tree_->setUpdatesEnabled( true );

    parent->setData( 0, PopulatedRole, true );
}

QTreeWidgetItem* WorkspaceExplorer::makeItem( const QFileInfo& info, QTreeWidgetItem* parent )
{
    auto* item = new QTreeWidgetItem( parent );
    item->setText( 0, info.fileName() );
    item->setToolTip( 0, QDir::toNativeSeparators( info.absoluteFilePath() ) );
    item->setData( 0, PathRole, info.absoluteFilePath() );
    item->setData( 0, IsRootRole, false );

    const bool isDir = info.isDir();
    item->setData( 0, IsDirRole, isDir );
    item->setData( 0, PopulatedRole, false );
    item->setIcon( 0, iconProvider_.icon( info ) );

    if ( isDir ) {
        // Make the expand arrow appear without scanning the directory yet.
        makePlaceholder( item );
    }
    else {
        item->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren );
    }

    return item;
}

void WorkspaceExplorer::showContextMenu( const QPoint& pos )
{
    auto* item = tree_->itemAt( pos );
    QMenu menu( this );

    if ( item == nullptr ) {
        menu.addAction( tr( "Add Folder to Workspace..." ), this,
                        [ this ] { Q_EMIT addFolderRequested(); } );
        menu.addAction( tr( "Refresh" ), this, [ this ] { refresh(); } );
        menu.exec( tree_->viewport()->mapToGlobal( pos ) );
        return;
    }

    const auto path = item->data( 0, PathRole ).toString();
    const bool isDir = item->data( 0, IsDirRole ).toBool();
    const bool isRoot = item->data( 0, IsRootRole ).toBool();

    if ( !isDir ) {
        menu.addAction( tr( "Open" ), this, [ this, path ] { Q_EMIT fileActivated( path ); } );
        menu.addSeparator();
    }
    else {
        // Open every regular file directly inside the folder (non-recursive).
        menu.addAction( tr( "Open All Files in Folder" ), this, [ this, path ] {
            QDir dir( path );
            dir.setFilter( QDir::Files | QDir::NoDotAndDotDot );
            dir.setSorting( QDir::Name | QDir::IgnoreCase );
            for ( const auto& info : dir.entryInfoList() ) {
                Q_EMIT fileActivated( info.absoluteFilePath() );
            }
        } );
        menu.addSeparator();
    }

    const auto selected = tree_->selectedItems();
    int selectedFiles = 0;
    for ( auto* s : selected ) {
        if ( !s->data( 0, IsDirRole ).toBool() ) {
            ++selectedFiles;
        }
    }
    if ( selectedFiles > 1 ) {
        menu.addAction( tr( "Open %n Selected File(s)", nullptr, selectedFiles ), this,
                        [ this, selected ] {
                            for ( auto* s : selected ) {
                                if ( !s->data( 0, IsDirRole ).toBool() ) {
                                    Q_EMIT fileActivated( s->data( 0, PathRole ).toString() );
                                }
                            }
                        } );
        menu.addSeparator();
    }

    menu.addAction( tr( "Reveal in File Explorer" ), this,
                    [ path ] { showPathInFileExplorer( path ); } );
    menu.addAction( tr( "Copy Path" ), this,
                    [ path ] { sendTextToClipboard( QDir::toNativeSeparators( path ) ); } );
    menu.addSeparator();

    if ( isDir ) {
        menu.addAction( tr( "Refresh" ), this, [ this, item ] {
            if ( item->data( 0, PopulatedRole ).toBool() ) {
                repopulate( item );
            }
        } );
    }

    if ( isRoot ) {
        menu.addSeparator();
        menu.addAction( tr( "Add Folder to Workspace..." ), this,
                        [ this ] { Q_EMIT addFolderRequested(); } );
        menu.addAction( tr( "Remove Folder from Workspace" ), this,
                        [ this, path ] { Q_EMIT removeFolderRequested( path ); } );
    }

    menu.exec( tree_->viewport()->mapToGlobal( pos ) );
}

void WorkspaceExplorer::updateEmptyState()
{
    emptyLabel_->setVisible( !hasFolders_ );
    tree_->setVisible( hasFolders_ );
}
