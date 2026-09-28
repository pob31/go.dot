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

# WfgTracktionPatches.cmake — Go.dot's changes to Tracktion Engine, applied by the
# build to the submodule's working tree before a single Tracktion source is compiled.
#
# WHY THERE ARE ANY. A media cue's speed (namespace draft §22) needs two things
# Tracktion cannot do at its pin: resample an auto-tempo clip while a time-stretcher
# is compiled in, and change the speed of a clip that is playing in a launcher slot.
# Both are small changes inside Tracktion. The author's decision DQ (2026-09-28) is
# that they live here, in Go.dot's own repository beside the code that needs them,
# and that the build applies them - rather than in a fork of Tracktion. The price,
# accepted with the decision: `git status` reports ThirdParty/tracktion_engine as
# modified, and moving the pin takes the patches off first (README, "Bumping a pin").
#
# WHAT THIS DOES, in the order it asks. The series is patches/tracktion_engine/series.
#   1. The series reverses cleanly: it is already on. Nothing is touched, so a
#      configure never causes a rebuild of Tracktion.
#   2. The series applies cleanly: a fresh checkout of the pin. It is applied, and a
#      copy of what was applied is kept as a stamp beside Tracktion's git directory.
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
# The lock sits beside Tracktion's git directory, which all of them share.
#
# Included from WfgThirdParty.cmake just before Tracktion's modules are added, so
# what CMake reads from them is the patched tree. Straight-line CMake, no functions
# or macros, for the reason WfgThirdParty.cmake gives at its top. It speaks only in
# STATUS lines and FATAL errors: the Linux job fails on any CMake Warning from outside
# ThirdParty/.

find_package(Git REQUIRED)

set(WFG_TE_DIR "${CMAKE_SOURCE_DIR}/ThirdParty/tracktion_engine")
set(WFG_TE_PATCH_DIR "${CMAKE_SOURCE_DIR}/patches/tracktion_engine")

# The series, in order: one file name a line, `#` for a comment.
file(STRINGS "${WFG_TE_PATCH_DIR}/series" _wfg_series_lines)
set(WFG_TE_PATCHES "")
set(_wfg_signature "")

foreach(_wfg_line IN LISTS _wfg_series_lines)
    string(STRIP "${_wfg_line}" _wfg_line)

    if(_wfg_line STREQUAL "" OR _wfg_line MATCHES "^#")
        continue()
    endif()

    if(NOT EXISTS "${WFG_TE_PATCH_DIR}/${_wfg_line}")
        message(FATAL_ERROR
            "Go.dot: patches/tracktion_engine/series names ${_wfg_line}, which is not in "
            "patches/tracktion_engine/.")
    endif()

    list(APPEND WFG_TE_PATCHES "${WFG_TE_PATCH_DIR}/${_wfg_line}")
    file(SHA256 "${WFG_TE_PATCH_DIR}/${_wfg_line}" _wfg_hash)
    string(APPEND _wfg_signature "${_wfg_line} ${_wfg_hash}\n")
endforeach()

# An edit to the series or to any patch re-runs the configure, and re-running the
# configure is what re-applies it.
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WFG_TE_PATCH_DIR}/series" ${WFG_TE_PATCHES})

execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${WFG_TE_DIR}"
    OUTPUT_VARIABLE _wfg_te_git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _wfg_rc
    ERROR_QUIET)

if(NOT _wfg_rc EQUAL 0 OR _wfg_te_git_dir STREQUAL "")
    message(FATAL_ERROR
        "Go.dot: ThirdParty/tracktion_engine is not a git checkout, so Go.dot's patches "
        "cannot be applied to it.\n"
        "    Fix: scripts/bootstrap.sh   (or: git submodule update --init ThirdParty/tracktion_engine)")
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${WFG_TE_DIR}"
    OUTPUT_VARIABLE _wfg_te_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

list(LENGTH WFG_TE_PATCHES _wfg_count)

if(_wfg_count EQUAL 0)
    # An empty series would hand `git apply` no file, and it would read its
    # standard input instead: the configure would wait for ever on nothing.
    message(STATUS "Go.dot: no patches for Tracktion Engine ${_wfg_te_head}")
else()
    set(_wfg_stamp_dir "${_wfg_te_git_dir}/wfg-applied")

    file(LOCK "${_wfg_te_git_dir}/wfg-patches.lock" GUARD FILE TIMEOUT 300
         RESULT_VARIABLE _wfg_lock)

    if(NOT _wfg_lock STREQUAL "0")
        message(FATAL_ERROR
            "Go.dot: waited five minutes for another configure to finish with the "
            "Tracktion patches, and it has not (${_wfg_lock}).")
    endif()

    set(_wfg_reversed ${WFG_TE_PATCHES})
    list(REVERSE _wfg_reversed)

    # 1. Already on?
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check ${_wfg_reversed}
        WORKING_DIRECTORY "${WFG_TE_DIR}"
        RESULT_VARIABLE _wfg_on
        OUTPUT_QUIET ERROR_QUIET)

    set(_wfg_state "")

    if(_wfg_on EQUAL 0)
        set(_wfg_state "carries")
    else()
        # 2. A clean checkout of the pin?
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check ${WFG_TE_PATCHES}
            WORKING_DIRECTORY "${WFG_TE_DIR}"
            RESULT_VARIABLE _wfg_fits
            OUTPUT_QUIET ERROR_QUIET)

        if(_wfg_fits EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${WFG_TE_PATCHES}
                WORKING_DIRECTORY "${WFG_TE_DIR}"
                RESULT_VARIABLE _wfg_rc
                ERROR_VARIABLE _wfg_err)

            if(NOT _wfg_rc EQUAL 0)
                message(FATAL_ERROR "Go.dot: applying the Tracktion patches failed after they checked clean:\n${_wfg_err}")
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
                    WORKING_DIRECTORY "${WFG_TE_DIR}"
                    RESULT_VARIABLE _wfg_old_on
                    OUTPUT_QUIET ERROR_QUIET)

                if(_wfg_old_on EQUAL 0)
                    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse ${_wfg_old_reversed}
                        WORKING_DIRECTORY "${WFG_TE_DIR}"
                        RESULT_VARIABLE _wfg_rc
                        ERROR_VARIABLE _wfg_err)

                    if(NOT _wfg_rc EQUAL 0)
                        message(FATAL_ERROR "Go.dot: taking the previous Tracktion patches off failed after they checked clean:\n${_wfg_err}")
                    endif()

                    execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${WFG_TE_PATCHES}
                        WORKING_DIRECTORY "${WFG_TE_DIR}"
                        RESULT_VARIABLE _wfg_rc
                        ERROR_VARIABLE _wfg_err)

                    if(NOT _wfg_rc EQUAL 0)
                        # The new series does not fit the pin at all. Put the old one
                        # back, so the tree is left exactly as it was found.
                        execute_process(COMMAND "${GIT_EXECUTABLE}" apply ${_wfg_old}
                            WORKING_DIRECTORY "${WFG_TE_DIR}"
                            OUTPUT_QUIET ERROR_QUIET)

                        message(FATAL_ERROR
                            "Go.dot: patches/tracktion_engine/series does not apply to Tracktion "
                            "Engine ${_wfg_te_head}; the tree was left carrying the series applied "
                            "before.\n${_wfg_err}\n"
                            "    Rework the patch: python3 scripts/te-patches.py apply --3way, "
                            "resolve, then refresh.")
                    endif()

                    set(_wfg_state "was given the edited")
                endif()
            endif()

            if(_wfg_state STREQUAL "")
                execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${WFG_TE_DIR}"
                    OUTPUT_VARIABLE _wfg_dirty
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)

                if(_wfg_dirty STREQUAL "")
                    message(FATAL_ERROR
                        "Go.dot: patches/tracktion_engine/series does not apply to a clean "
                        "checkout of Tracktion Engine ${_wfg_te_head}.\n"
                        "    If the pin has just moved, the patches need reworking against it:\n"
                        "        python3 scripts/te-patches.py apply --3way   (resolve what does not fit)\n"
                        "        python3 scripts/te-patches.py refresh\n"
                        "    README, \"Bumping a pin\", has the whole procedure.")
                endif()

                message(FATAL_ERROR
                    "Go.dot: ThirdParty/tracktion_engine has changes that are neither Go.dot's "
                    "patches (patches/tracktion_engine/series) nor a clean checkout of "
                    "${_wfg_te_head}. They were left alone.\n"
                    "    See them:      git -C ThirdParty/tracktion_engine status   (and: diff)\n"
                    "    Discard them:  git -C ThirdParty/tracktion_engine checkout -- .\n"
                    "    then configure again. Before pulling a commit that moves the Tracktion\n"
                    "    pin, take the patches off: python3 scripts/te-patches.py revert")
            endif()
        endif()
    endif()

    # The stamp: what is on the tree now, where every build tree of this checkout, and
    # scripts/te-patches.py, can find it. Rewritten only when it would change, so a
    # configure with nothing to do writes nothing.
    set(_wfg_stamped "")

    if(EXISTS "${_wfg_stamp_dir}/signature")
        file(READ "${_wfg_stamp_dir}/signature" _wfg_stamped)
    endif()

    if(NOT _wfg_stamped STREQUAL _wfg_signature)
        file(REMOVE_RECURSE "${_wfg_stamp_dir}")
        file(MAKE_DIRECTORY "${_wfg_stamp_dir}")
        file(COPY "${WFG_TE_PATCH_DIR}/series" ${WFG_TE_PATCHES} DESTINATION "${_wfg_stamp_dir}")
        file(WRITE "${_wfg_stamp_dir}/signature" "${_wfg_signature}")
    endif()

    file(LOCK "${_wfg_te_git_dir}/wfg-patches.lock" RELEASE)

    message(STATUS "Go.dot: Tracktion Engine ${_wfg_te_head} ${_wfg_state} ${_wfg_count} patch(es) "
                   "from patches/tracktion_engine/series")
endif()
