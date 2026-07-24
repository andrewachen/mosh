/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

/* ABOUTME: Windows-console I/O layer for mosh.exe: raw-mode/UTF-8 setup, teardown, and event loop. */
/* ABOUTME: Keeps Win32 console calls out of the OS-agnostic MoshCore. */
#ifndef CONSOLE_IO_H
#define CONSOLE_IO_H

/* winsock2 must precede windows.h, which would otherwise include legacy winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "mosh_core.h"
#include <windows.h>
#include <stdexcept>
#include <string>

struct ConsoleError : public std::runtime_error {
  DWORD win32_code;
  ConsoleError( DWORD code, const std::string &msg )
    : std::runtime_error( msg ), win32_code( code ) {}
};

struct ConsoleState {
  HANDLE h_in;
  HANDLE h_out;
  DWORD in_mode;
  DWORD out_mode;
  UINT in_cp;
  UINT out_cp;
};

void console_raw_enter( ConsoleState *saved );
void console_raw_restore( const ConsoleState *saved );
void console_dims( int *cols, int *rows );

/* Write every byte or throw ConsoleError. */
void write_all( HANDLE handle, const std::string &bytes );

void console_run( MoshCore &core, const ConsoleState &cs );
#endif
