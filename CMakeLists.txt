cmake_minimum_required(VERSION 3.24)
# The version the app REPORTS -- About and the Report an Issue mail body both
# read it through TRACE_VERSION_STRING (app/CMakeLists.txt), so it has to track
# the tag or every issue report carries the wrong build.
#
# Bumped to 0.3.0 for the UI redesign (roadmap steps 2-11). The MINOR moved
# rather than the patch because the shipping window is unrecognisable from
# v0.2.0-beta.1: the menu bar and status bar left the layout, the diagnostics
# HUD ships hidden, the floating transport panel became an edge-to-edge strip
# with ten controls, and the chrome, empty state and typography are all new.
# The playback engine underneath is unchanged and was re-measured flat.
#
# CMake's VERSION field cannot hold a prerelease suffix, so 0.3.0 covers the
# whole of this line and the SUFFIX is what distinguishes its releases. That
# suffix is a literal in buildIdentity(), so moving beta -> rc -> release is an
# edit in one function -- and if it is ever missed the number still looks right
# while every issue report names the wrong stage. The stage did NOT move here:
# all three "beta" literals in MainWindow.cpp are correct as they stand.
project(Trace VERSION 0.3.0 LANGUAGES CXX)

add_subdirectory(app)
