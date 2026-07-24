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

/* ABOUTME: Pure helpers for the SSH bootstrap: sh_quote, build_remote_command, win_quote_arg, build_ssh_command_line. */
/* ABOUTME: Task 1 of M3 (SSH bootstrap) milestone for native Windows ARM64 port. */

#ifndef MOSH_BOOTSTRAP_H
#define MOSH_BOOTSTRAP_H

#include <string>

struct BootstrapResult { std::string ip; std::string port; std::string key; };

/* scripts/mosh.pl shell_quote: wrap in single quotes, embedded ' -> '\'' */
std::string sh_quote( const std::string &arg );
/* Fixed minimal remote command (probe && mosh-server new -c 256 -s -l LC_ALL=C.UTF-8). */
std::string build_remote_command();
/* CommandLineToArgvW-compatible quoting for one Windows argument. */
std::string win_quote_arg( const std::string &arg );
/* Full ssh.exe command line (wide) for CreateProcessW, given the resolved ssh
   path and the destination. Includes -n, forced-direct + no-multiplexing
   options, the quoted target, --, and the quoted remote command. */
std::wstring build_ssh_command_line( const std::wstring &ssh_path, const std::string &target );

/* Spawn ssh, resolve the numeric endpoint. "" on success (fills *out), else error. */
std::string mosh_bootstrap( const char *target, BootstrapResult *out );

#endif /* MOSH_BOOTSTRAP_H */
