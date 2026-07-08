/*
 * Copyright (C) 2009, 2010 Nicolas Bonnefon and other contributors
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
 * Copyright (C) 2019 Anton Filimonov and other contributors
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

#include "predefinedfilters.h"

#include "log.h"

namespace {
// Some externally-produced filter configs store non-ASCII text as escaped UTF-8
// *bytes* (e.g. "\xe7\x9b\xb8" for 相). Qt's INI reader interprets "\xNN" as a
// Unicode code point instead, so such a value comes back as mojibake where every
// character is in U+0080..U+00FF (the raw UTF-8 bytes). Detect that case and
// re-decode the bytes as UTF-8. Values that are plain ASCII, or that contain real
// (code-point escaped) Unicode, are returned unchanged.
QString repairUtf8Mojibake( const QString& value )
{
    bool hasHighByte = false;
    for ( const QChar c : value ) {
        const ushort u = c.unicode();
        if ( u > 0xFF ) {
            return value; // contains genuine Unicode -> already correct
        }
        if ( u >= 0x80 ) {
            hasHighByte = true;
        }
    }

    if ( !hasHighByte ) {
        return value; // pure ASCII -> nothing to repair
    }

    const QString decoded = QString::fromUtf8( value.toLatin1() );
    if ( decoded.contains( QChar( 0xFFFD ) ) ) {
        return value; // not a valid UTF-8 byte sequence -> leave as-is
    }

    return decoded;
}
} // namespace

void PredefinedFiltersCollection::retrieveFromStorage( QSettings& settings )
{
    LOG_DEBUG << "PredefinedFiltersCollection::retrieveFromStorage";

    if ( settings.contains( "PredefinedFiltersCollection/version" ) ) {
        settings.beginGroup( "PredefinedFiltersCollection" );
        if ( settings.value( "version" ).toInt() <= PredefinedFiltersCollection_VERSION ) {
            filters_.clear();

            int size = settings.beginReadArray( "filters" );

            filters_.reserve( size );
            for ( int i = 0; i < size; ++i ) {
                settings.setArrayIndex( i );

                filters_.push_back( { repairUtf8Mojibake( settings.value( "name" ).toString() ),
                                      repairUtf8Mojibake( settings.value( "filter" ).toString() ),
                                      settings.value( "regex", true ).toBool() } );
            }
            settings.endArray();
        }
        else {
            LOG_ERROR << "Unknown version of PredefinedFiltersCollection, ignoring it...";
        }
        settings.endGroup();
    }
}

void PredefinedFiltersCollection::saveToStorage( QSettings& settings ) const
{
    LOG_DEBUG << "PredefinedFiltersCollection::saveToStorage";

    settings.beginGroup( "PredefinedFiltersCollection" );
    settings.setValue( "version", PredefinedFiltersCollection_VERSION );

    settings.remove( "filters" );

    settings.beginWriteArray( "filters" );
    int arrayIndex = 0;
    for ( const auto& filter : filters_ ) {
        settings.setArrayIndex( arrayIndex );
        settings.setValue( "name", filter.name );
        settings.setValue( "filter", filter.pattern );
        settings.setValue( "regex", filter.useRegex );

        arrayIndex++;
    }
    settings.endArray();
    settings.endGroup();
}

void PredefinedFiltersCollection::saveToStorage(
    const PredefinedFiltersCollection::Collection& filters )
{
    filters_ = filters;
    this->save();
}

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::getFilters() const
{
    return filters_;
}

PredefinedFiltersCollection::Collection PredefinedFiltersCollection::getSyncedFilters()
{
    filters_ = this->getSynced().getFilters();
    return filters_;
}

void PredefinedFiltersCollection::setFilters( const Collection& filters )
{
    filters_ = filters;
}
