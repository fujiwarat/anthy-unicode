/* ライブラリの関数呼び出しのテスト
 *
 * デフォルトでは、test.txtから1行ずつ読み込んで変換を行う。
 * 変換前の文字列と変換を行った結果をtest.expから探し、
 * 変換結果が合っているかをカウントして最後に出力する。
 *
 * ./anthy --from 1 --to 10 のように実行するとtest.txtの最初の10個の
 *  行の変換テストが行われます。
 *
 * --askオプションを付けて実行すると、結果が合っているかの判断を
 * 設定するモードになるので、表示された結果に対する判断を
 * 標準入力から'y', 'n', 'd', 'q'で入力してください。
 * 'd' dont care, 'q' quit
 * 判断できない場合はその他の文字を入力してください。
 *
 * Copyright (C) 2000-2006 TABATA Yusuke
 * Copyright (C) 2004-2006 YOSHIDA Yuichi
 * Copyright (C) 2001-2002 TAKAI Kosuke
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <anthy/anthy.h>
#include <anthy/convdb.h>
#include <anthy/xstr.h>
#include <config.h>

/* Makefile の $(srcdir) (静的データファイルの基準ディレクトリ) */
#ifndef SRCDIR
# define SRCDIR "."
#endif
/* ビルド時のカレントディレクトリ (ここに .anthy を作る) */
#ifndef TEST_HOME
# define TEST_HOME "."		/* FIXME: 実際は相対パスだと誤動作する */
#endif

/* テストデータとなる変換前の文字列 */
#define TESTDATA "test.txt"
static char *testdata; /* = SRCDIR "/" TESTDATA */

/* 変換後の文字列が妥当かどうかをチェックするためのデータ */
#define EXPDATA "test.exp"
const char *expdata = SRCDIR "/" EXPDATA;

struct input {
  char *str;
  int serial;
};

/* テストを行う条件 */
struct condition {
  /* conversion condition */
  int serial;
  int from;
  int to;
  /* operation */
  int ask;
  int quiet;
  int miss_only;
  int use_utf8;
};

static int
read_file(FILE *fp, struct input *in)
{
  char buf[256];
  while(fgets(buf, 256, fp)) {
    switch(buf[0]){
    case '#':
    case ':':
    case '-':
      break;
    case '*':
      if (in->str) {
	free(in->str);
	in->str = 0;
      }
      buf[strlen(buf)-1] = 0;
      in->str = strdup(&buf[1]);
      in->serial ++;
      return 0;
      break;
    }
  }
  return -1;
}

static int
check_cond(struct condition *cond, struct input *in)
{
  if (cond->from == -1 && cond->to == -1)
    return 1;
  else if (in->serial == cond->serial)
    return 1;
  else if (in->serial <= cond->to && in->serial >= cond->from)
    return 1;
  return 0;
}

static void
log_print(int lv, const char *msg)
{
  fprintf(stderr, "log:%d:%s\n", lv, msg);
}

static anthy_context_t
init_lib(int use_utf8)
{
  anthy_context_t ac;
  /* 既にインストールされているファイルの影響を受けないようにする */
  anthy_conf_override("CONFFILE", "../anthy-unicode.conf");
  anthy_conf_override("HOME", TEST_HOME);
  anthy_conf_override("DIC_FILE", "../mkanthydic/anthy.dic");
  anthy_set_logger(log_print, 0);
  if (anthy_init()) {
    fprintf(stderr, "failed to init anthy\n");
    exit(EXIT_FAILURE);
  }
  anthy_set_personality("");

  ac = anthy_create_context();
  if (use_utf8) {
    anthy_context_set_encoding(ac, ANTHY_UTF8_ENCODING);
    anthy_xstr_set_print_encoding (ANTHY_UTF8_ENCODING);
  } else {
    anthy_context_set_encoding(ac, ANTHY_EUC_JP_ENCODING);
  }
  return ac;
}

static void
print_usage(void)
{
  printf("Anthy "VERSION"\n"
	 "./anthy [test-id]\n"
	 " For example.\n"
	 "  ./anthy 1\n"
	 "  ./anthy --to 100\n"
	 "  ./anthy --from 10 --to 100\n"
	 "  ./anthy --all --print-miss-only --ask\n"
	 "  ./anthy --ll 1\n\n");
  exit(0);
}

static void
parse_args(struct condition *cond, int argc, char **argv)
{
  int i;
  char *arg;
  testdata = strdup(SRCDIR "/" TESTDATA);
  for (i = 1; i < argc; i++) {
    arg = argv[i];
    if (!strncmp(arg, "--", 2)) {
      arg = &arg[2];
      if (!strcmp(arg, "help") || !strcmp(arg, "version")) {
	print_usage();
      }
      if (!strcmp(arg, "all")) {
	cond->from = -1;
	cond->to = -1;
      } else if (!strcmp(arg, "verbose")) {
	cond->quiet = 0;
	//cond->miss_only = 0;
      } else if (!strcmp(arg, "quiet")) {
	cond->quiet = 1;
	cond->miss_only = 0;
      } else if (!strcmp(arg, "ask") ||
		 !strcmp(arg, "query")) {
	cond->ask = 1;
      } else if (!strcmp(arg, "print-miss-only")) {
	cond->miss_only = 1;
      } else if (!strcmp(arg, "euc")) {
	cond->use_utf8 = 0;
      }

      if (i + 1 < argc) {
	if (!strcmp(arg, "from")){
	  cond->from = atoi(argv[i+1]);
	  i++;
	}else if (!strcmp(arg, "to")){
	  cond->to = atoi(argv[i+1]);
	  i++;
	}else if (!strcmp(arg, "ll")) {
	  anthy_set_logger(NULL, atoi(argv[i+1]));
	  i++;
	}
      }
    } else {
      int num = atoi(arg);
      if (num) {
	cond->serial = num;
      } else {
	char *buf = alloca(strlen(SRCDIR)+strlen(arg) + 10);
	sprintf(buf, SRCDIR "/%s.txt", arg);
	free(testdata);
	testdata = strdup(buf);
      }
    }
  }
}

static void
print_run_env(void)
{
  time_t t;
  const char *env;
  env = getenv("ANTHY_ENABLE_DEBUG_PRINT");
  if (!env) {
    env = "";
  }
  printf("ANTHY_ENABLE_DEBUG_PRINT=(%s)\n", env);
  env = getenv("ANTHY_SPLITTER_PRINT");
  if (!env) {
    env = "";
  }
  printf("ANTHY_SPLITTER_PRINT=(%s)\n", env);
  printf("SRCDIR=(%s)\n", SRCDIR);
  t = time(&t);
  printf(PACKAGE "-" VERSION " %s", ctime(&t));
}

static void
sum_up(struct res_db *db, struct conv_res *cr)
{
  struct res_stat *rs;
  cr->used = 1;
  db->total ++;
  if (cr->res_str[0] == '|') {
    rs = &db->split;
  } else {
    rs = &db->res;
  }
  if (cr->check == CHK_OK) {
    rs->ok ++;
  } else if (cr->check == CHK_MISS) {
    rs->miss ++;
  } else if (cr->check == CHK_DONTCARE) {
    rs->dontcare ++;
  } else {
    rs->unknown ++;
  }
}

static void
set_string(struct condition *cond, struct res_db *db,
	   struct input *in, anthy_context_t ac)
{
  struct conv_res *cr1, *cr2;
  int pr;

  anthy_set_string(ac, in->str);

  /* result */
  cr1 = find_conv_res(db, ac, in->str, 1);
  sum_up(db, cr1);
  /* split */
  cr2 = find_conv_res(db, ac, in->str, 0);
  sum_up(db, cr2);

  /**/
  pr = 0;
  if (cond->miss_only) {
    if (cr1->check == CHK_MISS ||
	cr2->check == CHK_MISS) {
      pr = 1;
    }
  } else if (!cond->quiet) {
    pr = 1;
  }

  if (pr) {
    printf("%d:(%s)\n", in->serial, in->str);
    anthy_print_context(ac);
  }
  anthy_reset_context(ac);
}


static void
dump_res(FILE *fp, struct conv_res *r)
{
  fprintf(fp, "%s %s ", r->src_str, r->res_str);
  if (r->check == CHK_MISS) {
    fprintf(fp, "X");
  } else if (r->check == CHK_OK) {
    fprintf(fp, "OK");
  } else if (r->check == CHK_DONTCARE) {
    fprintf(fp, "*");
  } else {
    fprintf(fp, "?");
  }
  fprintf(fp, "\n");
}

static void
save_db(const char *fn, struct res_db *db)
{
  FILE *fp = fopen(fn, "w");
  struct conv_res *cr;
  if (!fp) {
    fprintf(stderr, "failed to open (%s) to write\n", fn);
    return ;
  }
  for (cr = db->res_list.next; cr; cr = cr->next) {
    dump_res(fp, cr);
  }
  fclose(fp);
}

static void
ask_results(struct res_db *db)
{
  struct conv_res *cr;
  for (cr = db->res_list.next; cr; cr = cr->next) {
    if (cr->check == CHK_UNKNOWN && cr->used == 1) {
      char buf[256];
      printf("%s -> %s (y/n/d/q)\n", cr->src_str, cr->res_str);
      if (!fgets(buf, 256, stdin))
        printf("Failed fgets in %s:%d\n", __FILE__, __LINE__);
      if (buf[0] == 'y') {
	cr->check = CHK_OK;
      } else if (buf[0] == 'n') {
	cr->check = CHK_MISS;
      } else if (buf[0] == 'd') {
	cr->check = CHK_DONTCARE;
      } else if (buf[0] == 'q') {
	return ;
      }
    }
  }
}

static void
show_stat(struct res_db *db)
{
  struct res_stat *rs;
  int i;
  /**/
  printf("%d items\n", db->total);
  for (i = 0; i < 2; i++) {
    if (i == 0) {
      printf("conversion result\n");
      rs = &db->res;
    } else {
      printf("split result\n");
      rs = &db->split;
    }
    printf("ok : %d\n", rs->ok);
    printf("miss : %d\n", rs->miss);
    printf("unknown : %d\n", rs->unknown);
    printf("\n");
  }
}

static void
init_condition(struct condition *cond)
{
  cond->serial = 0;
  cond->from = -1;
  cond->to = -1;
  /**/
  cond->quiet = 1;
  cond->ask = 0;
  cond->miss_only = 1;
  cond->use_utf8 = 1;
}

int
main(int argc,char **argv)
{
  anthy_context_t ac;
  FILE *fp;
  struct input cur_input;
  struct res_db *db;
  struct condition cond;
  int line = 1;

  cur_input.serial = 0;
  cur_input.str = NULL;
  init_condition(&cond);

  parse_args(&cond, argc, argv);
  db = create_db();
  read_db(db, expdata);

  printf("./test_anthy --help to print usage.\n");

  print_run_env();

  fp = fopen(testdata, "r");
  if (!fp) {
    fprintf(stderr, "failed to open %s.\n", testdata);
    return EXIT_FAILURE;
  }
  free(testdata);
  
  ac = init_lib(cond.use_utf8);

  /* ファイルを読んでいくループ */
  /* anthy_sort_candidate() spends time in anthy_do_context_set_str()
   */
  while (!read_file(fp, &cur_input)) {
    if (check_cond(&cond, &cur_input)) {
      if (getenv ("DEBUG") && line % 50 == 1) {
        printf ("reading #%03d %s\n", line, cur_input.str);
        fflush (stdout);
      }
      set_string(&cond, db, &cur_input, ac);
    }
    line++;
  }
  fclose(fp);
  free(cur_input.str);
  cur_input.str = NULL;

  anthy_release_context(ac);
  anthy_quit();

  if (cond.ask) {
    /* ユーザに聞く */
    ask_results(db);
  }

  show_stat(db);
  save_db(expdata, db);

  return EXIT_SUCCESS;
}
