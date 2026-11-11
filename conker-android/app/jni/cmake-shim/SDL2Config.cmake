# SDL2 is built in-tree (add_subdirectory) for Android; the toolkit's
# find_package(SDL2) only needs the target that already exists.
if(NOT TARGET SDL2::SDL2)
  message(FATAL_ERROR "SDL2::SDL2 must be defined before xboxrecomp")
endif()
set(SDL2_FOUND TRUE)
