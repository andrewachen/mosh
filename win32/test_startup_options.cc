/* ABOUTME: Windows-side tests for the StartupOptions snapshot and validate-before-bootstrap. */
/* ABOUTME: Verifies the snapshot aggregates the shared parsers and refuses bad config. */
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

#include <cstdio>
#include <string>

#include "src/frontend/terminaloverlay.h"
#include "win32/startup_options.h"

static int test_escape_key()
{
  StartupEnv env;
  env.escape_key = "\x01";  /* Ctrl-A */
  StartupOptions opts;
  std::string err;
  if ( !parse_startup_options( env, &opts, &err )
       || opts.escape.key != 0x01 || opts.escape.pass_key != 'A' ) {
    fprintf( stderr, "FAIL: snapshot did not carry the parsed escape key\n" );
    return 1;
  }
  return 0;
}

static int test_prediction_display()
{
  {
    StartupEnv env;
    StartupOptions opts;
    std::string err;
    if ( !parse_startup_options( env, &opts, &err )
         || opts.predict_display != Overlay::PredictionEngine::Adaptive ) {
      fprintf( stderr, "FAIL: absent MOSH_PREDICTION_DISPLAY did not default to Adaptive\n" );
      return 1;
    }
  }
  {
    StartupEnv env;
    env.predict_display = "always";
    StartupOptions opts;
    std::string err;
    if ( !parse_startup_options( env, &opts, &err )
         || opts.predict_display != Overlay::PredictionEngine::Always ) {
      fprintf( stderr, "FAIL: MOSH_PREDICTION_DISPLAY=always not honored\n" );
      return 1;
    }
  }
  {
    StartupEnv env;
    env.predict_display = "bogus";
    StartupOptions opts;
    std::string err;
    if ( parse_startup_options( env, &opts, &err ) || err.empty() ) {
      fprintf( stderr, "FAIL: invalid MOSH_PREDICTION_DISPLAY accepted\n" );
      return 1;
    }
  }
  return 0;
}

int main()
{
  if ( test_escape_key() != 0 ) {
    return 1;
  }
  if ( test_prediction_display() != 0 ) {
    return 1;
  }
  printf( "test_startup_options: all cases passed\n" );
  return 0;
}
