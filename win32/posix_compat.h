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

// ABOUTME: Minimal POSIX declarations needed to archive Windows-only unported utility sources.
// ABOUTME: These no-op signal helpers preserve compilation until M1 replaces the event loop.
#ifndef MOSH_WIN32_POSIX_COMPAT_H
#define MOSH_WIN32_POSIX_COMPAT_H

#include <winsock2.h>

using sigset_t = int;

struct sigaction {
  int sa_flags;
  void ( *sa_handler )( int );
  sigset_t sa_mask;
};

#define SA_RESTART 0
#define SIG_BLOCK 0
#define SIG_SETMASK 0

inline int sigemptyset( sigset_t* set )
{
  *set = 0;
  return 0;
}

inline int sigfillset( sigset_t* set )
{
  *set = 0;
  return 0;
}

inline int sigaddset( sigset_t* set, int signum )
{
  (void)set;
  (void)signum;
  return 0;
}

inline int sigprocmask( int how, const sigset_t* set, sigset_t* oldset )
{
  (void)how;
  (void)set;
  if ( oldset ) {
    *oldset = 0;
  }
  return 0;
}

inline int sigaction( int signum, const struct sigaction* action, struct sigaction* oldaction )
{
  (void)signum;
  (void)action;
  (void)oldaction;
  return 0;
}

#endif
