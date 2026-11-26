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

#include "recentworkspaces.h"

#include <QFileInfo>
#include <QSettings>

#include "log.h"

void RecentWorkspaces::removeRecent( const QString& path )
{
    QMutableStringListIterator i( recent_ );
    while ( i.hasNext() ) {
        if ( !QFileInfo::exists( i.next() ) ) {
            i.remove();
        }
    }
    recent_.removeAll( path );
}

void RecentWorkspaces::removeAll()
{
    recent_.clear();
}

void RecentWorkspaces::addRecent( const QString& path )
{
    removeRecent( path );
    recent_.push_front( path );
    while ( recent_.size() > MaxItems ) {
        recent_.pop_back();
    }
}

void RecentWorkspaces::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "RecentWorkspaces::saveToStorage";

    settings.beginGroup( "RecentWorkspaces" );
    settings.setValue( "version", Version );
    settings.remove( "history" );
    settings.beginWriteArray( "history" );
    for ( int i = 0; i < recent_.size(); ++i ) {
        settings.setArrayIndex( i );
        settings.setValue( "path", recent_.at( i ) );
    }
    settings.endArray();
    settings.endGroup();
}

void RecentWorkspaces::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "RecentWorkspaces::retrieveFromStorage";

    removeAll();

    if ( !settings.contains( "RecentWorkspaces/version" ) ) {
        return;
    }

    settings.beginGroup( "RecentWorkspaces" );
    if ( settings.value( "version" ).toInt() == Version ) {
        const int size = settings.beginReadArray( "history" );
        for ( int i = 0; i < size; ++i ) {
            settings.setArrayIndex( i );
            const auto path = settings.value( "path" ).toString();
            if ( !path.isEmpty() ) {
                recent_.append( path );
            }
        }
        settings.endArray();
    }
    else {
        LOG_ERROR << "Unknown version of recent workspaces, ignoring it...";
    }
    settings.endGroup();
}
