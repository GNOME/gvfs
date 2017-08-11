sources = files(
  'dirwatch.c',
  'trashdir.c',
  'trashitem.c',
  'trashwatcher.c',
  'trashexpunge.c'
)

libtrash = static_library(
  'trash',
  sources: sources,
  include_directories: top_inc,
  dependencies: glib_deps
)

libtrash_dep = declare_dependency(
  link_with: libtrash,
  include_directories: include_directories('.')
)
