cmake_minimum_required(VERSION 3.24)
# The version the app REPORTS -- About and the Report an Issue mail body both
# read it through TRACE_VERSION_STRING (app/CMakeLists.txt), so it has to track
# the tag or every issue report carries the wrong build. Bumped to 0.2.0 for
# the interface pass (spec phases 6-16); the tag is v0.2.0-alpha.1 and the
# "(alpha)" suffix is added by buildIdentity() rather than living here.
project(Trace VERSION 0.2.0 LANGUAGES CXX)

add_subdirectory(app)
