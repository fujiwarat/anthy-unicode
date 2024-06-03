calctrans_sources = [
  'calctrans.c',
  'input_set.c',
  'corpus.c',
]

proccorpus_sources = [
  'proccorpus.c',
]

calctrans = executable('calctrans',
  sources: calctrans_sources,
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
)

#TODO: Update params
proccorpus = executable('proccorpus',
  sources: calctrans_sources,
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
)

anthy_transition_matrix = custom_target('anthy.[cand_info|trans_info|corpus_array|corpus_bucket|weak_words]',
  input: ['corpus_info', 'weak_words'],
  output: ['anthy.cand_info', 'anthy.trans_info', 'anthy.corpus_array', 'anthy.corpus_bucket', 'anthy.weak_words'],
  command: [calctrans, '-c', '@INPUT0@', '@INPUT1@', '-d', meson.current_build_dir()],
)
