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

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "workspace.h"

namespace {

void writeFile( const QString& path, const QByteArray& content )
{
    QFile f( path );
    REQUIRE( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    f.write( content );
}

QByteArray readFile( const QString& path )
{
    QFile f( path );
    REQUIRE( f.open( QIODevice::ReadOnly ) );
    return f.readAll();
}

} // namespace

TEST_CASE( "Workspace strips JSON comments", "[workspace]" )
{
    SECTION( "line and block comments outside strings" )
    {
        const QByteArray in = "//\n{\n\t// c1\n\t\"a\": \"x//y\", /* c2 */ \"b\": 1\n}";
        const auto out = Workspace::stripJsonComments( in );
        const auto doc = QJsonDocument::fromJson( out );
        REQUIRE( doc.isObject() );
        REQUIRE( doc.object().value( "a" ).toString() == "x//y" );
        REQUIRE( doc.object().value( "b" ).toInt() == 1 );
    }

    SECTION( "trailing commas" )
    {
        const QByteArray in = "{ \"folders\": [ { \"path\": \"a\", }, ], }";
        const auto doc = QJsonDocument::fromJson( Workspace::stripJsonComments( in ) );
        REQUIRE( doc.isObject() );
        REQUIRE( doc.object().value( "folders" ).toArray().size() == 1 );
    }

    SECTION( "escaped quote inside string" )
    {
        const QByteArray in = "{ \"a\": \"q\\\"//notacomment\" }";
        const auto doc = QJsonDocument::fromJson( Workspace::stripJsonComments( in ) );
        REQUIRE( doc.object().value( "a" ).toString() == "q\"//notacomment" );
    }
}

TEST_CASE( "Workspace loads VS Code .code-workspace", "[workspace]" )
{
    QTemporaryDir tmp;
    REQUIRE( tmp.isValid() );
    const QDir root( tmp.path() );
    REQUIRE( root.mkpath( "android/vendor" ) );
    REQUIRE( root.mkpath( "script" ) );

    // Same shape as a real VS Code file, including a leading comment line,
    // per-entry comments and an absolute path.
    const QByteArray content = QByteArray( "//\n\n{\n\t\"folders\": [\n\t\t{\n\t\t\t\"path\": "
                                           "\"android/vendor/\"\n\t\t},\n\t\t{//note\n\t\t\t\"path\": "
                                           "\"script\"\n\t\t},\n\t\t{\n\t\t\t\"path\": \"" )
                               + root.absoluteFilePath( "script" ).toUtf8()
                               + "\",\n\t\t\t\"name\": \"dup\"\n\t\t},\n\t\t{\n\t\t\t\"path\": "
                                 "\".\"\n\t\t}\n\t],\n\t\"settings\": { \"files.exclude\": {} }\n}\n";
    const auto file = root.absoluteFilePath( "test.code-workspace" );
    writeFile( file, content );

    Workspace ws;
    QString error;
    REQUIRE( ws.load( file, &error ) );
    REQUIRE( error.isEmpty() );
    REQUIRE( ws.filePath() == QDir::cleanPath( file ) );
    REQUIRE( !ws.isUntitled() );
    REQUIRE( ws.name() == "test" );

    // "script" and its absolute duplicate collapse into one entry.
    const auto paths = ws.folderPaths();
    REQUIRE( paths.size() == 3 );
    REQUIRE( paths[ 0 ] == QDir::cleanPath( root.absoluteFilePath( "android/vendor" ) ) );
    REQUIRE( paths[ 1 ] == QDir::cleanPath( root.absoluteFilePath( "script" ) ) );
    REQUIRE( paths[ 2 ] == QDir::cleanPath( root.absolutePath() ) );
    REQUIRE( ws.folders()[ 0 ].displayName() == "vendor" );

    SECTION( "round trip keeps relative paths and settings" )
    {
        const auto out = root.absoluteFilePath( "out.code-workspace" );
        REQUIRE( ws.save( out, &error ) );
        REQUIRE( ws.filePath() == QDir::cleanPath( out ) );

        const auto text = readFile( out );
        // Plain JSON, tab indented like VS Code.
        REQUIRE( text.startsWith( "{\n\t\"folders\": [" ) );
        REQUIRE( text.contains( "\"path\": \"android/vendor\"" ) );
        REQUIRE( text.contains( "\"path\": \"script\"" ) );
        REQUIRE( text.contains( "\"path\": \".\"" ) );
        REQUIRE( text.contains( "\"files.exclude\"" ) );
        REQUIRE( !text.contains( "//" ) );

        Workspace again;
        REQUIRE( again.load( out, &error ) );
        REQUIRE( again.folderPaths() == paths );
    }

    SECTION( "saving elsewhere rewrites relative paths" )
    {
        REQUIRE( root.mkpath( "sub" ) );
        const auto out = root.absoluteFilePath( "sub/moved.code-workspace" );
        REQUIRE( ws.save( out, &error ) );
        const auto text = readFile( out );
        REQUIRE( text.contains( "\"path\": \"../android/vendor\"" ) );
        REQUIRE( text.contains( "\"path\": \"..\"" ) );
    }

    SECTION( "add and remove folders" )
    {
        REQUIRE( !ws.isModified() );
        REQUIRE( !ws.addFolder( root.absoluteFilePath( "script/" ) ) );
        REQUIRE( ws.addFolder( root.absoluteFilePath( "android" ) ) );
        REQUIRE( ws.isModified() );
        REQUIRE( ws.folderPaths().size() == 4 );
        REQUIRE( ws.removeFolder( root.absoluteFilePath( "script" ) ) );
        REQUIRE( ws.folderPaths().size() == 3 );
        REQUIRE( !ws.removeFolder( "/nonexistent" ) );
    }
}

TEST_CASE( "Workspace rejects malformed files", "[workspace]" )
{
    QTemporaryDir tmp;
    const auto file = QDir( tmp.path() ).absoluteFilePath( "bad.code-workspace" );
    writeFile( file, "{ \"folders\": \"nope\" }" );

    Workspace ws;
    ws.addFolder( tmp.path() );
    QString error;
    REQUIRE( !ws.load( file, &error ) );
    REQUIRE( !error.isEmpty() );
    // Failed load leaves the workspace untouched.
    REQUIRE( ws.folderPaths().size() == 1 );

    REQUIRE( !ws.load( QDir( tmp.path() ).absoluteFilePath( "missing.code-workspace" ), &error ) );
    REQUIRE( !error.isEmpty() );
}

TEST_CASE( "Workspace untitled naming", "[workspace]" )
{
    Workspace ws;
    REQUIRE( ws.isUntitled() );
    REQUIRE( ws.isEmpty() );
    REQUIRE( ws.name().isEmpty() );
    REQUIRE( Workspace::isWorkspaceFile( "a/b.code-workspace" ) );
    REQUIRE( Workspace::isWorkspaceFile( "a/B.CODE-WORKSPACE" ) );
    REQUIRE( !Workspace::isWorkspaceFile( "a/b.log" ) );

    QTemporaryDir tmp;
    ws.addFolder( tmp.path() );
    REQUIRE( ws.name() == QDir( tmp.path() ).dirName() );
}
