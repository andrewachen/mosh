/* ABOUTME: Typed, owned, validated Windows startup-config snapshot. */
/* ABOUTME: Aggregates the shared frontend parsers; validated before mosh_bootstrap. */
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

#ifndef WIN32_STARTUP_OPTIONS_H
#define WIN32_STARTUP_OPTIONS_H

#include <string>

#include "src/frontend/startup_config.h"

struct StartupOptions {
  Overlay::PredictionEngine::DisplayPreference predict_display = Overlay::PredictionEngine::Adaptive;
  EscapeConfig escape;
};

struct StartupEnv {
  const char *predict_display = nullptr;  /* MOSH_PREDICTION_DISPLAY */
  const char *escape_key = nullptr;  /* MOSH_ESCAPE_KEY */
};

/* Build the validated snapshot from the environment. Returns false and sets
   *error on the first locally detectable configuration error, so the caller
   can refuse before starting a remote server. */
bool parse_startup_options( const StartupEnv &env, StartupOptions *out, std::string *error );

#endif
