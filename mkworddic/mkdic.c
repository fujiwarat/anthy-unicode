/*
 * cannadic形式のファイルから辞書ファイルを作る
 *
 * Funded by IPA未踏ソフトウェア創造事業 2002 1/1
 *
 * Copyright (C) 2000-2007 TABATA Yusuke
 * Copyright (C) 2005 YOSHIDA Yuichi
 * Copyright (C) 2001-2002 TAKAI Kousuke
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
 */
/*
 * 辞書は読みをindexとし、品詞や変換後の単語(=entry)を検索
 * する構造になっている。
 *
 * 読み -> 単語、単語、、
 *
 * 辞書ファイルはネットワークバイトオーダーを用いる。
 *
 * 辞書ファイルは複数のセクションから構成されている
 *  0 ヘッダ 16*4 bytes
 *  2 読みのインデックス (読み512個ごと)
 *  3 読み
 *  4 ページ
 *  5 ページのインデックス
 *  6 用例辞書(?)
 *  7 読み hash
 *
 * source 元の辞書ファイル
 * file_dic 生成するファイル
 *
 * yomi_hash 辞書ファイルに出力されるhashのbitmap
 * index_hash このソース中でstruct yomi_entryを検索するためのhash
 *
 */

#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <config.h>

#include <anthy/anthy.h>
#include <anthy/diclib.h>
#include <anthy/logger.h>
#include <anthy/ruleparser.h>
#include <anthy/word_dic.h>
#include <anthy/wtype.h>
#include <anthy/xstr.h>
#include "mkdic.h"

#define MAX_LINE_LEN 10240
#define NR_HEADER_SECTIONS 16
#define SECTION_ALIGNMENT 8
#define MAX_WTYPE_LEN 20

#define DEFAULT_FN "anthy.wdic"

static const char *progname;

/* writewords.cからアクセスするために、global変数 */
FILE *yomi_entry_index_out, *yomi_entry_out;
FILE *page_out, *page_index_out;
/**/
static FILE *uc_out;
static FILE *yomi_hash_out;
/* ハッシュの衝突の数、統計情報 */
static int yomi_hash_collision;

/* ファイル中の順序に従って並べる */
struct file_section {
  FILE **fpp;
  char *fn;
} file_array[] = {
  {&yomi_entry_index_out, NULL},
  {&yomi_entry_out, NULL},
  {&page_out, NULL},
  {&page_index_out, NULL},
  {&uc_out, NULL},
  {&yomi_hash_out, NULL},
  {NULL, NULL},
};

/* 辞書生成の状態 */
struct mkdic_stat {
  /* 単語のリスト */
  struct yomi_entry_list yl;
  /**/
  struct adjust_command ac_list;
  /* 用例辞書 */
  struct uc_dict *ud;
  /**/
  const char *output_fn;
  /**/
  int input_encoding;
  /**/
  int nr_excluded;
  char **excluded_wtypes;
};

/* 辞書の出力先のファイルをオープンする */
static void
open_output_files(void)
{
  struct file_section *fs;
  char *tmpdir = getenv("TMPDIR");
  tmpdir = tmpdir ? strdup (tmpdir) : strdup ("/tmp");
  for (fs = file_array; fs->fpp; fs ++) {
    char buf[256];
    int fd = -1;
    /* tmpfile()がTMPDIRを見ないため、TMPDIRを指定された場合mkstempを使う。
     * tmpfile() creates files with predictable names, which is unsafe and
     * is not recommended.
     */
    fs->fn = NULL;
    snprintf(buf, sizeof(buf), "%s/mkanthydic.XXXXXX", tmpdir);
    fd = mkstemp(buf);
    if (fd == -1) {
      *(fs->fpp) = NULL;
    } else {
      *(fs->fpp) = fdopen(fd, "w+");
      fs->fn = strdup(buf);
    }
    /**/
    if (!(*(fs->fpp))) {
      fprintf (stderr, "%s: cannot open temporary file: %s\n",
               progname, strerror (errno));
      exit (2);
    }
  }
  free(tmpdir);
}

/* fflushする */
static void
flush_output_files (void)
{
  struct file_section *fs;
  for (fs = file_array; fs->fpp; fs ++) {
    if (ferror(*(fs->fpp))) {
      fprintf (stderr, "%s: write error\n", progname);
      exit (1);
    }
  }
  for (fs = file_array; fs->fpp; fs ++) {
    if (fflush(*(fs->fpp))) {
      fprintf (stderr, "%s: write error: %s\n", progname, strerror (errno));
      exit (1);
    }
  }
}

/* ネットワークbyteorderで4bytes書き出す */
void
write_nl(FILE *fp, int i)
{
  i = anthy_dic_htonl(i);
  fwrite(&i, sizeof(int), 1, fp);
}

static void
print_usage(void)
{
  printf("please do not use mkanthydic command directly.\n");
  exit(0);
}

static char *
read_line(FILE *fp, char *buf)
{
  /* 長すぎる行を無視する */
  int toolong = 0;

  while (fgets(buf, MAX_LINE_LEN, fp)) {
    int len = strlen(buf);
    if (buf[0] == '#') {
      continue ;
    }
    if (buf[len - 1] != '\n') {
      toolong = 1;
      continue ;
    }

    buf[len - 1] = 0;
    if (toolong) {
      toolong = 0;
    } else {
      return buf;
    }
  }
  return NULL;
}

/** cannadic形式の辞書の行からindexとなる部分を取り出す */
static xstr *
get_index_from_line(struct mkdic_stat *mds, char *buf)
{
  char *sp;
  xstr *xs;
  sp = strchr(buf, ' ');
  if (!sp) {
    /* 辞書のフォーマットがおかしい */
    return NULL;
  }
  *sp = 0;
  xs = anthy_cstr_to_xstr(buf, mds->input_encoding);
  *sp = ' ';
  return xs;
}

/** cannadic形式の辞書の行からindex以外の部分を取り出す */
static char *
get_entry_from_line(char *buf)
{
  char *sp;
  sp = strchr(buf, ' ');
  assert(sp);
  while(*sp == ' ') {
    sp ++;
  }
  return strdup(sp);
}

static int
index_hash(xstr *xs)
{
  int i;
  unsigned int h = 0;
  for (i = 0; i < xs->len; i++) {
    h += xs->str[i] * 11;
  }
  return (int)(h % YOMI_HASH);
}

const char *
get_wt_name(const char *name)
{
  wtype_t dummy;
  const char *res;
  if (!strcmp(name, "#T35")) {
    return "#T";
  }
  res = anthy_type_to_wtype(name, &dummy);
  if (!res) {
    return "unknown";
  }
  return res;
}

/** 読みに対して、単語を一つを追加する */
static void
push_back_word_entry(struct mkdic_stat *mds,
		     struct yomi_entry *ye, const char *wt_name,
		     int freq, const char *word, int order)
{
  wtype_t wt;
  char *s;
  if (freq == 0) {
    return ;
  }
  if (!anthy_type_to_wtype(wt_name, &wt)) {
    /* anthyの知らない品詞 */
    return ;
  }
  ye->entries = realloc(ye->entries,
			sizeof(struct word_entry) *
			(ye->nr_entries + 1));
  ye->entries[ye->nr_entries].ye = ye;
  ye->entries[ye->nr_entries].wt_name = get_wt_name(wt_name);
  ye->entries[ye->nr_entries].raw_freq = freq;
  ye->entries[ye->nr_entries].feature = 0;
  ye->entries[ye->nr_entries].source_order = order;
  if (mds->input_encoding == ANTHY_EUC_JP_ENCODING) {
    s = anthy_conv_euc_to_utf8(word);
  } else {
    s = strdup(word);
  }
  ye->entries[ye->nr_entries].word_utf8 = s;
  ye->nr_entries++;
}

static int
parse_wtype(char *wtbuf, char *cur)
{
  /* 品詞 */
  char *t;
  int freq;
  if (strlen(cur) >= MAX_WTYPE_LEN) {
    return 0;
  }
  strcpy(wtbuf, cur);
  /* 頻度 */
  t = strchr(wtbuf, '*');
  freq = 1;
  if (t) {
    int tmp_freq;
    *t = 0;
    t++;
    tmp_freq = atoi(t);
    if (tmp_freq) {
      freq = tmp_freq;
    }
  }
  return freq;
}

/* 複合語の要素の長さは 1,2,3, ... 9,a,b,c */
static int
get_element_len(xchar xc)
{
  if (xc > '0' && xc <= '9') {
    return xc - '0';
  }
  if (xc >= 'a' && xc <= 'z') {
    return xc - 'a' + 10;
  }
  return 0;
}

/** 複合候補の形式チェック */
static int
check_compound_candidate(struct mkdic_stat *mds, xstr *index, const char *cur)
{
  /* 読みの文字数の合計を数える */
  xstr *xs = anthy_cstr_to_xstr(cur, mds->input_encoding);
  int i, total = 0;
  for (i = 0; i < xs->len - 1; i++) {
    if (xs->str[i] == '_') {
      total += get_element_len(xs->str[i+1]);
    }
  }
  anthy_free_xstr(xs);
  /* 比較する */
  if (total != index->len) {
    fprintf(stderr, "Invalid compound candidate (%s, length = %d).\n",
	    cur, total);
    return 0;
  }
  return 1;
}

static int
is_excluded_wtype(struct mkdic_stat *mds, char *wt)
{
  int i;
  for (i = 0; i < mds->nr_excluded; i++) {
    if (!strcmp(mds->excluded_wtypes[i], wt)) {
      return 1;
    }
  }
  return 0;
}

static char *
find_token_end(char *cur)
{
  char *n;
  for (n = cur; *n != ' ' && *n != '\0'; n++) {
    if (*n == '\\') {
      if (!n[1]) {
	return NULL;
      }
      n++;
    }
  }
  return n;
}

/** 読みに対応する行を分割して、配列を構成する */
static void
push_back_word_entry_line(struct mkdic_stat *mds, struct yomi_entry *ye,
			  const char *ent)
{
  char *buf = alloca(strlen(ent) + 1);
  char *cur = buf;
  char *n;
  char wtbuf[MAX_WTYPE_LEN];
  int freq = 0;
  int order = 0;

  strcpy(buf, ent);
  wtbuf[0] = 0;

  while (1) {
    /* トークンを\0で切る。curの後の空白か\0を探す */
    n = find_token_end(cur);
    if (!n) {
      fprintf(stderr, "invalid \\ at the end of line (%s).\n",
	      ent);
      return ;
    }
    if (*n) {
      *n = 0;
    } else {
      n = NULL;
    }
    /**/
    if (cur[0] == '#') {
      if (isalpha((unsigned char)cur[1])) {
	/* #XX*?? をパース */
	freq = parse_wtype(wtbuf, cur);
      } else {
	if (cur[1] == '_' &&
	    check_compound_candidate(mds, ye->index_xstr, &cur[1])) {
	  /* #_ 複合候補 */
	  push_back_word_entry(mds, ye, wtbuf, freq, cur, order);
	  order ++;
	}
      }
    } else {
      /* 品詞が除去リストに入っているかをチェック */
      if (!is_excluded_wtype(mds, wtbuf)) {
	/* 単語を追加 */
	push_back_word_entry(mds, ye, wtbuf, freq, cur, order);
	order ++;
      }/* :to extract excluded words
	  else {
	  anthy_putxstr(ye->index_xstr);
	  printf(" %s*%d %s\n", wtbuf, freq, cur);
	  }*/
    }
    if (!n) {
      /* 行末 */
      return ;
    }
    cur = n;
    cur ++;
  }
}

/** 同じ単語が無いかチェック */
static int
check_same_word(struct yomi_entry *ye, int idx)
{
  struct word_entry *base = &ye->entries[idx];
  int i = idx - 1;
  if (i >= 0) {
    struct word_entry *cur = &ye->entries[i];
    if (base->raw_freq != cur->raw_freq) {
      return 0;
    }
    if (strcmp(base->wt_name, cur->wt_name)) {
      return 0;
    }
    if (strcmp(base->word_utf8, cur->word_utf8)) {
      return 0;
    }
    /* 同じだった */
    return 1;
  }
  return 0;
}

/** qsort用の比較関数 */
static int
compare_word_entry_by_freq(const void *p1, const void *p2)
{
  const struct word_entry *e1 = p1;
  const struct word_entry *e2 = p2;
  return e2->raw_freq - e1->raw_freq;
}

/** qsort用の比較関数 */
static int
compare_word_entry_by_wtype(const void *p1, const void *p2)
{
  const struct word_entry *e1 = p1;
  const struct word_entry *e2 = p2;
  int ret = strcmp(e1->wt_name, e2->wt_name);
  if (ret != 0) {
    return ret;
  } else {
    return compare_word_entry_by_freq(p1, p2);
  }
}

/** 読みに対する単語を頻度順に並べ、いらない単語を消す */
static int
normalize_word_entry(struct yomi_entry *ye)
{
  int i, nr_dup = 0;
  if (!ye) {
    return 0;
  }
  /* 単語を並べる */
  qsort(ye->entries, ye->nr_entries,
	sizeof(struct word_entry),
	compare_word_entry_by_freq);
  /* ダブったら、0点 */
  for (i = 0; i < ye->nr_entries; i++) {
    if (check_same_word(ye, i)) {
      ye->entries[i].raw_freq = 0;
      nr_dup ++;
    }
  }
  /* 再びソート */
  qsort(ye->entries, ye->nr_entries,
	sizeof(struct word_entry),
	compare_word_entry_by_wtype);
  return ye->nr_entries - nr_dup;
}

/*その読みに対応するyomi_entryを返す
**/
struct yomi_entry *
find_yomi_entry(struct yomi_entry_list *yl, xstr *index, int create)
{
  struct yomi_entry *ye;
  int hash = index_hash(index);
  int search = 0;
  /* hash chainから探す */
  for (ye = yl->hash[hash];ye ; ye = ye->hash_next) {
    search ++;
    if (!anthy_xstrcmp(ye->index_xstr, index)) {
      return ye;
    }
  }
  if (!create) {
    return NULL;
  }

  /* 無いので確保 */
  ye = malloc(sizeof(struct yomi_entry));
  ye->nr_entries = 0;
  ye->entries = 0;
  ye->next = NULL;
  ye->index_xstr = anthy_xstr_dup(index);
  ye->index_str = NULL;

  /* hash chainにつなぐ */
  ye->hash_next = yl->hash[hash];
  yl->hash[hash] = ye;

  /* リストにつなぐ */

  ye->next = yl->head;
  yl->head = ye;

  yl->nr_entries++;

  return ye;
}

/* 辞書ファイル中のhash bitmapにマークを付ける */
static void
mark_hash_array(unsigned char *hash_array, xstr *xs)
{
  int val, idx, bit, mask;
  val = anthy_xstr_hash(xs);
  val &= (YOMI_HASH_ARRAY_SIZE*YOMI_HASH_ARRAY_BITS-1);
  idx=(val>>YOMI_HASH_ARRAY_SHIFT)&(YOMI_HASH_ARRAY_SIZE-1);
  bit= val & ((1<<YOMI_HASH_ARRAY_SHIFT)-1);
  mask = (1<<bit);
  if (hash_array[idx] & mask) {
    yomi_hash_collision ++;
  }
  hash_array[idx] |= mask;
}

/* 読みhashのビットマップを作る */
static void
mk_yomi_hash(FILE *yomi_hash_out, struct yomi_entry_list *yl)
{
  unsigned char *hash_array;
  int i;
  struct yomi_entry *ye;
  if (!(hash_array = (unsigned char *)malloc(YOMI_HASH_ARRAY_SIZE))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return;
  }
  for (i = 0; i < YOMI_HASH_ARRAY_SIZE; i++) {
    hash_array[i] = 0;
  }
  for (i = 0; i < yl->nr_valid_entries; i++) {
    ye = yl->ye_array[i];
    mark_hash_array(hash_array, ye->index_xstr);
  }
  fwrite(hash_array, YOMI_HASH_ARRAY_SIZE, 1, yomi_hash_out);
  printf("generated yomi hash bitmap (%d collisions/%d entries)\n",
	 yomi_hash_collision, yl->nr_valid_entries);
  free(hash_array);
}

static struct adjust_command *
parse_modify_freq_command (const char        *buf,
                           struct mkdic_stat *mds)
{
  char *line = alloca (strlen (buf) + 1);
  char *yomi, *wt, *word, *type_str;
  struct adjust_command *cmd;
  int type = 0;
  strcpy (line, buf);
  yomi = strtok (line, " ");
  wt = strtok (NULL, " ");
  word = strtok (NULL, " ");
  type_str = strtok (NULL, " ");
  if (!yomi || !wt || !word || !type_str)
    return NULL;
  if (!strcmp (type_str, "up"))
    type = ADJUST_FREQ_UP;
  if (!strcmp (type_str, "down"))
    type = ADJUST_FREQ_DOWN;
  if (!strcmp (type_str, "kill"))
    type = ADJUST_FREQ_KILL;
  if (!type)
    return NULL;
  if (!(cmd = malloc (sizeof (struct adjust_command)))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return NULL;
  }
  cmd->type = type;
  cmd->yomi = anthy_cstr_to_xstr (yomi, mds->input_encoding);
  cmd->wt = get_wt_name(wt);
  if (mds->input_encoding == ANTHY_EUC_JP_ENCODING)
    cmd->word = anthy_conv_euc_to_utf8 (word);
  else
    cmd->word = strdup (word);
  return cmd;
}

static void
parse_adjust_command (const char        *buf,
                      struct mkdic_stat *mds)
{
  struct adjust_command *cmd = NULL;
  if (!strncmp ("\\modify_freq ", buf, 13)) {
    cmd = parse_modify_freq_command (&buf[13], mds);
  }
  if (cmd) {
    struct adjust_command *ac_list = &mds->ac_list;
    cmd->next = ac_list->next;
    ac_list->next = cmd;
  }
}

/** 辞書を一行ずつ読み込んでリストを作る
 * このコマンドのコア */
static void
parse_dict_file(FILE *fin, struct mkdic_stat *mds)
{
  xstr *index_xs;
  char buf[MAX_LINE_LEN];
  char *ent;
  struct yomi_entry *ye = NULL;

  /* １行ずつ処理 */
  while (read_line(fin, buf)) {
    if (buf[0] == '\\' && buf[1] != ' ') {
      parse_adjust_command(buf, mds);
      continue ;
    }
    index_xs = get_index_from_line(mds, buf);
    if (!index_xs) {
      break;
    }
    ent = get_entry_from_line(buf);

    /* 読みが30文字を越える場合は無視 */
    if (index_xs->len < 31) {
      ye = find_yomi_entry(&mds->yl, index_xs, 1);
      push_back_word_entry_line(mds, ye, ent);
    }

    free(ent);
    anthy_free_xstr(index_xs);
  }
}

/* 読み、品詞、単語の三つ組から単語の構造体を取得する */
static struct word_entry *
find_word_entry(struct yomi_entry_list *yl, xstr *yomi,
		const char *wt, char *word)
{
  struct yomi_entry *ye = find_yomi_entry(yl, yomi, 0);
  int i;
  if (!ye) {
    return NULL;
  }
  for (i = 0; i < ye->nr_entries; i++) {
    struct word_entry *we = &ye->entries[i];
    if (!strcmp(we->wt_name, wt) &&
	!strcmp(we->word_utf8, word)) {
      return we;
    }
  }
  return NULL;
}
		
/* 頻度調整のコマンドを適用する */
static void
apply_adjust_command(struct yomi_entry_list *yl,
		     struct adjust_command *ac_list)
{
  struct adjust_command *cmd;
  for (cmd = ac_list->next; cmd; cmd = cmd->next) {
    struct word_entry *we;
    assert(cmd);
    we = find_word_entry(yl, cmd->yomi, cmd->wt, cmd->word);
    if (!we) {
      char *yomi = anthy_xstr_to_cstr(cmd->yomi, ANTHY_UTF8_ENCODING);
      printf("failed to find target of adjust command (%s, %s, %s)\n",
	     yomi, cmd->wt, cmd->word);
      free(yomi);
      continue;
    }
    if (cmd->type == ADJUST_FREQ_UP) {
      we->raw_freq *= 4;
    }
    if (cmd->type == ADJUST_FREQ_DOWN) {
      we->raw_freq /= 4;
      if (we->raw_freq == 0) {
	we->raw_freq = 1;
      }
    }
    if (cmd->type == ADJUST_FREQ_KILL) {
      we->raw_freq = 0;
    }
  }
}

/* qsort用の比較関数 */
static int
compare_yomi_entry(const void *p1, const void *p2)
{
  const struct yomi_entry *const *y1 = p1;
  const struct yomi_entry *const *y2 = p2;
  return strcmp((*y1)->index_str, (*y2)->index_str);
}

/* yomi_entryでsortする */
static void
sort_word_dict(struct yomi_entry_list *yl)
{
  int i;
  struct yomi_entry *ye;
  yl->nr_valid_entries = 0;
  /* 単語を持つ読みだけを yl->ye_arrayに詰め直す */
  if (!(yl->ye_array = malloc(sizeof(struct yomi_entry *) * yl->nr_entries))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return;
  }
  for (i = 0, ye = yl->head; i < yl->nr_entries; i++, ye = ye->next) {
    if (ye->nr_entries > 0) {
      yl->ye_array[yl->nr_valid_entries] = ye;
      yl->nr_valid_entries++;
    }
  }
  /**/
  for (i = 0; i < yl->nr_valid_entries; i++) {
    struct yomi_entry *ye = yl->ye_array[i];
    ye->index_str = anthy_xstr_to_cstr(ye->index_xstr, yl->index_encoding);
  }
  /* ソートする */
  qsort(yl->ye_array, yl->nr_valid_entries,
	sizeof(struct yomi_entry *),
	compare_yomi_entry);
  /* 不要な単語を消す */
  yl->nr_words = 0;
  for (i = 0; i < yl->nr_valid_entries; i++) {
    struct yomi_entry *ye = yl->ye_array[i];
    yl->nr_words += normalize_word_entry(ye);
  }
}

/** ファイルのサイズを取得する */
static int
get_file_size(FILE *fp)
{
  if (!fp) {
    return 0;
  }
  return (ftell (fp) + SECTION_ALIGNMENT - 1) & (-SECTION_ALIGNMENT);
}

static void
copy_file(struct mkdic_stat *mds, FILE *in, FILE *out)
{
  int i;
  size_t nread;
  char buf[BUFSIZ];

  /* Pad OUT to the next aligned offset.  */
  for (i = ftell (out); i & (SECTION_ALIGNMENT - 1); i++) {
    fputc (0, out);
  }

  /* Copy the contents.  */
  rewind (in);
  while ((nread = fread (buf, 1, sizeof buf, in)) > 0) {
    if (fwrite (buf, 1, nread, out) < nread) {
      /* Handle short write (maybe disk full).  */
      fprintf (stderr, "%s: %s: write error: %s\n",
	       progname, mds->output_fn, strerror (errno));
      exit (1);
    }
  }
}

static void
generate_header(FILE *fp)
{
  int buf[NR_HEADER_SECTIONS];
  int i;
  struct file_section *fs;
  int off;

  /* 初期化 */
  for (i = 0; i < NR_HEADER_SECTIONS; i++) {
    buf[i] = 0;
  }

  /* ヘッダ */
  buf[0] = NR_HEADER_SECTIONS * sizeof(int);
  buf[1] = 0;

  /* 各セクションのオフセット */
  off = buf[0];
  for (i = 2, fs = file_array; fs->fpp; fs ++, i++) {
    buf[i] = off;
    off += get_file_size(*(fs->fpp));
  }

  /* ファイルへ出力する */
  for (i = 0; i < NR_HEADER_SECTIONS; i++) {
    write_nl(fp, buf[i]);
  }
}

/* 各セクションのファイルをマージして、ひとつの辞書ファイルを作る */
static void
link_dics(struct mkdic_stat *mds)
{
  FILE *fp;
  struct file_section *fs;

  fp = fopen (mds->output_fn, "w");
  if (!fp) {
      fprintf (stderr, "%s: %s: cannot create: %s\n",
	       progname, mds->output_fn, strerror (errno));
      exit (1);
  }

  /* ヘッダを出力する */
  generate_header(fp);

  for (fs = file_array; fs->fpp; fs ++) {
    /* 各セクションのファイルを結合する */
    copy_file(mds, *(fs->fpp), fp);
    if (fs->fn) {
      unlink(fs->fn);
    }
  }

  if (fclose (fp)) {
    fprintf (stderr, "%s: %s: write error: %s\n",
	     progname, mds->output_fn, strerror (errno));
    exit (1);
  }
}

static void
read_dict_file(struct mkdic_stat *mds, const char *fn)
{
  FILE *fp;
  /* ファイル名が指定されたので読み込む */
  fp = fopen(fn, "r");
  if (fp) {
    printf("file = %s\n", fn);
    parse_dict_file(fp, mds);
    fclose(fp);
  } else {
    printf("failed file = %s\n", fn);
  }
}

static void
complete_words(struct mkdic_stat *mds)
{
  /* 頻度補正を適用する */
  apply_adjust_command(&mds->yl, &mds->ac_list);

  /**/
  calc_freq(&mds->yl);

  /* 読みで並び替える */
  sort_word_dict(&mds->yl);

  /* ファイルを準備する */
  open_output_files();
  /* 単語辞書を出力する */
  output_word_dict(&mds->yl);

  /* 読みハッシュを作る */
  mk_yomi_hash(yomi_hash_out, &mds->yl);
}

static void
read_udict_file(struct mkdic_stat *mds, const char *fn)
{
  if (!mds->ud) {
    mds->ud = create_uc_dict();
    complete_words(mds);
  }
  read_uc_file(mds->ud, fn);
  printf("uc = %s\n", fn);
}

static xstr*
xstr_strncat(xstr* xs, xchar* src, int n)
{
  int i;
  xs->str = realloc(xs->str, sizeof(xchar) * (xs->len + n + 1));

  for (i = 0; i < n; ++i) {
    xs->str[xs->len + i] = src[i];
  }
  xs->len += n;
  return xs;
}

static void
reverse_multi_segment_word(struct mkdic_stat *mds, struct word_entry *we)
{
  /*
    「かなかんじへんかんえんじん #T35 #_2仮名_3漢字_4変換_4エンジン」
    から
    「仮名漢字変換エンジン #T35 #_2かな_2かんじ_2へんかん_4えんじん」
    を作る
  */
  int j;
  /* yomiは仮名漢字混じり wordは平仮名のみからなる */
  int yomi_seg_start = 0;
  int word_seg_start = 0;
  int word_seg_len = 0;
  xstr *yomibuf = anthy_cstr_to_xstr(we->word_utf8, ANTHY_UTF8_ENCODING);
  xstr *wordbuf = we->ye->index_xstr;
  xstr *yomi_xs = anthy_cstr_to_xstr("", 0);
  xstr *word_xs = anthy_cstr_to_xstr("#", 0);
  char *word;
  char ch[256];
  struct yomi_entry *target_ye;

  for (j = 0; j <= yomibuf->len; ++j) {
    if (j == yomibuf->len || yomibuf->str[j] == '_') {
      if (yomi_seg_start != 0) {
	anthy_xstrappend(word_xs, '_');
	snprintf(ch, 256, "%x", j - yomi_seg_start);
	anthy_xstrappend(word_xs, (xchar)ch[0]);
	xstr_strncat(word_xs, &wordbuf->str[word_seg_start], word_seg_len);
	xstr_strncat(yomi_xs, &yomibuf->str[yomi_seg_start], j - yomi_seg_start);
      }
      if (j == yomibuf->len) {
	break;
      }
      yomi_seg_start = j + 2;
      word_seg_start += word_seg_len;
      word_seg_len = get_element_len(yomibuf->str[j + 1]);
    }
  }

  target_ye = find_yomi_entry(&mds->yl, yomi_xs, 1);
  word = anthy_xstr_to_cstr(word_xs, mds->input_encoding);

  /* 逆変換用の辞書はfreqが負 */
  push_back_word_entry(mds, target_ye, we->wt_name, -we->raw_freq,
		       word, we->source_order);

  free(word);
  anthy_free_xstr(yomibuf);
  anthy_free_xstr(yomi_xs);
  anthy_free_xstr(word_xs);
}

/* 逆変換用の辞書を作る */
static void
build_reverse_dict(struct mkdic_stat *mds)
{
  struct yomi_entry *ye;
  int i, n;
  struct word_entry *we_array;
  printf("building reverse index\n");

  /* 単語の数を数える */
  n = 0;
  for (ye = mds->yl.head; ye; ye = ye->next) {
    for (i = 0; i < ye->nr_entries; i++) {
      n++;
    }
  }
  /* コピーする
   * (元の辞書中のポインタはreallocで動くのでコピーが必要)
   */
  if (!(we_array = malloc(sizeof(struct word_entry )* n))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return;
  }
  n = 0;
  for (ye = mds->yl.head; ye; ye = ye->next) {
    for (i = 0; i < ye->nr_entries; i++) {
      we_array[n] = ye->entries[i];
      n++;
    }
  }

  /* 辞書に追加していく */
  for (i = 0; i < n; i++) {
    struct word_entry *we;
    struct yomi_entry *target_ye;

    we = &we_array[i];
    if (we->word_utf8[0] == '#') {
      if (we->word_utf8[1] == '_') {
	reverse_multi_segment_word(mds, we);
      }
    } else {
      /* yomiは仮名漢字混じり wordは平仮名のみからなる */
      xstr *yomi_xs;
      char *word;

      yomi_xs = anthy_cstr_to_xstr(we->word_utf8, ANTHY_UTF8_ENCODING);
      target_ye = find_yomi_entry(&mds->yl, yomi_xs, 1);
      word = anthy_xstr_to_cstr(we->ye->index_xstr, mds->input_encoding);

      /* 逆変換用の辞書はfreqが負 */
      push_back_word_entry(mds, target_ye, we->wt_name, -we->raw_freq,
			   word, we->source_order);

      anthy_free_xstr(yomi_xs);
      free(word);
    }
  }
  /**/
  free(we_array);
}

static void
clear_exclude_wtypes(struct mkdic_stat *mds)
{
  int i;
  for (i = 0; i < mds->nr_excluded; i++) {
    free(mds->excluded_wtypes[i]);
  }
  free(mds->excluded_wtypes);
  /**/
  mds->excluded_wtypes = NULL;
  mds->nr_excluded = 0;
}

static void
set_exclude_wtypes(struct mkdic_stat *mds, int nr, char **tokens)
{
  int i;
  assert(nr > 0);
  mds->nr_excluded = nr - 1;
  if (nr == 1) {
    anthy_log(0, "nr == 1 in %s:%d\n", __FILE__, __LINE__);
    return;
  }
  if (!(mds->excluded_wtypes = malloc(sizeof(char *) * (nr - 1)))) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    mds->nr_excluded = 0;
    return;
  }
  /**/
  mds->excluded_wtypes[0] = NULL;
  for (i = 1; i < nr; i++)
    mds->excluded_wtypes[i - 1] = strdup(tokens[i]);
}

static void
free_exclude_wtypes(struct mkdic_stat *mds)
{
  int i;
  for (i = 0; i < mds->nr_excluded; i++) {
    free (mds->excluded_wtypes[i]);
  }
  free (mds->excluded_wtypes);
  mds->excluded_wtypes = NULL;
}

static void
set_dict_encoding(struct mkdic_stat *mds, const char *enc)
{
  if (!strcmp(enc, "utf8")) {
    mds->yl.body_encoding = ANTHY_UTF8_ENCODING;
  }
}

static void
set_input_encoding(struct mkdic_stat *mds, const char *enc)
{
  if (!strcmp(enc, "utf8")) {
    mds->input_encoding = ANTHY_UTF8_ENCODING;
  }
  if (!strcmp(enc, "eucjp")) {
    mds->input_encoding = ANTHY_EUC_JP_ENCODING;
  }
}

static void
write_dict_file(struct mkdic_stat *mds)
{
  if (!mds->ud) {
    printf("can not build without use case dict\n");
    exit(1);
  }

  /* 用例辞書を作る */
  make_ucdict(uc_out, mds->ud);

  /* 辞書ファイルにまとめる */
  flush_output_files();
  link_dics(mds);
}

static void
show_command(char **tokens, int nr)
{
  int i;
  printf("cmd:");
  for (i = 0; i < nr; i++) {
    printf(" %s", tokens[i]);
  }
  printf("\n");
}

static int
execute_batch(struct mkdic_stat *mds, const char *fn)
{
  int nr = 0;
  char **tokens = NULL;
  if (anthy_open_file(fn)) {
    printf("mkanthydic: failed to open %s\n", fn);
    return 1;
  }
  while (!anthy_read_line(&tokens, &nr)) {
    char *cmd;
    if (!nr)
      break;
    assert(nr > 0);
    cmd = tokens[0];
    show_command(tokens, nr);
    if (!strcmp(cmd, "read") && nr == 2) {
      read_dict_file(mds, tokens[1]);
    } else if (!strcmp(cmd, "read_uc") && nr == 2) {
      read_udict_file(mds, tokens[1]);
    } else if (!strcmp(cmd, "build_reverse_dict")) {
      build_reverse_dict(mds);
    } else if (!strcmp(cmd, "write")) {
      write_dict_file(mds);
    } else if (!strcmp(cmd, "set_exclude_wtypes")) {
      set_exclude_wtypes(mds, nr, tokens);
    } else if (!strcmp(cmd, "clear_exclude_wtypes")) {
      clear_exclude_wtypes(mds);
    } else if (!strcmp(cmd, "set_dict_encoding") && nr == 2) {
      set_dict_encoding(mds, tokens[1]);
    } else if (!strcmp(cmd, "set_input_encoding") && nr == 2) {
      set_input_encoding(mds, tokens[1]);
    } else if (!strcmp(cmd, "done")) {
      anthy_free_line();
      break;
    } else {
      printf("Unknown command(%s).\n", cmd);
    }
    anthy_free_line();
  }
  anthy_close_file();
  return 0;
}

/* 辞書生成のための変数の初期化 */
static void
init_mds(struct mkdic_stat *mds, char *output)
{
  int i;
  mds->output_fn = output;
  mds->ud = NULL;

  /* 単語辞書を初期化する */
  mds->yl.head = NULL;
  mds->yl.nr_entries = 0;
  for (i = 0; i < YOMI_HASH; i++) {
    mds->yl.hash[i] = NULL;
  }
  mds->yl.index_encoding = ANTHY_UTF8_ENCODING;
  mds->yl.body_encoding = ANTHY_EUC_JP_ENCODING;
  /**/
  mds->ac_list.next = NULL;
  /**/
  mds->input_encoding = ANTHY_EUC_JP_ENCODING;
  /**/
  mds->nr_excluded = 0;
  mds->excluded_wtypes = NULL;
}

static void
free_yomi_entry_list(struct yomi_entry_list *yl)
{
  struct yomi_entry *ye, *ye_prev;
  int i;

  free (yl->ye_array);
  yl->ye_array = NULL;
  for (i = 0, ye = yl->head; ye && (i < yl->nr_entries); i++) {
    free (ye->index_xstr->str);
    free (ye->index_xstr);
    ye->index_xstr = NULL;
    free (ye->index_str);
    ye->index_str = NULL;
    free (ye->entries);
    ye->entries = NULL;
    ye->hash_next = NULL;
    ye_prev = ye;
    ye = ye->next;
    free (ye_prev);
  }
}

/* libanthyの使用する部分だけを初期化する */
static void
init_libs(void)
{
  int res;
  res = anthy_init_xstr();
  if (res == -1) {
    fprintf (stderr, "failed to init dic lib\n");
    exit(1);
  }
}

/**/
int
main(int argc, char **argv)
{
  struct mkdic_stat mds;
  int i;
  char *script_fn = NULL;
  char *out_fn = DEFAULT_FN;
  int help_mode = 0;
  int retval;

  for (i = 1; i < argc; i++) {
    char *arg = argv[i];
    char *prev_arg = argv[i-1];
    if (!strcmp(arg, "--help")) {
      help_mode = 1;
    }
    if (!strcmp(prev_arg, "-f")) {
      script_fn = arg;
    }
    if (!strcmp(prev_arg, "-o")) {
      out_fn = arg;
    }
  }

  if (help_mode || !script_fn) {
    print_usage();
  }

  anthy_init_wtypes();
  init_libs();
  init_mds(&mds, out_fn);

  retval = execute_batch(&mds, script_fn);
  free_yomi_entry_list(&mds.yl);
  free_exclude_wtypes(&mds);
  free_uc_dict(mds.ud);
  mds.ud = NULL;
  return retval;
}
