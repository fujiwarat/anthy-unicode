mkworddic_sources = [
  'mkdic.c',
  'writewords.c',
  'mkudic.c',
  'calcfreq.c',
]

mkworddic = executable('mkworddic',
  sources: mkworddic_sources,
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
)

dict_args_conf = configuration_data()
dict_args_conf.set('top_srcdir', meson.project_source_root())
dict_args = configure_file(input: 'dict.args.in', output: 'dict.args', configuration: dict_args_conf)

anthy_word_dic = custom_target('anthy.wdic',
  input: [dict_args],
  output: ['anthy.wdic'],
  command: [mkworddic, '-f', './@INPUT0@', '-o', '@OUTPUT0@'],
)

install_data('zipcode.t', install_dir : get_option('datadir') / meson.project_name())
