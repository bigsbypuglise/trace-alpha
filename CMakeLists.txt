cmake_minimum_required(VERSION 3.24)
# The version the app REPORTS -- About and the Report an Issue mail body both
# read it through TRACE_VERSION_STRING (app/CMakeLists.txt), so it has to track
# the tag or every issue report carries the wrong build.
#
# Bumped to 0.4.0 for the EXR + colour phase. The MINOR moved by the same test
# 0.3.0 used and passes it in the other direction: 0.3.0 changed how Trace
# LOOKS, this changes what Trace can OPEN. A player that could not read an EXR
# at all now reads stills and numbered sequences as scene-linear float, cycles
# multilayer passes, and carries an OpenColorIO transform stage with LUT and
# display/view. Two new third-party libraries ship inside the package
# (OpenImageIO 3.1.14.0, OpenColorIO 2.5.2), taking the ZIP 95.4 -> 118.3 MB,
# and Copy Current Frame changed behaviour for video as well as EXR.
# The playback engine underneath is unchanged and was re-measured flat at the
# panel -- docs/exr-panel-regression-260825.md, 88 scrub legs at delta 0.
#
# The 0.3.0 line was the UI redesign (roadmap steps 2-11): the menu bar and
# status bar left the layout, the diagnostics HUD shipped hidden, the floating
# transport became an edge-to-edge strip, and the chrome, empty state and
# typography were all new.
#
# CMake's VERSION field cannot hold a prerelease suffix, so 0.4.0 covers the
# whole of this line and the SUFFIX is what distinguishes its releases. That
# suffix is a literal in buildIdentity(), so moving beta -> rc -> release is an
# edit in one function -- and if it is ever missed the number still looks right
# while every issue report names the wrong stage. The stage did NOT move here:
# the "beta" literals in MainWindow.cpp are correct as they stand.
#
# THERE ARE FOUR OF THEM, NOT THREE. This comment said three until 2026-08-26
# and CLAUDE.md said the same. The fourth is the --scrub-selftest report header
# (`build: Trace %1 (beta)`), added when that selftest was built and never added
# to the recorded count -- and it is in the block a tester pastes back. The four:
# the Report an Issue mail SUBJECT, buildIdentity(), the About dialog's small
# print (capital "Beta.", so a lower-case-only search misses it), and the
# selftest header. Verify against the BUILT BINARY, not the source.
project(Trace VERSION 0.4.0 LANGUAGES CXX)

add_subdirectory(app)
