# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-src")
  file(MAKE_DIRECTORY "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-src")
endif()
file(MAKE_DIRECTORY
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-build"
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix"
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/tmp"
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/src/nats-populate-stamp"
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/src"
  "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/src/nats-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/src/nats-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/macbas/work/AJ/crop-preparator/build/_deps/nats-subbuild/nats-populate-prefix/src/nats-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
