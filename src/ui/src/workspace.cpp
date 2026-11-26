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

#include "workspace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>

#include "log.h"

namespace {

constexpr auto FoldersKey = "folders";
constexpr auto PathKey = "path";
constexpr auto UriKey = "uri";
constexpr auto NameKey = "name";
constexpr auto SettingsKey = "settings";

// QJsonDocument indents with four spaces; VS Code writes tabs. Convert the
// leading indentation of every line so a file saved by klogg is byte-for-byte
// in the style VS Code produces. JSON strings cannot contain raw newlines, so
// leading whitespace is always indentation.
QByteArray spacesToTabs( const QByteArray& json )
{
    QByteArray result;
    result.reserve( json.size() );

    int lineStart = 0;
    while ( lineStart < json.size() ) {
        int lineEnd = json.indexOf( '\n', lineStart );
        if ( lineEnd < 0 ) {
            lineEnd = json.size();
        }

        int spaces = 0;
        while ( lineStart + spaces < lineEnd && json.at( lineStart + spaces ) == ' ' ) {
            ++spaces;
        }

        result.append( QByteArray( spaces / 4, '\t' ) );
        result.append( QByteArray( spaces % 4, ' ' ) );
        result.append( json.mid( lineStart + spaces, lineEnd - lineStart - spaces ) );
        if ( lineEnd < json.size() ) {
            result.append( '\n' );
        }
        lineStart = lineEnd + 1;
    }

    return result;
}

} // namespace

QString Workspace::Folder::displayName() const
{
    if ( !name.isEmpty() ) {
        return name;
    }
    const auto fileName = QFileInfo( path ).fileName();
    return fileName.isEmpty() ? path : fileName;
}

bool Workspace::isWorkspaceFile( const QString& fileName )
{
    return QFileInfo( fileName ).suffix().compare( QLatin1String( FileExtension ),
                                                   Qt::CaseInsensitive )
           == 0;
}

QByteArray Workspace::stripJsonComments( const QByteArray& jsonc )
{
    QByteArray out;
    out.reserve( jsonc.size() );

    enum class State { Code, String, LineComment, BlockComment };
    State state = State::Code;

    const int size = jsonc.size();
    for ( int i = 0; i < size; ++i ) {
        const char c = jsonc.at( i );
        const char next = ( i + 1 < size ) ? jsonc.at( i + 1 ) : '\0';

        switch ( state ) {
        case State::Code:
            if ( c == '"' ) {
                state = State::String;
                out.append( c );
            }
            else if ( c == '/' && next == '/' ) {
                state = State::LineComment;
                ++i;
            }
            else if ( c == '/' && next == '*' ) {
                state = State::BlockComment;
                ++i;
            }
            else if ( c == ',' ) {
                // Drop a trailing comma: look past whitespace (and comments
                // are already gone from `out`, but not from the input) for
                // the closing bracket.
                int j = i + 1;
                while ( j < size ) {
                    const char d = jsonc.at( j );
                    if ( d == ' ' || d == '\t' || d == '\r' || d == '\n' ) {
                        ++j;
                    }
                    else if ( d == '/' && j + 1 < size && jsonc.at( j + 1 ) == '/' ) {
                        while ( j < size && jsonc.at( j ) != '\n' ) {
                            ++j;
                        }
                    }
                    else if ( d == '/' && j + 1 < size && jsonc.at( j + 1 ) == '*' ) {
                        const int end = jsonc.indexOf( "*/", j + 2 );
                        j = ( end < 0 ) ? size : end + 2;
                    }
                    else {
                        break;
                    }
                }
                if ( j < size && ( jsonc.at( j ) == ']' || jsonc.at( j ) == '}' ) ) {
                    // trailing comma, skip it
                }
                else {
                    out.append( c );
                }
            }
            else {
                out.append( c );
            }
            break;

        case State::String:
            out.append( c );
            if ( c == '\\' && i + 1 < size ) {
                out.append( next );
                ++i;
            }
            else if ( c == '"' ) {
                state = State::Code;
            }
            break;

        case State::LineComment:
            if ( c == '\n' ) {
                state = State::Code;
                out.append( c );
            }
            break;

        case State::BlockComment:
            if ( c == '*' && next == '/' ) {
                state = State::Code;
                ++i;
            }
            else if ( c == '\n' ) {
                // keep line structure for error reporting
                out.append( c );
            }
            break;
        }
    }

    return out;
}

QString Workspace::normalizePath( const QString& path )
{
    return QDir::cleanPath( QFileInfo( path ).absoluteFilePath() );
}

bool Workspace::load( const QString& filePath, QString* errorMessage )
{
    QFile file( filePath );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        if ( errorMessage ) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    const auto raw = file.readAll();
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson( stripJsonComments( raw ), &parseError );
    if ( parseError.error != QJsonParseError::NoError ) {
        if ( errorMessage ) {
            *errorMessage = QStringLiteral( "%1 (offset %2)" )
                                .arg( parseError.errorString() )
                                .arg( parseError.offset );
        }
        return false;
    }
    if ( !document.isObject() ) {
        if ( errorMessage ) {
            *errorMessage = QStringLiteral( "top-level value is not an object" );
        }
        return false;
    }

    const auto root = document.object();
    const auto foldersValue = root.value( QLatin1String( FoldersKey ) );
    if ( !foldersValue.isUndefined() && !foldersValue.isArray() ) {
        if ( errorMessage ) {
            *errorMessage = QStringLiteral( "\"folders\" is not an array" );
        }
        return false;
    }

    const QDir baseDir = QFileInfo( filePath ).absoluteDir();

    std::vector<Folder> folders;
    for ( const auto& entry : foldersValue.toArray() ) {
        QString path;
        QString name;
        if ( entry.isString() ) {
            path = entry.toString();
        }
        else if ( entry.isObject() ) {
            const auto obj = entry.toObject();
            path = obj.value( QLatin1String( PathKey ) ).toString();
            name = obj.value( QLatin1String( NameKey ) ).toString();
            if ( path.isEmpty() ) {
                const QUrl uri( obj.value( QLatin1String( UriKey ) ).toString() );
                if ( uri.isLocalFile() ) {
                    path = uri.toLocalFile();
                }
            }
        }

        if ( path.isEmpty() ) {
            LOG_WARNING << "Skipping workspace folder entry without a local path";
            continue;
        }

        if ( path.startsWith( QLatin1String( "~/" ) ) || path == QLatin1String( "~" ) ) {
            path = QDir::homePath() + path.mid( 1 );
        }

        const auto absolute = QDir::cleanPath( baseDir.absoluteFilePath( path ) );
        if ( std::any_of( folders.begin(), folders.end(),
                          [ &absolute ]( const Folder& f ) { return f.path == absolute; } ) ) {
            continue;
        }
        folders.push_back( Folder{ absolute, name } );
    }

    QJsonObject extra = root;
    extra.remove( QLatin1String( FoldersKey ) );

    filePath_ = normalizePath( filePath );
    folders_ = std::move( folders );
    extra_ = std::move( extra );
    modified_ = false;

    LOG_INFO << "Loaded workspace " << filePath_ << " with " << folders_.size() << " folders";
    return true;
}

QJsonObject Workspace::toJson( const QString& baseDir ) const
{
    QJsonArray foldersArray;
    const QDir base( baseDir );

    for ( const auto& folder : folders_ ) {
        QString stored = folder.path;
        if ( !baseDir.isEmpty() ) {
            if ( QDir::cleanPath( base.absolutePath() ) == folder.path ) {
                stored = QStringLiteral( "." );
            }
            else {
                auto relative = base.relativeFilePath( folder.path );
                // relativeFilePath() returns the absolute path unchanged when
                // the two are on different drives (Windows); keep that. It
                // also leaves a trailing slash on "../" style results.
                while ( relative.size() > 1 && relative.endsWith( QLatin1Char( '/' ) ) ) {
                    relative.chop( 1 );
                }
                if ( !relative.isEmpty() && !QDir::isAbsolutePath( relative ) ) {
                    stored = relative;
                }
            }
        }

        QJsonObject entry;
        entry.insert( QLatin1String( PathKey ), stored );
        if ( !folder.name.isEmpty() ) {
            entry.insert( QLatin1String( NameKey ), folder.name );
        }
        foldersArray.append( entry );
    }

    QJsonObject root = extra_;
    root.insert( QLatin1String( FoldersKey ), foldersArray );
    if ( !root.contains( QLatin1String( SettingsKey ) ) ) {
        root.insert( QLatin1String( SettingsKey ), QJsonObject{} );
    }
    return root;
}

QByteArray Workspace::toJsonText( const QString& baseDir ) const
{
    return spacesToTabs( QJsonDocument( toJson( baseDir ) ).toJson( QJsonDocument::Indented ) );
}

bool Workspace::save( const QString& filePath, QString* errorMessage )
{
    const auto absolute = normalizePath( filePath );
    const auto baseDir = QFileInfo( absolute ).absolutePath();

    QSaveFile file( absolute );
    if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) ) {
        if ( errorMessage ) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    file.write( toJsonText( baseDir ) );
    if ( !file.commit() ) {
        if ( errorMessage ) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    filePath_ = absolute;
    modified_ = false;
    LOG_INFO << "Saved workspace " << filePath_;
    return true;
}

QString Workspace::name() const
{
    if ( !filePath_.isEmpty() ) {
        return QFileInfo( filePath_ ).completeBaseName();
    }
    if ( folders_.size() == 1 ) {
        return folders_.front().displayName();
    }
    return {};
}

QStringList Workspace::folderPaths() const
{
    QStringList paths;
    paths.reserve( static_cast<int>( folders_.size() ) );
    for ( const auto& folder : folders_ ) {
        paths.append( folder.path );
    }
    return paths;
}

bool Workspace::containsFolder( const QString& path ) const
{
    const auto absolute = normalizePath( path );
    return std::any_of( folders_.begin(), folders_.end(),
                        [ &absolute ]( const Folder& f ) { return f.path == absolute; } );
}

bool Workspace::addFolder( const QString& path, const QString& name )
{
    const auto absolute = normalizePath( path );
    if ( containsFolder( absolute ) ) {
        return false;
    }
    folders_.push_back( Folder{ absolute, name } );
    modified_ = true;
    return true;
}

bool Workspace::removeFolder( const QString& path )
{
    const auto absolute = normalizePath( path );
    const auto it = std::find_if( folders_.begin(), folders_.end(),
                                  [ &absolute ]( const Folder& f ) { return f.path == absolute; } );
    if ( it == folders_.end() ) {
        return false;
    }
    folders_.erase( it );
    modified_ = true;
    return true;
}

void Workspace::setFolderPaths( const QStringList& paths )
{
    folders_.clear();
    for ( const auto& path : paths ) {
        const auto absolute = normalizePath( path );
        if ( !containsFolder( absolute ) ) {
            folders_.push_back( Folder{ absolute, {} } );
        }
    }
    modified_ = true;
}

void Workspace::clear()
{
    filePath_.clear();
    folders_.clear();
    extra_ = QJsonObject{};
    modified_ = false;
}
