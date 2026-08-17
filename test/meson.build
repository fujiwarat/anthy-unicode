test_c_args = ['-DSRCDIR="@0@"'.format(meson.current_source_dir()),
               '-DTEST_HOME="@0@"'.format(meson.current_build_dir())]

anthy = executable('anthy',
  sources: 'main.c',
  include_directories: [top_srcdir],
  dependencies: [convdb_dep, anthy_dep, anthydic_dep],
  c_args: test_c_args,
)

checklib = executable('checklib',
  sources: 'check.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: test_c_args,
)

prediction = executable('prediction',
  sources: 'prediction.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: test_c_args,
)

test_matrix = executable('test-matrix',
  sources: 'test-matrix.c',
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: test_c_args,
)

test('Main anthy tests', anthy,
  args: ['--all'],
  workdir: meson.current_build_dir(),
  depends: [ anthy_word_dic ]
)
test('Library load tests', checklib,
  workdir: meson.current_build_dir(),
  depends: [ anthy_word_dic ]
)
#test('Anthy prediction tests', prediction,
#  args: ['わ'],
#  workdir: meson.current_build_dir(),
#  depends: [ anthy_word_dic, anthy_dep_graph ]
#)
test('Anthy matrix tests', test_matrix, workdir: meson.current_build_dir())
