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

#ifndef KLOGG_RECENTWORKSPACES_H
#define KLOGG_RECENTWORKSPACES_H

#include <QStringList>

#include "persistable.h"

// Recently opened folders and .code-workspace files (VS Code "Open Recent"
// equivalent for workspaces). Entries that no longer exist are pruned on add.
class RecentWorkspaces final : public Persistable<RecentWorkspaces, session_settings> {
  public:
    static constexpr int MaxItems = 10;

    static const char* persistableName()
    {
        return "RecentWorkspaces";
    }

    // Path of a folder or of a .code-workspace file.
    void addRecent( const QString& path );
    void removeRecent( const QString& path );
    void removeAll();

    // Latest first.
    QStringList recentWorkspaces() const
    {
        return recent_;
    }

    void saveToStorage( QSettings& settings ) const;
    void retrieveFromStorage( QSettings& settings );

  private:
    static constexpr int Version = 1;

    QStringList recent_;
};

#endif // KLOGG_RECENTWORKSPACES_H
