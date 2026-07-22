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
