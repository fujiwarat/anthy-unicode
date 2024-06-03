mkfiledic = executable('mkfiledic',
  sources: 'mkfiledic.c',
  include_directories: [top_srcdir],
  dependencies: [diclib_dep],
)

anthy_word_dic = custom_target('anthy.dic',
  input: [ anthy_word_dic, anthy_dep_graph, anthy_transition_matrix],
  output: ['anthy.dic'],
  command: [mkfiledic, '-p', meson.project_build_root(), '-o', '@OUTPUT0@'],
  install: true,
  install_dir: get_option('datadir') / meson.project_name(),
)
