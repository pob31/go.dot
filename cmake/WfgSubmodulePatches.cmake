# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# WfgSubmodulePatches.cmake — Go.dot's changes to one vendored submodule, applied by
# the build to the submodule's working tree before a single source of it is compiled.
#
# Included once per patched submodule, by a file that says why that one is patched and
# sets two variables first:
#   WFG_PATCHED       the directory name, the same under ThirdParty/ and patches/
#                     (tracktion_engine, JUCE)
#   WFG_PATCHED_NAME  what the messages call it (Tracktion Engine, JUCE)
# WfgTracktionPatches.cmake and WfgJucePatches.cmake are those files. The decision that
# patches live here, applied by the build, rather than in forks, is DQ (2026-09-28);
# JUCE joined it on 2026-10-14. The price, accepted with it: `git status` reports the
# submodule as modified, and moving its pin takes the patches off first (README,
# "Bumping a pin").
#
# WHAT THIS DOES, in the order it asks. The series is patches/${WFG_PATCHED}/series.
#   1. The series reverses cleanly: it is already on. Nothing is touched, so a
#      configure never causes a rebuild of the submodule.
#   2. The series applies cleanly: a fresh checkout of the pin. It is applied, and a
#      copy of what was applied is kept as a stamp beside the submodule's git directory.
#   3. The stamped series reverses cleanly: a patch was edited, or one added, since
#      it was last applied. The old series comes off and the new one goes on, so an
#      edit heals itself in every build tree of the checkout.
#   4. Anything else stops the configure with a message that says how to look at
#      the tree and how to clean it. A tree this step does not recognise is somebody's
#      work, and it is never reset from here.
#
# One configure at a time: four build trees share this checkout (build/vs,
# build/strict-vs, build/ci-windows, build/spikes) and so do several sessions, and
# two configures applying the same patch at once would each see the other's half.
# The lock sits beside the submodule's git directory, which all of them share.
#
# Straight-line CMake, no functions or macros, for the reason WfgThirdParty.cmake
# gives at its top: every name it uses is set again by the next inclusion before it
# is read. It speaks only in STATUS lines and FATAL errors: the Linux job fails on any
# CMake Warning from outside ThirdParty/.

find_package(Git REQUIRED)

set(_wfg_patched_dir "${CMAKE_SOURCE_DIR}/ThirdParty/${WFG_PATCHED}")
set(_wfg_patch_dir "${CMAKE_SOURCE_DIR}/patches/${WFG_PATCHED}")

# The series, in order: one file name a line, `#` for a comment.
file(STRINGS "${_wfg_patch_dir}/series" _wfg_series_lines)
set(_wfg_patches "")
set(_wfg_signature "")

foreach(_wfg_line IN LISTS _wfg_series_lines)
    string(STRIP "${_wfg_line}" _wfg_line)

    if(_wfg_line STREQUAL "" OR _wfg_line MATCHES "^#")
        continue()
    endif()

    if(NOT EXISTS "${_wfg_patch_dir}/${_wfg_line}")
        message(FATAL_ERROR
            "Go.dot: patches/${WFG_PATCHED}/series names ${_wfg_line}, which is not in "
            "patches/${WFG_PATCHED}/.")
    endif()

    list(APPEND _wfg_patches "${_wfg_patch_dir}/${_wfg_line}")
    file(SHA256 "${_wfg_patch_dir}/${_wfg_line}" _wfg_hash)
    string(APPEND _wfg_signature "${_wfg_line} ${_wfg_hash}\n")
endforeach()

# An edit to the series or to any patch re-runs the configure, and re-running the
# configure is what re-applies it.
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_wfg_patch_dir}/series" ${_wfg_patches})

execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${_wfg_patched_dir}"
    OUTPUT_VARIABLE _wfg_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _wfg_rc
    ERROR_QUIET)

if(NOT _wfg_rc EQUAL 0 OR _wfg_git_dir STREQUAL "")
    message(FATAL_ERROR
        "Go.dot: ThirdParty/${WFG_PATCHED} is not a git checkout, so Go.dot's patches "
        "cannot be applied to it.\n"
        "    Fix: scripts/bootstrap.sh   (or: git submodule update --init ThirdParty/${WFG_PATCHED})")
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${_wfg_patched_dir}"
    OUTPUT_VARIABLE _wfg_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

list(LENGTH _wfg_patches _wfg_count)

if(_wfg_count EQUAL 0)
    # An empty series would hand `git apply` no file, and it would read its
    # standard input instead: the configure would wait for ever on nothing.
    message(STATUS "Go.dot: no patches for ${WFG_PATCHED_NAME} ${_wfg_head}")
else()
    set(_wfg_stamp_dir "${_wfg_git_dir}/wfg-applied")

    file(LOCK "${_wfg_git_dir}/wfg-patches.lock" GUARD FILE TIMEOUT 300
         RESULT_VARIABLE _wfg_lock)

    if(NOT _wfg_lock STREQUAL "0")
        message(FATAL_ERROR
            "Go.dot: waited five minutes for another configure to finish with the "
            "${WFG_PATCHED_NAME} patches, and it has not (${_wfg_lock}).")
    endif()

    set(_wfg_reversed ${_wfg_patches})
    list(REVERSE _wfg_reversed)

    # 1. Already on?
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check ${_wfg_reversed}
        WORKING_DIRECTORY "${_wfg_patched_dir}"
        RESULT_VARIABLE _wfg_on
        OUTPUT_QUIET ERROR_QUIET)

    set(_wfg_state "")

    if(_wfg_on EQUAL 0)
        set(_wfg_state "carries")
    else()
        # 2. A clean checkout of the pin?
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check ${_wfg_patches}
            WORKING_DIRECTORY "${_wfg_patched_dir}"
            RESULT_VARIABLE _wfg_fits
            OUTPUT_QUIET ERROR_QUIET)

        if(_wfg_fits EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${_wfg_patches}
                WORKING_DIRECTORY "${_wfg_patched_dir}"
                RESULT_VARIABLE _wfg_rc
                ERROR_VARIABLE _wfg_err)

            if(NOT _wfg_rc EQUAL 0)
                message(FATAL_ERROR "Go.dot: applying the ${WFG_PATCHED_NAME} patches failed after they checked clean:\n${_wfg_err}")
            endif()

            set(_wfg_state "was given")
        else()
            # 3. The series applied last time, since edited?
            set(_wfg_old "")

            if(EXISTS "${_wfg_stamp_dir}/series")
                file(STRINGS "${_wfg_stamp_dir}/series" _wfg_old_lines)

                foreach(_wfg_line IN LISTS _wfg_old_lines)
                    string(STRIP "${_wfg_line}" _wfg_line)

                    if(_wfg_line STREQUAL "" OR _wfg_line MATCHES "^#")
                        continue()
                    endif()

                    if(EXISTS "${_wfg_stamp_dir}/${_wfg_line}")
                        list(APPEND _wfg_old "${_wfg_stamp_dir}/${_wfg_line}")
                    endif()
                endforeach()
            endif()

            if(_wfg_old)
                set(_wfg_old_reversed ${_wfg_old})
                list(REVERSE _wfg_old_reversed)

                execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check ${_wfg_old_reversed}
                    WORKING_DIRECTORY "${_wfg_patched_dir}"
                    RESULT_VARIABLE _wfg_old_on
                    OUTPUT_QUIET ERROR_QUIET)

                if(_wfg_old_on EQUAL 0)
                    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse ${_wfg_old_reversed}
                        WORKING_DIRECTORY "${_wfg_patched_dir}"
                        RESULT_VARIABLE _wfg_rc
                        ERROR_VARIABLE _wfg_err)

                    if(NOT _wfg_rc EQUAL 0)
                        message(FATAL_ERROR "Go.dot: taking the previous ${WFG_PATCHED_NAME} patches off failed after they checked clean:\n${_wfg_err}")
                    endif()

                    execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${_wfg_patches}
                        WORKING_DIRECTORY "${_wfg_patched_dir}"
                        RESULT_VARIABLE _wfg_rc
                        ERROR_VARIABLE _wfg_err)

                    if(NOT _wfg_rc EQUAL 0)
                        # The new series does not fit the pin at all. Put the old one
                        # back, so the tree is left exactly as it was found.
                        execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${_wfg_old}
                            WORKING_DIRECTORY "${_wfg_patched_dir}"
                            OUTPUT_QUIET ERROR_QUIET)

                        message(FATAL_ERROR
                            "Go.dot: patches/${WFG_PATCHED}/series does not apply to "
                            "${WFG_PATCHED_NAME} ${_wfg_head}; the tree was left carrying the series applied "
                            "before.\n${_wfg_err}\n"
                            "    Rework the patch: python3 scripts/patches.py ${WFG_PATCHED} apply --3way, "
                            "resolve, then refresh.")
                    endif()

                    set(_wfg_state "was given the edited")
                endif()
            endif()

            if(_wfg_state STREQUAL "")
                execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${_wfg_patched_dir}"
                    OUTPUT_VARIABLE _wfg_dirty
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)

                if(_wfg_dirty STREQUAL "")
                    message(FATAL_ERROR
                        "Go.dot: patches/${WFG_PATCHED}/series does not apply to a clean "
                        "checkout of ${WFG_PATCHED_NAME} ${_wfg_head}.\n"
                        "    If the pin has just moved, the patches need reworking against it:\n"
                        "        python3 scripts/patches.py ${WFG_PATCHED} apply --3way   (resolve what does not fit)\n"
                        "        python3 scripts/patches.py ${WFG_PATCHED} refresh\n"
                        "    README, \"Bumping a pin\", has the whole procedure.")
                endif()

                message(FATAL_ERROR
                    "Go.dot: ThirdParty/${WFG_PATCHED} has changes that are neither Go.dot's "
                    "patches (patches/${WFG_PATCHED}/series) nor a clean checkout of "
                    "${_wfg_head}. They were left alone.\n"
                    "    See them:      git -C ThirdParty/${WFG_PATCHED} status   (and: diff)\n"
                    "    Discard them:  git -C ThirdParty/${WFG_PATCHED} checkout -- .\n"
                    "    then configure again. Before pulling a commit that moves the ${WFG_PATCHED_NAME}\n"
                    "    pin, take the patches off: python3 scripts/patches.py ${WFG_PATCHED} revert")
            endif()
        endif()
    endif()

    # The stamp: what is on the tree now, where every build tree of this checkout, and
    # scripts/patches.py, can find it. Rewritten only when it would change, so a
    # configure with nothing to do writes nothing.
    set(_wfg_stamped "")

    if(EXISTS "${_wfg_stamp_dir}/signature")
        file(READ "${_wfg_stamp_dir}/signature" _wfg_stamped)
    endif()

    if(NOT _wfg_stamped STREQUAL _wfg_signature)
        file(REMOVE_RECURSE "${_wfg_stamp_dir}")
        file(MAKE_DIRECTORY "${_wfg_stamp_dir}")
        file(COPY "${_wfg_patch_dir}/series" ${_wfg_patches} DESTINATION "${_wfg_stamp_dir}")
        file(WRITE "${_wfg_stamp_dir}/signature" "${_wfg_signature}")
    endif()

    file(LOCK "${_wfg_git_dir}/wfg-patches.lock" RELEASE)

    message(STATUS "Go.dot: ${WFG_PATCHED_NAME} ${_wfg_head} ${_wfg_state} ${_wfg_count} patch(es) "
                   "from patches/${WFG_PATCHED}/series")
endif()
