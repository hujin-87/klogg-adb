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

#ifndef KLOGG_WORKSPACE_H
#define KLOGG_WORKSPACE_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

// A multi-root workspace in the Visual Studio Code ".code-workspace" format:
//
//   {
//       "folders": [ { "path": "relative/or/absolute", "name": "optional" }, ... ],
//       "settings": {}
//   }
//
// Folder paths are stored relative to the workspace file when possible (as
// VS Code does) and resolved to absolute paths on load. Files written by VS
// Code may contain // and /* */ comments and trailing commas, both are
// accepted on load. Unknown top-level keys ("settings", "extensions",
// "launch", ...) are preserved verbatim so a file edited by klogg still works
// in VS Code.
class Workspace {
  public:
    static constexpr const char* FileExtension = "code-workspace";

    struct Folder {
        QString path; // absolute, cleaned
        QString name; // optional display name from the file, may be empty

        // Name shown in the UI: explicit name or the last path component.
        QString displayName() const;
    };

    // True if the file name has the ".code-workspace" extension.
    static bool isWorkspaceFile( const QString& fileName );

    // Remove // and /* */ comments (outside string literals) and trailing
    // commas so QJsonDocument can parse a VS Code "JSON with comments" file.
    static QByteArray stripJsonComments( const QByteArray& jsonc );

    // Load from a .code-workspace file. On failure the workspace is left
    // unchanged and errorMessage (if given) describes the problem.
    bool load( const QString& filePath, QString* errorMessage = nullptr );

    // Write to a .code-workspace file and remember it as this workspace's
    // file. Paths are rewritten relative to the new location.
    bool save( const QString& filePath, QString* errorMessage = nullptr );

    // Serialize to the VS Code JSON layout, with folder paths relative to
    // baseDir (absolute when baseDir is empty or on another drive).
    QJsonObject toJson( const QString& baseDir ) const;
    QByteArray toJsonText( const QString& baseDir ) const;

    // Path of the backing file, empty for an untitled workspace.
    QString filePath() const
    {
        return filePath_;
    }
    bool isUntitled() const
    {
        return filePath_.isEmpty();
    }

    // Workspace name: file base name, or the single folder's name, or empty.
    QString name() const;

    const std::vector<Folder>& folders() const
    {
        return folders_;
    }
    QStringList folderPaths() const;
    bool isEmpty() const
    {
        return folders_.empty();
    }
    bool containsFolder( const QString& path ) const;

    // Returns false if the folder is already part of the workspace.
    bool addFolder( const QString& path, const QString& name = {} );
    bool removeFolder( const QString& path );

    // Replace all folders (used to restore an untitled workspace).
    void setFolderPaths( const QStringList& paths );

    // Changes since the last load/save.
    bool isModified() const
    {
        return modified_;
    }

    // Forget everything, back to an empty untitled workspace.
    void clear();

  private:
    static QString normalizePath( const QString& path );

    QString filePath_;
    std::vector<Folder> folders_;
    // Top-level keys other than "folders", preserved verbatim.
    QJsonObject extra_;
    bool modified_ = false;
};

#endif // KLOGG_WORKSPACE_H
