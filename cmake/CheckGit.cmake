# In case Git is not available, default is empty string
set(GIT_HASH "")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    OUTPUT_VARIABLE D3_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  # Append "-dirty" when the working tree has uncommitted TRACKED changes, so a build made
  # from un-committed edits is obvious in the dedicated banner and the main-menu version
  # string (an in-progress / not-yet-tested build should never look like a clean release).
  # This restores the upstream `git describe --dirty` signal that was dropped in 564ce0e9 —
  # that commit switched to rev-parse to kill the menu tag double-display (keep that win);
  # this only re-adds the dirty marker on top, without the tag. Untracked files are ignored
  # (-uno) to match describe semantics and avoid false-dirty from local navdump/export
  # clutter (git-hash.txt is itself gitignored, so it can't self-trigger).
  if(D3_GIT_HASH)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
      OUTPUT_VARIABLE D3_GIT_DIRTY
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(NOT "${D3_GIT_DIRTY}" STREQUAL "")
      set(D3_GIT_HASH "${D3_GIT_HASH}-dirty")
    endif()
  endif()
  file(WRITE ${SOURCE_DIR}/git-hash.txt ${D3_GIT_HASH})
else()
  # Try to read pregenerated file with commit hash on it (archive version of repository)
  if(EXISTS ${SOURCE_DIR}/git-hash.txt)
    file(READ ${SOURCE_DIR}/git-hash.txt D3_GIT_HASH)
  endif()
endif()

message(STATUS "Git hash is ${D3_GIT_HASH}")

configure_file(
  ${SOURCE_DIR}/lib/d3_version.h.in
  ${TARGET_DIR}/lib/d3_version.h
  @ONLY
)
