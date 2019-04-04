sources = files(
  'dirwatch.c',
  'trashdir.c',
  'trashitem.c',
  'trashwatcher.c',
  'trashexpunge.c',
)

deps = [
  gio_dep,
  gio_unix_dep,
  glib_dep,
]

libtrash = static_library(
  'trash',
  sources: sources,
  include_directories: top_inc,
  dependencies: deps,
)

libtrash_dep = declare_dependency(
  include_directories: '.',
  dependencies: gio_dep,
  link_with: libtrash,
)
