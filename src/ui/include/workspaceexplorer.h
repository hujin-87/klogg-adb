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

#ifndef KLOGG_WORKSPACEEXPLORER_H
#define KLOGG_WORKSPACEEXPLORER_H

#include <QFileIconProvider>
#include <QWidget>

#include "workspace.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// VS Code style "Explorer" side panel: one root node per workspace folder,
// sub-directories populated lazily on expand so very large trees (e.g. an
// Android source checkout) stay cheap. Activating a file asks the main window
// to open it in a new tab.
class WorkspaceExplorer : public QWidget {
    Q_OBJECT

  public:
    explicit WorkspaceExplorer( QWidget* parent = nullptr );

    // Rebuild the tree from the workspace folders. Expanded roots stay
    // expanded when the same folder is still present.
    void setWorkspace( const Workspace& workspace );

    // Re-read all populated directories from disk.
    void refresh();

    void reTranslateUI();

  Q_SIGNALS:
    void fileActivated( const QString& filePath );
    void addFolderRequested();
    void removeFolderRequested( const QString& folderPath );
    void openWorkspaceFileRequested();
    void openFolderRequested();

  private:
    enum ItemRole {
        PathRole = Qt::UserRole,
        IsDirRole,
        IsRootRole,
        PopulatedRole,
    };

    void populate( QTreeWidgetItem* parent );
    void repopulate( QTreeWidgetItem* parent );
    QTreeWidgetItem* makeItem( const QFileInfo& info, QTreeWidgetItem* parent );
    void showContextMenu( const QPoint& pos );
    void updateEmptyState();

    QTreeWidget* tree_;
    QLabel* emptyLabel_;
    QFileIconProvider iconProvider_;
    bool hasFolders_ = false;
};

#endif // KLOGG_WORKSPACEEXPLORER_H
