mkdepgraph = executable('mkdepgraph',
  sources: ['mkdepgraph.c'],
  include_directories: [top_srcdir],
  dependencies: [anthy_dep, anthydic_dep],
  c_args: '-DSRCDIR="'+meson.current_source_dir()+'"',
)

anthy_dep_graph = custom_target('anthy.dep',
  output: ['anthy.dep'],
  command: [mkdepgraph, '-o', '@OUTPUT0@'],
)
