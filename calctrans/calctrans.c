/*
 * 文節の遷移行列を作成する
 *
 * このコマンドは二つの機能を持っている。(-cオプションで制御)
 * (1) proccorpusの結果からテキスト形式で経験的格率の表を作る
 * (2) テキスト形式の表からバイナリ形式に変換する
 *
 * morphological-analyzerの出力には下記のマークが付けてある
 * ~ 候補の誤り
 * ! 文節長の誤り
 * ^ 複合文節の2つめ以降の要素
 *
 * generate transition matrix
 *
 * Copyright (C) 2006 HANAOKA Toshiyuki
 * Copyright (C) 2006-2007 TABATA Yusuke
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
 *
 */
/*
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <anthy/anthy.h>
#include <anthy/corpus.h>
#include <anthy/diclib.h>
#include <anthy/feature_set.h>
#include <anthy/logger.h>
#include <anthy/xstr.h>
#include "input_set.h"

#define FEATURE_SET_SIZE NR_EM_FEATURES

#define ARRAY_SIZE 16

struct array {
  int len;
  int f[ARRAY_SIZE];
};

#define MAX_SEGMENT 64

struct segment_info {
  int orig_hash;
  int hash;
};

struct sentence_info {
  int nr_segments;
  struct segment_info segs[MAX_SEGMENT];
};

/* 確率のテーブル */
struct input_info {
  /* 候補全体の素性 */
  struct input_set *cand_is;
  /* 文節の素性 */
  struct input_set *seg_is;
  /* 自立語の全文検索用情報 */
  struct corpus *indep_corpus;

  /**/
  struct array missed_cand_features;

  /**/
  int nth_input_file;

  /* 入力された例文の量に関する情報 */
  int nr_sentences;
  int nr_connections;
};

static struct input_info *
init_input_info(void)
{
  struct input_info *m;
  m = malloc(sizeof(struct input_info));
  m->seg_is = input_set_create();
  m->cand_is = input_set_create();
  m->indep_corpus = corpus_new();
  m->missed_cand_features.len = 0;
  m->nth_input_file = 0;
  m->nr_sentences = 0;
  m->nr_connections = 0;
  return m;
}

/* features=1,2,3,,の形式をparseする */
static void
parse_features(struct array *features, char *s)
{
  char *tok, *str = s;
  tok = strtok(str, ",");
  features->len = 0;
  do {
    features->f[features->len] = atoi(tok);
    features->len++;
    tok = strtok(NULL, ",");
  } while(tok);
}

static void
add_seg_struct_info(struct input_info *m,
		    struct array *features,
		    int weight)
{
  input_set_set_features(m->cand_is, features->f, features->len, weight);
}

static void
set_hash(struct sentence_info *sinfo, int error_class,
	 char tag, int hash)
{
  if (tag == '~') {
    sinfo->segs[sinfo->nr_segments].orig_hash = hash;
  } else {
    sinfo->segs[sinfo->nr_segments].hash = hash;
  }
  if (!error_class) {
    sinfo->nr_segments++;
  }
}

static int
compare_array(struct array *a1, struct array *a2)
{
  int i;
  if (a1->len != a2->len) {
    return 1;
  }
  for (i = 0; i < a1->len; i++) {
    if (a1->f[i] != a2->f[i]) {
      return 1;
    }
  }
  return 0;
}

/* 自立語の行をparseする */
static void
parse_indep(struct input_info *m, struct sentence_info *sinfo,
	    char *line, char *buf, int error_class)
{
  struct array features = { 0, };
  char *s;
  int weight = 1;
  /**/
  s = strstr(buf, "features=");
  if (s) {
    s += 9;
    parse_features(&features, s);
    m->nr_connections ++;
  }
  s = strstr(buf, "hash=");
  if (s) {
    s += 5;
    set_hash(sinfo, error_class, line[0], atoi(s));
  }

  /* 加算する */
  if (error_class) {
    if (line[0] == '~') {
      /* 誤った候補の構造を保存 */
      m->missed_cand_features = features;
    }
    if (line[0] == '!') {
      /* 文節長の誤り */
      input_set_set_features(m->seg_is, features.f, features.len, -weight);
    }
  } else {
    /* 接続行列 */
    input_set_set_features(m->seg_is, features.f, features.len, weight);
    /* 候補の構造 */
    if (m->missed_cand_features.len != 0 &&
	compare_array(&features, &m->missed_cand_features)) {
      /* 正解と異なる構造なら分母に加算 */
      add_seg_struct_info(m, &m->missed_cand_features, -weight);
    }
    m->missed_cand_features.len = 0;
    add_seg_struct_info(m, &features, weight);
  }
}

static void
init_sentence_info(struct sentence_info *sinfo)
{
  int i;
  sinfo->nr_segments = 0;
  for (i = 0; i < MAX_SEGMENT; i++) {
    sinfo->segs[i].orig_hash = 0;
    sinfo->segs[i].hash = 0;
  }
}

/* 一つの文を読んだときに全文検索用のデータを作る
 */
static void
complete_sentence_info(struct input_info *m, struct sentence_info *sinfo)
{
  int i;
  if (m->nth_input_file > 0) {
    /* 二つめ以降の入力ファイルは使わない */
    return ;
  }
  for (i = 0; i < sinfo->nr_segments; i++) {
    int flags = ELM_NONE;
    int nr = 1;
    int buf[2];
    if (i == 0) {
      flags |= ELM_BOS;
    }
    /**/
    buf[0] = sinfo->segs[i].hash;
    if (sinfo->segs[i].orig_hash) {
      /*
      buf[1] = sinfo->segs[i].orig_hash;
      nr ++;
      */
    }
    corpus_push_back(m->indep_corpus, buf, nr, flags);
  }
}

static void
do_read_file(struct input_info *m, FILE *fp)
{
  char line[1024];
  struct sentence_info sinfo;

  init_sentence_info(&sinfo);

  while (fgets(line, 1024, fp)) {
    char *buf = line;
    int error_class = 0;
    if (!strncmp(buf, "eos", 3)) {
      m->nr_sentences ++;
      complete_sentence_info(m, &sinfo);
      init_sentence_info(&sinfo);
    }
    if (line[0] == '~' || line[0] == '!' ||
	line[0] == '^') {
      buf ++;
      error_class = 1;
    }
    if (!strncmp(buf, "indep_word", 10) ||
	!strncmp(buf, "eos", 3)) {
      parse_indep(m, &sinfo, line, buf, error_class);
    }
  }
}

static void
read_file(struct input_info *m, char *fn)
{
  FILE *ifp;
  ifp = fopen(fn, "r");
  if (!ifp) {
    return ;
  }
  do_read_file(m, ifp);
  fclose(ifp);
}

static void
write_nl(FILE *fp, int i)
{
  i = anthy_dic_htonl(i);
  fwrite(&i, sizeof(int), 1, fp);
}

static void
dump_line(FILE *ofp, struct input_line *il)
{
  int i;
  for (i = 0; i < FEATURE_SET_SIZE || i < il->nr_features; i++) {
    if (i) {
      fprintf(ofp, ", ");
    }
    if (i < il->nr_features) {
      fprintf(ofp, "%d", il->features[i]);
    } else {
      fprintf(ofp, "0");
    }
  }
  fprintf(ofp,",%d,%d\n", (int)il->negative_weight, (int)il->weight);
}

static int
compare_line(const void *p1, const void *p2)
{
  const struct input_line *const *il1 = p1;
  const struct input_line *const *il2 = p2;
  int i;
  for (i = 0; i < (*il1)->nr_features &&
	 i < (*il2)->nr_features; i++) {
    if ((*il1)->features[i] !=
	(*il2)->features[i]) {
      return (*il1)->features[i] - (*il2)->features[i];
    }
  }
  return (*il1)->nr_features - (*il2)->nr_features;
}

static void
dump_features(FILE *ofp, struct input_set *is)
{
  struct input_line *il, **lines;
  int i, nr = 0;
  int weight = 0;

  /* count lines */
  for (il = input_set_get_input_line(is); il; il = il->next_line) {
    nr ++;
    weight += (int)il->weight;
  }
  /* copy lines */
  lines = malloc(sizeof(struct input_line *) * nr);
  for (il = input_set_get_input_line(is), i = 0; i < nr;
       i++, il = il->next_line) {
    lines[i] = il;
  }
  /* sort */
  qsort(lines, nr, sizeof(struct input_line *), compare_line);
  /* output */
  fprintf(ofp, "%d %d total_line_weight,count\n", weight, nr);
  /**/
  for (i = 0; i < nr; i++) {
    dump_line(ofp, lines[i]);
  }
  free(lines);
}

static void
dump_input_info(FILE *ofp, struct input_info *m)
{
  fprintf(ofp, "section anthy.trans_info ");
  dump_features(ofp, m->seg_is);
  fprintf(ofp, "section anthy.cand_info ");
  dump_features(ofp, m->cand_is);
  fprintf(ofp, "section anthy.corpus_bucket ");
  corpus_write_bucket(ofp, m->indep_corpus);
  fprintf(ofp, "section anthy.corpus_array ");
  corpus_write_array(ofp, m->indep_corpus);
  /**/
  fprintf(ofp, "section anthy.feature_info ");
  input_set_output_feature_freq(ofp, m->seg_is);
}

static void
convert_line(FILE *ofp, char *buf)
{
  char *tok;
  tok = strtok(buf, ",");
  do {
    int n = atoi(tok);
    write_nl(ofp, n);
    tok = strtok(NULL, ",");
  } while (tok);
}

static void
convert_file(FILE       *ifp,
             const char *ifn,
             const char *output_dir)
{
  char buf[1024];
  FILE *ofp = NULL;
  char *fn = NULL;
  while (fgets(buf, 1024, ifp)) {
    /**/
    if (buf[0] == '#') {
      continue;
    }
    if (!strncmp("section", buf, 7)) {
      int w, n, i;
      char file_name[1024];
      if (ofp) {
	fclose(ofp);
	ofp = NULL;
      }
      if (fn) {
        free(fn);
        fn = NULL;
      }
      sscanf(buf, "section %s %d %d", file_name, &w, &n);
      fn = malloc(strlen(output_dir) + strlen(file_name) + 4);
      if (!fn) {
        fprintf(stderr, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
        abort();
      }
      sprintf(fn, "%s/%s", output_dir, file_name);
      ofp = fopen(fn, "w");
      if (!ofp) {
	fprintf(stderr, "failed to open (%s)\n", fn);
	abort();
      }
      write_nl(ofp, w);
      write_nl(ofp, n);
      for (i = 0; i < NR_EM_FEATURES; i++) {
	write_nl(ofp, 0);
      }
    } else {
      if (!ofp) {
        anthy_log(0, "section could not found in %s\n", ifn);
        continue;
      }
      convert_line(ofp, buf);
    }
  }
  if (ofp) {
    fclose(ofp);
  }
  if (fn) {
    free(fn);
  }
}

static void
convert_data(int nr_fn, char **fns, const char *output_dir)
{
  FILE *ifp;
  int i;
  /**/
  for (i = 0; i < nr_fn; i++) {
    ifp = fopen(fns[i], "r");
    if (!ifp) {
      fprintf(stderr, "failed to open (%s)\n", fns[i]);
      continue;
    }
    convert_file(ifp, fns[i], output_dir);
    fclose(ifp);
  }
}

/**/
#define STRING_HASH_SIZE 256
struct string_node {
  int key;
  char *str;
  struct string_node *next_hash;
};
struct string_pool {
  int nr;
  struct string_node hash[STRING_HASH_SIZE];
  struct string_node **array;
};
struct resize_info {
  char *indep;
  int valid;
};
struct extract_stat {
  int nr;
  struct resize_info info[MAX_SEGMENT];
};

static void
string_pool_init(struct string_pool *sp)
{
  int i;
  for (i = 0; i < STRING_HASH_SIZE; i++) {
    sp->hash[i].next_hash = NULL;
  }
  sp->nr = 0;
}

static int
compare_string_node(const void *p1, const void *p2)
{
  const struct string_node *const *n1 = p1;
  const struct string_node *const *n2 = p2;
  return (*n1)->key -(*n2)->key;
}

static void
string_pool_sort(struct string_pool *sp)
{
  int idx, h;
  if (!(sp->array = malloc(sizeof(struct string_node *) * sp->nr))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return;
  }
  for (idx = 0, h = 0; h < STRING_HASH_SIZE; h++) {
    struct string_node *node;
    for (node = sp->hash[h].next_hash; node; node = node->next_hash) {
      sp->array[idx] = node;
      idx ++;
    }
  }
  /**/
  qsort(sp->array, sp->nr, sizeof(struct string_node *), compare_string_node);
}

static void
string_pool_dump(FILE *ofp, struct string_pool *sp)
{
  int i;
  fprintf(ofp, "section anthy.weak_words 0 %d\n", sp->nr);
  for (i = 0; i < sp->nr; i++) {
    fprintf(ofp, "%d\n", sp->array[i]->key);
  }
}

static unsigned int
string_hash(const unsigned char *str)
{
  unsigned int h = 0;
  assert(str);
  while (*str) {
    h += *str;
    h *= 13;
    str ++;
  }
  return h % STRING_HASH_SIZE;
}

static struct string_node *
find_string_node(struct string_pool *sp, const char *str)
{
  int h = (int)string_hash((const unsigned char *)str);
  struct string_node *node;
  for (node = sp->hash[h].next_hash; node; node = node->next_hash) {
    if (!strcmp(str, node->str)) {
      return node;
    }
  }
  /* allocate new */
  node = malloc(sizeof(*node));
  node->str = strdup(str);
  node->key = 0;
  node->next_hash = sp->hash[h].next_hash;
  sp->hash[h].next_hash = node;
  sp->nr ++;
  return node;
}

static void
flush_extract_stat(struct extract_stat *es, struct string_pool *sp)
{
  int i;
  assert(es);
  if (es->nr > 0)
    assert(es->info);
  for (i = 0; i < es->nr; i++) {
    if (es->info[i].valid) {
      struct string_node *node;
      node = find_string_node(sp, es->info[i].indep);
      if (!node) {
        anthy_log(0, "Failed malloc at %d/%d in %s:%d\n",
                  i, es->nr, __FILE__, __LINE__);
        free(es->info[i].indep);
        es->info[i].indep = NULL;
        continue;
      }
      if (node->key == 0) {
	xstr *xs = anthy_cstr_to_xstr(node->str, ANTHY_EUC_JP_ENCODING);
	node->key = anthy_xstr_hash(xs);
	anthy_free_xstr(xs);
      }
      /* printf("(%s)%d\n", es->info[i].indep, node->key); */
    }
    free(es->info[i].indep);
    es->info[i].indep = NULL;
  }
  es->nr = 0;
}

static char *
get_indep_part(char *buf)
{
  int len;
  char *c = strchr(buf, '#');
  if (!c) {
    return NULL;
  }
  c = strchr(c, ' ');
  if (!c) {
    return NULL;
  }
  c++;
  c = strchr(c, ' ');
  if (!c) {
    return NULL;
  }
  c++;
  len = strlen(c);
  c[len-1] = 0;
  return c;
}

static void
fixup_missed_word(struct extract_stat *es, char *buf)
{
  int i;
  char *c = get_indep_part(buf);
  if (!c) {
    return ;
  }
  for (i = 0; i < es->nr; i++) {
    if (!strcmp(es->info[i].indep, c)) {
      es->info[i].valid = 0;
    }
  }
}

static void
fill_missed_word(struct extract_stat *es, char *buf)
{
  char *c = get_indep_part(buf);
  assert(es);
  assert(es->info);
  if (!c) {
    return ;
  }
  es->info[es->nr].indep = strdup(c);
  es->info[es->nr].valid = 1;
  es->nr++;
}

static void
extract_word_from_file(FILE *ifp, struct string_pool *sp)
{
  int i;
  char buf[1024];
  struct extract_stat es;
  /**/
  es.nr = 0;
  for (i = 0; i < MAX_SEGMENT; i++) {
    es.info[i].indep = NULL;
  }
  /**/
  while (fgets(buf, 1024, ifp)) {
    if (buf[0] == '#') {
      continue;
    }
    if (buf[0] == '\n' ||
	buf[0] == ' ') {
      flush_extract_stat(&es, sp);
      continue;
    }
    /**/
    if (!strncmp("!indep_word ", buf, 12)) {
      fill_missed_word(&es, buf);
    }
    if (!strncmp("indep_word", buf, 10)) {
      fixup_missed_word(&es, buf);
    }
  }
  flush_extract_stat(&es, sp);
}

static void
extract_word(int nr_fn, char **fns, FILE *ofp)
{
  struct string_pool sp;
  FILE *ifp;
  int i;
  /**/
  string_pool_init(&sp);
  /**/
  for (i = 0; i < nr_fn; i++) {
    ifp = fopen(fns[i], "r");
    if (!ifp) {
      fprintf(stderr, "failed to open (%s)\n", fns[i]);
      continue;
    }
    extract_word_from_file(ifp, &sp);
    fclose(ifp);
  }
  /**/
  string_pool_sort(&sp);
  string_pool_dump(ofp, &sp);
}

/* 変換結果から確率のテーブルを作る */
static void
proc_corpus(int nr_fn, char **fns, FILE *ofp)
{
  int i;
  struct input_info *m;
  /**/
  m = init_input_info();
  /**/
  for (i = 0; i < nr_fn; i++) {
    m->nth_input_file = i;
    read_file(m, fns[i]);
  }

  corpus_build(m->indep_corpus);
  /**/
  dump_input_info(ofp, m);
  /**/
  fprintf(stderr, " %d sentences\n", m->nr_sentences);
  fprintf(stderr, " %d connections\n", m->nr_connections);
  fprintf(stderr, " %d segments\n", m->nr_connections - m->nr_sentences);
  input_set_free(m->seg_is);
  input_set_free(m->cand_is);
  corpus_free(m->indep_corpus);
  free(m);
}

int
main(int argc, char **argv)
{
  FILE *ofp;
  int i;
  int nr_input = 0;
  char **input_files;
  const char *output_dir = ".";
  int convert = 0;
  int extract = 0;

  ofp = NULL;
  if (!(input_files = malloc(sizeof(char *) * argc))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return 1;
  }
  
  for (i = 1; i < argc; i++) {
    char *arg = argv[i];
    if (!strcmp(arg, "-o")) {
      ofp = fopen(argv[i+1], "w");
      if (!ofp) {
	fprintf(stderr, "failed to open (%s)\n", argv[i+1]);
      }
      i ++;
    } else if (!strcmp(arg, "-d")) {
      output_dir = argv[i+1];
      i ++;
    } else if (!strcmp(arg, "-c") ||
	       !strcmp(arg, "--convert")) {
      convert = 1;
    } else if (!strcmp(arg, "-e") ||
	       !strcmp(arg, "--extract")) {
      extract = 1;
    } else {
      input_files[nr_input] = arg;
      nr_input++;
    }
  }
  if (extract) {
    printf(" -- extracting missed words\n");
    if (!ofp)
      ofp = stdout;
    extract_word(nr_input, input_files, ofp);
    free(input_files);
    if (ofp != stdout)
      fclose(ofp);
    return 0;
  }
  if (ofp) {
    printf(" -- generating dictionary in text form\n");
    proc_corpus(nr_input, input_files, ofp);
    fclose(ofp);
  }
  if (convert) {
    printf(" -- converting dictionary from text to binary form\n");
    convert_data(nr_input, input_files, output_dir);
  }
  free(input_files);

  return 0;
}
