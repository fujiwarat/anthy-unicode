/*
 * Copyright (C) 2000-2007 TABATA Yusuke
 * Copyright (C) 2004-2006 YOSHIDA Yuichi
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
 */
/*
 * 付属語グラフをバイナリ化する
 * init_word_seq_tab()
 *   付属語テーブル中のノードへのポインタの初期化
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

#include <anthy/alloc.h>
#include <anthy/conf.h>
#include <anthy/ruleparser.h>
#include <anthy/xstr.h>
#include <anthy/logger.h>
#include <anthy/splitter.h>
#include <anthy/anthy.h>
#include <anthy/depgraph.h>
#include <anthy/diclib.h>

#ifndef SRCDIR
#define SRCDIR "."
#endif

static int verbose;

static struct dep_node* gNodes;
static char** gNodeNames;
static int nrNodes;

/* 単語接続ルール */
static struct wordseq_rule *gRules;
static int nrRules;

static int 
get_node_id_by_name(const char *name)
{
  struct dep_node *tmp = NULL;
  char **tmp2 = NULL;
  int i;
  if (nrNodes > 0)
    assert(gNodeNames);
  /* 登録済みのものから探す */
  for (i = 0; i < nrNodes; i++) {
    if (!strcmp(name,gNodeNames[i])) {
      return i;
    }
  }
  /* なかったので作る */
  if (!(tmp = realloc(gNodes, sizeof(struct dep_node)*(nrNodes+1)))) {
    anthy_log(0, "Could not realloc gNodes.\n");
    free(gNodes);
    gNodes = NULL;
    nrNodes = 0;
    return 0;
  }
  gNodes = tmp;
  if (!(tmp2 = realloc(gNodeNames, sizeof(char*)*(nrNodes+1)))) {
    anthy_log(0, "Could not realloc gNodeNames.\n");
    free(gNodes);
    gNodes = NULL;
    free(gNodeNames);
    gNodeNames = NULL;
    nrNodes = 0;
    return 0;
  }
  gNodeNames = tmp2;
  gNodes[nrNodes].nr_branch = 0;
  gNodes[nrNodes].branch = 0;
  gNodeNames[nrNodes] = strdup(name);
  nrNodes++;
  return nrNodes-1;
}


/* 遷移条件からbranchを捜し出す */
static struct dep_branch *
find_branch(struct dep_node *node, xstr **strs, int nr_strs)
{
  struct dep_branch *db;
  int i, j;
  assert(node);
  /* 同じ遷移条件のブランチを探す */
  for (i = 0; i < node->nr_branch; i++) {
    db = &node->branch[i];
    if (nr_strs != db->nr_strs) {
      continue ;
    }
    for (j = 0; j < nr_strs; j++) {
      if (anthy_xstrcmp(db->str[j], strs[j])) {
	goto fail;
      }
    }
    /**/
    return db;
  fail:;
  }
  /* 新しいブランチを確保する */
  node->branch = realloc(node->branch,
			 sizeof(struct dep_branch)*(node->nr_branch+1));
  db = &node->branch[node->nr_branch];
  node->nr_branch++;
  db->str = malloc(sizeof(xstr*)*nr_strs);
  for (i = 0; i < nr_strs; i++) {
    db->str[i] = strs[i];
  }
  db->nr_strs = nr_strs;
  db->nr_transitions = 0;
  db->transition = 0;
  return db;
}

/*
 * 遷移をparseする
 *  doc/SPLITTER参照
 */
static void
parse_transition(char *token, struct dep_transition *tr)
{
  int ct = CT_NONE;
  int pos = POS_NONE;
  enum dep_class dc = DEP_NONE;
  char *str = token;
  tr->head_pos = POS_NONE;
  tr->weak = 0;
  /* 遷移の属性を解析*/
  while (*token != '@') {
    switch(*token){
    case ':':
    case '.':
      tr->weak = 1;
      break;
    case 'C':
      /* 活用形 */
      switch (token[1]) {
      case 'z': ct = CT_MIZEN; break;
      case 'y': ct = CT_RENYOU; break;
      case 's': ct = CT_SYUSI; break;
      case 't': ct = CT_RENTAI; break;
      case 'k': ct = CT_KATEI; break;
      case 'm': ct = CT_MEIREI; break;
      case 'g': ct = CT_HEAD; break;
      }
      token ++;
      break;
    case 'H':
      /* 自立語部の品詞 */
      switch (token[1]) {
      case 'n':	tr->head_pos = POS_NOUN; break;
      case 'v':	tr->head_pos = POS_V; break;
      case 'j':	tr->head_pos = POS_AJV; break;
      }
      token ++;
      break;
    case 'S':
      /* 文節の属性 */
      switch (token[1]) {
	/*      case 'n': sc = DEP_NO; break;*/
      case 'f': dc = DEP_FUZOKUGO; break;
      case 'k': dc = DEP_KAKUJOSHI; break;
      case 'y': dc = DEP_RENYOU; break;
      case 't': dc = DEP_RENTAI; break;
      case 'e': dc = DEP_END; break;
      case 'r': dc = DEP_RAW; break;
      default: printf("unknown (S%c)\n", token[1]);
      }
      token ++;
      break;
    default:
      printf("Unknown (%c) %s\n", *token, str);
      break;
    }
    token ++;
  }
  /* @から後はノードの名前 */
  tr->next_node = get_node_id_by_name(token);
  /**/
  tr->pos = pos;
  tr->ct = ct;
  tr->dc = dc;
}

/*
 * ノード名 遷移条件+ 遷移先+
 */
static void
parse_dep(char **tokens, int nr)
{
  int id, row = 0;
  struct dep_branch *db;
  struct dep_node *dn;
  int nr_strs;

  assert(gNodes);
  xstr **strs = alloca(sizeof(xstr*) * nr);
  /* ノードとそのidを確保 */
  id = get_node_id_by_name(tokens[row]);
  assert(id < nrNodes);
  dn = &gNodes[id];
  row++;
  assert(dn);

  nr_strs = 0;

  /* 遷移条件の付属語の配列を作る */
  for (; row < nr && tokens[row][0] == '\"'; row++) {
    char *s;
    s = strdup(&tokens[row][1]);
    s[strlen(s)-1] =0;
    strs[nr_strs] = anthy_cstr_to_xstr(s, ANTHY_UTF8_ENCODING);
    nr_strs ++;
    free(s);
  }

  /* 遷移条件がない時は警告を出して、空の遷移条件を追加する */
  if (nr_strs == 0) {
    char *s;
    anthy_log(0, "node %s has a branch without any transition condition.\n",
	      tokens[0]);
    s = strdup("");
    strs[0] = anthy_cstr_to_xstr(s, ANTHY_UTF8_ENCODING);
    nr_strs = 1;
    free(s);
  }

  /* ブランチに遷移先のノードを追加する */
  db = find_branch(dn, strs, nr_strs);
  for ( ; row < nr; row++){
    struct dep_transition *tr;
    db->transition = realloc(db->transition,
			     sizeof(struct dep_transition)*
			     (db->nr_transitions+1));
    tr = &db->transition[db->nr_transitions];
    parse_transition(tokens[row], tr);
    db->nr_transitions ++;
  }
}

/* 文法定義ファイル中に空のノードがあるかチェックする */
static void
check_nodes(void)
{
  int i;
  if (nrNodes > 0) {
    assert(gNodes);
    assert(gNodeNames);
  }
  for (i = 1; i < nrNodes; i++) {
    if (gNodes[i].nr_branch == 0) {
      anthy_log(0, "node %s has no branch.\n",
                gNodeNames ? gNodeNames[i] : "(null)");
    }
  }
}


static int
init_depword_tab(void)
{
  const char *fn;
  char **tokens;
  int nr;

  /* id 0 を空ノードに割当てる */
  get_node_id_by_name("@");

  /**/
  fn = anthy_conf_get_str("DEPWORD");
  if (!fn) {
    anthy_log(0, "Dependent word dictionary is unspecified.\n");
    return -1;
  }
  if (anthy_open_file(fn) == -1) {
    anthy_log(0, "Failed to open dep word dict (%s).\n", fn);
    return -1;
  }
  /* 一行ずつ付属語グラフを読む */
  while (!anthy_read_line(&tokens, &nr)) {
    parse_dep(tokens, nr);
    anthy_free_line();
  }
  anthy_close_file();
  check_nodes();
  return 0;
}


static void
parse_indep(char **tokens, int nr)
{
  struct wordseq_rule *tmp = NULL;
  if (nr < 2) {
    printf("Syntex error in indepword defs"
	   " :%d.\n", anthy_get_line_number());
    return ;
  }
  if (!(tmp = realloc(gRules, sizeof(struct wordseq_rule)*(nrRules+1)))) {
    anthy_log(0, "Could not realloc gRules.\n");
    /* CPPCHECK_WARNING and CLANG_WARNING are conflicted.
     * CPPCHECK_WARNING reports: Common realloc mistake:
     *     'gRules' nulled but not freed upon failure
     * also CLANG_WARNING reports: Potential leak of memory ponted to by
     *     'gRules'
     * On the other hand,
     * CLANG_WARNING reports: 'gRules' is freed twice.
     */
    free(gRules);
    gRules = NULL;
    nrRules = 0;
    return;
  }
  gRules= tmp;

  /* 行の先頭には品詞の名前が入っている */
  gRules[nrRules].wt = anthy_init_wtype_by_name(tokens[0]);

  /* その次にはノード名が入っている */
  gRules[nrRules].node_id = get_node_id_by_name(tokens[1]);

  if (verbose) {
    printf("%d (%s)\n", nrRules, tokens[0]);
  }

  nrRules++;
}

/** 自立語からの遷移表 */
static int 
init_indep_word_seq_tab(void)
{
  const char *fn;
  char **tokens;
  int nr;

  fn = anthy_conf_get_str("INDEPWORD");
  if (!fn){
    printf("independent word dict unspecified.\n");
    return -1;
  }
  if (anthy_open_file(fn) == -1) {
    printf("Failed to open indep word dict (%s).\n", fn);
    return -1;
  }
  /* ファイルを一行ずつ読む */
  while (!anthy_read_line(&tokens, &nr)) {
    parse_indep(tokens, nr);
    anthy_free_line();
  }
  anthy_close_file();

  return 0;
}

/*  
    ネットワークバイトオーダーで4byte書き出す
*/
static void
write_nl(FILE* fp, int i)
{
  i = anthy_dic_htonl(i);
  fwrite(&i, sizeof(int), 1, fp);
}

static void
write_transition(FILE* fp, struct dep_transition* transition)
{
  write_nl(fp, transition->next_node); 
  write_nl(fp, transition->pos); 
  write_nl(fp, transition->ct); 
  write_nl(fp, transition->dc); 
  write_nl(fp, transition->head_pos); 
  write_nl(fp, transition->weak); 
}

static void
write_xstr(FILE* fp, xstr* str)
{
  int i;
  xchar c;
  write_nl(fp, str->len);

  for (i = 0; i < str->len; i++) {
    c = anthy_dic_htonl(str->str[i]);
    fwrite(&c, sizeof(xchar), 1, fp);
  }
}

static void
write_branch(FILE* fp, struct dep_branch* branch)
{
  int i;

  write_nl(fp, branch->nr_strs);
  for (i = 0; i < branch->nr_strs; ++i) {
    write_xstr(fp, branch->str[i]);
  }

  write_nl(fp, branch->nr_transitions);
  for (i = 0; i < branch->nr_transitions; ++i) {
    write_transition(fp, &branch->transition[i]);
  }
}

static void
write_node(FILE* fp, struct dep_node* node)
{
  int i;
  write_nl(fp, node->nr_branch);
  for (i = 0; i < node->nr_branch; ++i) {
    write_branch(fp, &node->branch[i]);
  }
}

static void
write_wtype(FILE *fp, wtype_t wt)
{
  fputc(anthy_wtype_get_pos(wt), fp);
  fputc(anthy_wtype_get_cos(wt), fp);
  fputc(anthy_wtype_get_scos(wt), fp);
  fputc(anthy_wtype_get_cc(wt), fp);
  fputc(anthy_wtype_get_ct(wt), fp);
  fputc(anthy_wtype_get_wf(wt), fp);
  fputc(0, fp);
  fputc(0, fp);
}

static void
write_file(const char* file_name)
{
  int i;
  FILE* fp = fopen(file_name, "w");
  int* node_offset = malloc(sizeof(int) * nrNodes); /* gNodesのファイル上の位置 */

  if (!fp) {
    anthy_log(0, "Could not write-open %s.\n", file_name);
    free(node_offset);
    return;
  }
  /* 各ルール */
  write_nl(fp, nrRules);
  for (i = 0; i < nrRules; ++i) {
    write_wtype(fp, gRules[i].wt);
    write_nl(fp, gRules[i].node_id);
  }

  write_nl(fp, nrNodes);

  if (nrNodes > 0)
    assert(gNodes);
  for (i = 0; i < nrNodes; ++i) {
    write_node(fp, &gNodes[i]);
  }

  free(node_offset);
  fclose(fp);
}

int 
main(int argc, char **argv)
{
  int i;
  const char *out_fn = "anthy.dep";
  /* 付属語辞書を読み込んでファイルに書き出す */
  anthy_conf_override("CONFFILE", "../anthy-unicode.conf");
  anthy_conf_override("ANTHYDIR", SRCDIR "/../depgraph/");

  anthy_init_wtypes();
  anthy_do_conf_init();
  /* 付属語グラフ */
  init_depword_tab();
  /* 自立語からの遷移表 */
  init_indep_word_seq_tab();

  for (i = 1; i < argc; i++) {
    char *arg = argv[i];
    char *prev_arg = argv[i-1];
    if (!strcmp(prev_arg, "-o")) {
      out_fn = arg;
    }
  }

  write_file(out_fn);

  return 0;
}
