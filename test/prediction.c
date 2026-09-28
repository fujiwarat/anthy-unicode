#include <stdlib.h>
#include <stdio.h>
#include <anthy/anthy.h>
#include <anthy/xstr.h>


/* Makefile の $(srcdir) (静的データファイルの基準ディレクトリ) */
#ifndef SRCDIR
# define SRCDIR "."
#endif

static void
init_lib(void)
{
  /* 既にインストールされているファイルの影響を受けないようにする */
  anthy_conf_override("CONFFILE", "../anthy-unicode.conf");
  anthy_conf_override("DEPWORD", "master.depword");
  anthy_conf_override("DEPGRAPH", "../depgraph/anthy.dep");
  anthy_conf_override("DIC_FILE", "../mkanthydic/anthy.dic");
  anthy_conf_override("ANTHYDIR", SRCDIR "/../depgraph");
  if (anthy_init()) {
    fprintf(stderr, "failed to init anthy\n");
    exit(EXIT_FAILURE);
  }
}


int main(int argc, char** argv)
{
  struct anthy_prediction_stat ps;
  anthy_context_t ac;
  int i;
  int use_utf8;

  if (argc == 1) {
    fprintf(stderr, "no input string specified\n");
    return EXIT_FAILURE;
  }
  use_utf8 = 1;
  init_lib();
  ac = anthy_create_context();
  if (!ac) {
    fprintf(stderr, "failed to create context\n");
    return EXIT_FAILURE;
  }
  if (use_utf8) {
    anthy_context_set_encoding(ac, ANTHY_UTF8_ENCODING);
    anthy_xstr_set_print_encoding(ANTHY_UTF8_ENCODING);
  } else {
    anthy_context_set_encoding(ac, ANTHY_EUC_JP_ENCODING);
  }

  anthy_set_prediction_string(ac, argv[1]);
  anthy_get_prediction_stat(ac, &ps);
  if (!ps.nr_prediction) {
    fprintf(stderr, "no predictions found for %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  for (i = 0; i < ps.nr_prediction; ++i) {
    char* buf;
    int len;
    len = anthy_get_prediction(ac, i, NULL, 0);
    buf = malloc(sizeof(char) * (len + 1));
    len = anthy_get_prediction(ac, i, buf, len + 1);
    printf("%s, %d\n", buf, len);
    free(buf);
  }
  anthy_commit_prediction(ac, 0);

  anthy_release_context(ac);

  anthy_quit();

  return 0;
}
