cmake_minimum_required(VERSION 3.24)
# The version the app REPORTS -- About and the Report an Issue mail body both
# read it through TRACE_VERSION_STRING (app/CMakeLists.txt), so it has to track
# the tag or every issue report carries the wrong build. Bumped to 0.2.0 for
# the interface pass (spec phases 6-16); the current tag is v0.2.0-beta.1 and
# the "(beta)" suffix is added by buildIdentity() rather than living here.
#
# CMake's VERSION field cannot hold a prerelease suffix, so 0.2.0 covers both
# the alpha and the beta of this line and the SUFFIX is what distinguishes
# them. That suffix is a literal in buildIdentity(), so moving alpha -> beta
# is an edit in one function -- and if it is ever missed the number still
# looks right while every issue report names the wrong stage.
project(Trace VERSION 0.2.0 LANGUAGES CXX)

add_subdirectory(app)
