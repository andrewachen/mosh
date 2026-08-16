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

/* ABOUTME: Win32-typed helpers for the SSH bootstrap: widen, resolve_on_path, resolve_ssh_path, drain_and_parse, spawn_and_drain. */
/* ABOUTME: Task 3 of M3 (SSH bootstrap) milestone - Win32 spawn and pipe-drain layer. */

#include "win32/mosh_bootstrap.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>

/* PROC_THREAD_ATTRIBUTE_JOB_LIST is not defined in older Windows SDKs. */
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
/* ProcThreadAttributeValue(ProcThreadAttributeJobList=13, FALSE, TRUE, FALSE); the
   target SDK's winbase.h lacks the JobList enum/macro, so define it explicitly
   from the SDK's own flag macros (verified: NUMBER=0x0000ffff, INPUT=0x00020000). */
#define PROC_THREAD_ATTRIBUTE_JOB_LIST ( ( 13 & PROC_THREAD_ATTRIBUTE_NUMBER ) | PROC_THREAD_ATTRIBUTE_INPUT )
#endif

std::wstring widen( const std::string &s );          // UTF-8 -> wide, MB_ERR_INVALID_CHARS
std::string resolve_on_path( const std::wstring &path_dirs, const std::wstring &name, std::wstring *out );
std::string resolve_ssh_path( std::wstring *out );    // PATH-only, fail-closed
std::string drain_and_parse( HANDLE read_end, ServerReply *r );
/* Spawn app_path with cmdline, whitelisting only 3 std handles, in a kill-on-
   close job associated atomically at creation; drain stdout on this thread and
   reap the child. "" on a valid MOSH CONNECT, or an error (ssh-exit / fatal /
   read). */
std::string spawn_and_drain( const std::wstring &app_path, const std::wstring &cmdline,
                             ServerReply *r, HANDLE cancel = NULL );
