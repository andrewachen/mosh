/* ABOUTME: Assembles the Windows StartupOptions snapshot from the shared parsers. */
/* ABOUTME: Refuses locally detectable errors before any remote server is started. */
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

#include <string>

#include "win32/startup_options.h"
#include "src/frontend/startup_config.h"

bool parse_startup_options( const StartupEnv &env, StartupOptions *out, std::string *error )
{
  *out = StartupOptions();

  std::string predict_error;
  if ( !parse_prediction_display( env.predict_display, &out->predict_display, &predict_error ) ) {
    *error = std::string( "mosh: " ) + predict_error + " (MOSH_PREDICTION_DISPLAY)";
    return false;
  }

  out->predict_overwrite = parse_prediction_overwrite( env.predict_overwrite );
  out->escape = parse_escape_key( env.escape_key );
  out->title_prefix = wants_title_prefix( env.title_noprefix );
  out->no_term_init = env.no_term_init != nullptr;
  return true;
}
