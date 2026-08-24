/* ABOUTME: User-visible strings mirrored from the upstream STMClient frontend. */
/* ABOUTME: Each literal cites its upstream source so drift is greppable. */
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
    file(s), but you are not obligated to do so. If you do not wish
    to do so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/
#ifndef UPSTREAM_STRINGS_H
#define UPSTREAM_STRINGS_H

/* Mirrors src/frontend/stmclient.cc:296. */
static const wchar_t EXIT_ON_USER_REQUEST[] = L"Exiting on user request...";

/* Mirrors src/frontend/stmclient.cc:444. */
static const wchar_t EXIT_ON_IO_LOSS[] = L"Exiting...";

/* Mirrors src/frontend/stmclient.cc:462. */
static const wchar_t EXIT_ON_SIGNAL[] = L"Signal received, shutting down...";

/* Mirrors src/frontend/stmclient.cc:169-177; the unsupported -p sentence is omitted. */
static const char CONNECTION_FAILURE_DIAGNOSTIC[] =
  "\nmosh did not make a successful connection to %s:%s.\n"
  "Please verify that UDP port %s is not firewalled and can reach the server.\n\n";

/* Mirrors src/frontend/stmclient.cc:178-182. */
static const char UNCLEAN_EXIT_DIAGNOSTIC[] =
  "\n\nmosh did not shut down cleanly. Please note that the\n"
  "mosh-server process may still be running on the server.\n";

/* Mirrors src/frontend/mosh-client.cc:215. */
static const char EXIT_BANNER[] = "[mosh is exiting.]\n";

#endif
