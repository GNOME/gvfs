tests = [
  'outputstream'
]

deps = [
  gio_dep,
  glib_dep,
]

foreach test: tests
  executable(
    test,
    test + '.c',
    include_directories: top_inc,
    dependencies: deps,
  )
endforeach