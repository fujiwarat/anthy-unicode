anthy = executable('anthy',
  sources: 'main.c',
  include_directories: [top_srcdir],
  dependencies: [convdb_dep, anthy_dep, anthydic_dep],
  c_args: ['-DSRCDIR="'+meson.current_source_dir()+'"', '-DTEST_HOME="'+meson.current_build_dir()+'"'],
)

checklib = executable('checklib',
  sources: 'check.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: ['-DSRCDIR="'+meson.current_source_dir()+'"', '-DTEST_HOME="'+meson.current_build_dir()+'"'],
)

prediction = executable('prediction',
  sources: 'prediction.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: ['-DSRCDIR="'+meson.current_source_dir()+'"', '-DTEST_HOME="'+meson.current_build_dir()+'"'],
)

test_matrix = executable('test-matrix',
  sources: 'test-matrix.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: ['-DSRCDIR="'+meson.current_source_dir()+'"', '-DTEST_HOME="'+meson.current_build_dir()+'"'],
)

test('Main anthy tests', anthy, args: ['--all'])
test('Library load tests', checklib)
test('Anthy prediction tests', prediction)
test('Anthy matrix tests', test_matrix)
