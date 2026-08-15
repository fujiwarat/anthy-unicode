/*
 * 学習の履歴などを管理するためのデータベース
 * 文字列(xstr)をキーにして高速に行(row)を検索することができる．
 * 複数のセクションをもつことができ，学習の違うフェーズなどに対応する
 *  (セクション * 文字列 -> 行)
 * 各行は文字列か数を持つ配列になっている
 *
 * 「パトリシア・トライ」というデータ構造を使用している。
 * 自然言語の検索などを扱っている教科書を参照のこと
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
/*
 * Funded by IPA未踏ソフトウェア創造事業 2002 1/18
 * Funded by IPA未踏ソフトウェア創造事業 2005
 * Copyright (C) 2005 YOSHIDA Yuichi
 * Copyright (C) 2000-2006 TABATA Yusuke
 * Copyright (C) 2000-2003 UGAWA Tomoharu
 * Copyright (C) 2001-2002 TAKAI Kosuke
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
 */
/*
 * パーソナリティ""は匿名パーソナリティであり，
 * ファイルへの読み書きは行わない．
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include <anthy/anthy.h>
#include <anthy/dic.h>
#include <anthy/alloc.h>
#include <anthy/conf.h>
#include <anthy/ruleparser.h>
#include <anthy/record.h>
#include <anthy/logger.h>
#include <anthy/prediction.h>

#include "dic_main.h"
#include "dic_personality.h"

/* 個人辞書をセーブするファイル名のsuffix */
#define ENCODING_SUFFIX ".utf8"


enum val_type {
  RT_EMPTY, RT_VAL, RT_XSTR, RT_XSTRP
};

/* 値 */
struct record_val {
  enum val_type type;
  union {
    xstr str;
    int val;
    xstr* strp;
  } u;
};

/* 行 */
struct record_row {
  xstr key;
  int nr_vals;
  struct record_val *vals;
};

/* trie node管理用 */
struct trie_node {
  struct trie_node *l;
  struct trie_node *r;
  int bit;
  struct record_row row;
  struct trie_node *lru_prev, *lru_next; /* 両端ループ */
  int dirty; /* LRU のための used, sused ビット */
};

/* trie treeのroot */
struct trie_root {
  struct trie_node root;
  allocator node_ator;
};

#define LRU_USED  0x01
#define LRU_SUSED 0x02
#define PROTECT   0x04 /* 差分書き出し時に使う(LRUとは関係ない)
			*   差分書き出しでは、ファイルに書き出す前に
			*   ファイル上に他のプロセスが記録した更新を
			*   読み込む。それによって、これから追加しよ
			*   うとするノードが消されるのを防ぐ
			*/
/*
 * LRU:
 *   USED:  メモリ上で使われた
 *   SUSED: 保存された used ビット
 *
 * LRUリスト上では、 USED は必ずリスト先頭に並んでいるが、 SUSED は
 * フラグなしのノードと混在している可能性がある。
 *
 * n個を残すように指定された時の動作
 *    1. used > n
 *        LRU リストの先頭から n 番目以降を消す
 *    2. used + sused > n
 *        used -> 残す
 *        sused -> sused フラグを落す
 *        それ以外 -> 消す
 *    3. それ以外
 *        全て残す
 * ファイルに書き出す時に、 used || sused -> sused として書き出す
 */

/** セクション */
struct record_section {
  const char *name;
  struct trie_root cols;
  struct record_section *next;
  int lru_nr_used, lru_nr_sused; /* LRU 用 */
};

/** データベース */
struct record_stat {
  struct record_section section_list; /* sectionのリスト*/
  struct record_section *cur_section;
  struct trie_root xstrs; /* xstr を intern するための trie */
  struct trie_node *cur_row;
  int row_dirty; /* cur_row が保存の必要があるか */
  int encoding;
  /**/
  int is_anon;
  const char *id;         /* パーソナリティのid */
  char *base_fn; /* 基本ファイル 絶対パス */
  char *journal_fn; /* 差分ファイル 絶対パス */
  /**/
  time_t base_timestamp; /* 基本ファイルのタイムスタンプ */
  int last_update;  /* 差分ファイルの最後に読んだ位置 */
  time_t journal_timestamp; /* 差分ファイルのタイムスタンプ */
};

/* 差分が100KB越えたら基本ファイルへマージ */
#define FILE2_LIMIT 102400


/*
 * xstr の intern:
 *  個人ごと( record_stat ごと)に文字列を intern する。これは、
 *  メモリの節約の他に、データベースの flush 時にデータベースに
 *  由来する xstr が無効になるのを防ぐ目的がある。
 *  したがって、データベースの flush 時でも xstr の intern 用
 *  のデータベース xstrs はそのまま保存する。
 *
 *  xstrs: xstr の intern 用のデータベース
 *         row の key を intern された xstr として使う。
 *         row に value は持たない。
 *                    (将来的には参照カウンタをつけてもいいかも)
 *  参照: intern_xstr()
 */

/*
 * 差分書き出し:
 *  データベースの保存、複数の anthy ライブラリをリンクした
 *  プロセスの学習履歴の同期のために、学習履歴の更新情報を
 *  逐一ファイルに書き出す。
 *
 * ・基本ファイル  古い anthy の学習履歴と同じ形式。
 *                 差分情報を適用する元となるファイル。
 *                 基本的には起動時だけに読み込む。
 *                 このプログラム中でファイル1，baseと呼ぶことがある。
 * ・差分ファイル  基本ファイルに対する更新情報。
 *                 データベースに対する更新がコミットされるたびに
 *                 読み書きされる。
 *                 このプログラム中でファイル2，journalと呼ぶことがある。
 *  基本方針:
 *     データベースに対する更新がコミットされると、まず差分ファイル
 *     に他のプロセスが追加した更新情報を読み込み、その後に自分の
 *     コミットした更新を差分ファイルに書き出す。
 *     これらはロックファイルを用いてアトミックに行われる。また、
 *     基本ファイル、差分ファイルとも、ロックを取っている間しか
 *     オープンしていてはいけない。
 *  追加と削除:
 *     追加はすでにメモリ上で更新された row をコミットによって
 *     メモリに書き出すため、
 *       1. コミット対象 row 以外を差分ファイルの情報で
 *       2. コミット対象 row を差分ファイルに書き出し
 *     とする。削除はまだメモリ上に row が残っている状態でコミット
 *     が行われる(削除要求をコミットとして扱う)ため、
 *       1. 削除の情報を差分ファイルに書き出し
 *       2. 差分ファイルの読み込みにより削除要求も実行する
 *     とする。
 *  基本ファイルの更新:
 *     差分ファイルがある程度肥大化すると、差分ファイルの情報を
 *     基本ファイルに反映して差分ファイルを空にする。
 *     更新するプロセス:
 *       差分ファイルに書き出しを行った後、差分ファイルの大きさを調べ、
 *       肥大化していれば、そのときのメモリ上のデータベース(これには
 *       全ての差分ファイルの更新が適用されている)を基本ファイルに
 *       書き出す。
 *     それ以外のプロセス:
 *       差分ファイルを読む前に、基本ファイルが更新されているかを
 *       ファイルのタイムスタンプで調べ、更新されていれば、コミット
 *       された更新情報を直ちに更新ファイルに追加し、メモリ上の
 *       データベースを flush した後基本ファイル、差分ファイルを
 *       読み込み直す。
 *       データベースの flush により、
 *           ・cur_row が無効になる (NULL になる)
 *           ・cur_section の有効性は保存される(sectionは解放しない)
 *           ・xstr は intern していれば保存される
 *                              (すべての xstr は intern されているはず)
 *   結局、次の様になる:
 *     if (基本ファイルが更新されている) {
 *             差分ファイルへコミットされた更新を書き出す;
 *             データベースのフラッシュ;
 *             基本ファイルの読込と差分ファイルの最終読込位置クリア;
 *             差分ファイルの読込と差分ファイルの最終読込位置更新;
 *     } else {
 *             if (追加) {
 *                     差分ファイルの読込と差分ファイルの最終読込位置更新;
 *                     差分ファイルへの書き出し;
 *             } else {
 *                     差分ファイルへの書き出し;
 *                     差分ファイルの読込と差分ファイルの最終読込位置更新;
 *             }
 *     }
 *     if (差分ファイルが大きい) {
 *             基本ファイルへの書き出し;
 *             差分ファイルのクリア;
 *     }
 */

static allocator record_ator;

/* trie操作用 */
static void init_trie_root(struct trie_root *n);
static int trie_key_nth_bit(xstr* key, int n);
static int trie_key_first_diff_bit_1byte(xchar c1, xchar c2);
static int trie_key_first_diff_bit(xstr *k1, xstr *k2);
static int trie_key_cmp(xstr *k1, xstr *k2);
static void trie_key_dup(xstr *dst, xstr *src);
static void trie_row_init(struct record_row *rc);
static void trie_row_free(struct record_row *rc);
static struct trie_node *trie_find(struct trie_root *root, xstr *key);
static struct trie_node *trie_insert(struct trie_root *root, xstr *key,
				     int dirty, int *nr_used, int *nr_sused);
static void trie_remove(struct trie_root *root, xstr *key,
			int *nr_used, int *nr_sused);
static struct trie_node *trie_first(struct trie_root *root);
static struct trie_node *trie_next(struct trie_root *root,
				   struct trie_node *cur);
static void trie_remove_all(struct trie_root *root,
			    int *nr_used, int *nr_sused);
static void trie_remove_old(struct trie_root *root, int count,
			    int* nr_used, int* nr_sused);
static void trie_mark_used(struct trie_root *root, struct trie_node *n,
			   int *nr_used, int *nr_sused);


/*
 * トライの実装
 * struct trie_nodeのうちrow以外の部分とrow.keyを使用
 * 削除の時はtrie_row_freeを使ってrowの内容を解放
 */

#define PUTNODE(x) ((x) == &root->root ? printf("root\n") : anthy_putxstrln(&(x)->row.key))


/*
 * Orphan debug function, presumably for dumping trees.
 */


/* static int */
/* debug_trie_dump(FILE* fp, struct trie_node* n, int encoding) */
/* { */
/*   int cnt = 0; */
/*   char buf[1024]; */

/*   if (n->l->bit > n->bit) { */
/*     cnt = debug_trie_dump(fp, n->l, encoding); */
/*   } else { */
/*     if (n->l->row.key.len == -1) { */
/*       if (fp) { */
/* 	fprintf(fp, "root\n"); */
/*       } */
/*     } else { */
/*       if (fp) { */
/* 	anthy_sputxstr(buf, &n->l->row.key, encoding); */
/* 	fprintf(fp, "%s\n", buf); */
/*       } */
/*       cnt = 1; */
/*     } */
/*   } */

/*   if (n->r->bit > n->bit) { */
/*     return cnt + debug_trie_dump(fp, n->r, encoding); */
/*   } else { */
/*     if (n->r->row.key.len == -1) { */
/*       if(fp) { */
/* 	fprintf(fp, "root\n"); */
/*       } */
/*     } else { */
/*       if(fp) { */
/* 	anthy_sputxstr(buf, &n->r->row.key, encoding); */
/* 	fprintf(fp, "%s\n", buf); */
/*       } */
/*       return cnt + 1; */
/*     } */
/*   } */

/*   return cnt; */
/* } */

static void
init_trie_root(struct trie_root *root)
{
  struct trie_node* n;
  root->node_ator = anthy_create_allocator(sizeof(struct trie_node), NULL);
  n = &root->root;
  n->l = n;
  n->r = n;
  n->bit = 0;
  n->lru_next = n;
  n->lru_prev = n;
  n->dirty = 0;
  trie_row_init(&n->row);
  n->row.key.len = -1;
}

/*
 * bit0: 0
 * bit1: headのキーだけ0
 * bit2: 文字列のビット0
 * bit3: 文字列のビット1
 *   ...
 * 文字列長を越えると0
 */
static int
trie_key_nth_bit(xstr* key, int n)
{
  switch (n) {
  case 0:
    return 0;
  case 1:
    return key->len + 1; /* key->len == -1 ? 0 : non-zero */
  default:
    {
      int pos;
      n -= 2;
      pos = n / (sizeof(xchar) << 3);
      if (pos >= key->len) {
	return 0;
      }
      return key->str[pos] & (1 << (n % (sizeof(xchar) << 3)));
    }
  }
}

/* c1 == c2 では呼んではいけない */
static int
trie_key_first_diff_bit_1byte(xchar c1, xchar c2)
{
  int i;
  int ptn;
  for (i = 0, ptn = c1 ^ c2; !(ptn & 1); i++, ptn >>= 1 )
    ;
  return i;
}

/*
 * k1 == k2 では呼んではいけない
 * ki->str[0 .. (ki->len - 1)]に0はないと仮定
 */
#define MIN(a,b) ((a)<(b)?(a):(b))
static int
trie_key_first_diff_bit(xstr *k1, xstr *k2)
{
  int len;
  int i;

  len = MIN(k1->len, k2->len);
  if (len == -1) {
    return 1;
  }
  for ( i = 0 ; i < len ; i++ ){
    if (k1->str[i] != k2->str[i]) {
      return (2 + (i * (sizeof(xchar) << 3)) +
	      trie_key_first_diff_bit_1byte(k1->str[i], k2->str[i]));
    }
  }
  if (k1->len < k2->len) {
    return (2 + (i * (sizeof(xchar) << 3)) +
	    trie_key_first_diff_bit_1byte(0, k2->str[i]));
  } else {
    return (2 + (i * (sizeof(xchar) << 3)) +
	    trie_key_first_diff_bit_1byte(k1->str[i], 0));
  }
}
#undef MIN

static int
trie_key_cmp(xstr *k1, xstr *k2)
{
  if (k1->len == -1 || k2->len == -1) {
    return k1->len - k2->len;
  }
  return anthy_xstrcmp(k1, k2);
}

static void
trie_key_dup(xstr *dst, xstr *src)
{
  dst->str = anthy_xstr_dup_str(src);
  dst->len = src->len;
}

/*
 * 見つからなければ 0
 */
static struct trie_node *
trie_find(struct trie_root *root, xstr *key)
{
  struct trie_node *p;
  struct trie_node *q;

  p = &root->root;
  q = p->l;
  while (p->bit < q->bit) {
    p = q;
    q = trie_key_nth_bit(key, p->bit) ? p->r : p->l;
  }
  return trie_key_cmp(&q->row.key,key) ? NULL : q;
}

/*
 * 最長マッチのための補助関数
 *  key で探索して、始めて一致しなくなったノードを返す。
 */
static struct trie_node *
trie_find_longest (struct trie_root* root, xstr *key)
{
  struct trie_node *p;
  struct trie_node *q;

  p = &root->root;
  q = p->l;
  while (p->bit < q->bit) {
    p = q;
    q = trie_key_nth_bit(key, p->bit) ? p->r : p->l;
  }

  return q;
}

/*
 * 追加したノードを返す
 * すでに同じキーをもつノードがあるときは、追加せずに0を返す
 */
static struct trie_node *
trie_insert(struct trie_root *root, xstr *key,
	    int dirty, int *nr_used, int *nr_sused)
{
  struct trie_node *n;
  struct trie_node *p;
  struct trie_node *q;
  int i;

  p = &root->root;
  q = p->l;
  while (p->bit < q->bit) {
    p = q;
    q = trie_key_nth_bit(key, p->bit) ? p->r : p->l;
  }
  if (trie_key_cmp(&q->row.key,key) == 0) {
    /* USED > SUSED > 0 で強い方を残す */
    if (dirty == LRU_USED) {
      trie_mark_used(root, q, nr_used, nr_sused);
    } else if (q->dirty == 0) {
      q->dirty = dirty;
    }
    return 0;
  }
  i = trie_key_first_diff_bit(&q->row.key, key);
  p = &root->root;
  q = p->l;
  while (p->bit < q->bit && i > q->bit) {
    p = q;
    q = trie_key_nth_bit(key, p->bit) ? p->r : p->l;
  }
  n = anthy_smalloc(root->node_ator);
  trie_row_init(&n->row);
  trie_key_dup(&n->row.key, key);
  n->bit = i;
  if (trie_key_nth_bit(key, i)) {
    n->l = q;
    n->r = n;
  } else {
    n->l = n;
    n->r = q;
  }
  if (p->l == q) {
    p->l = n;
  } else {
    p->r = n;
  }

  /* LRU の処理 */
  if (dirty == LRU_USED) {
    root->root.lru_next->lru_prev = n;
    n->lru_prev = &root->root;
    n->lru_next = root->root.lru_next;
    root->root.lru_next = n;
    (*nr_used)++;
  } else {
    root->root.lru_prev->lru_next = n;
    n->lru_next = &root->root;
    n->lru_prev = root->root.lru_prev;
    root->root.lru_prev = n;
    if (dirty == LRU_SUSED) {
      (*nr_sused)++;
    }
  }
  n->dirty = dirty;
  return n;
}

/*
 * ノードを見つけると削除する
 * 内部でtrie_row_freeを呼び、キーを含むデータ部分をfreeする
 *
 * データとノードを削除する。
 * 削除対象のデータは削除対象のノードに格納されているとは
 * 限らないことに注意。
 * 1. 削除対象の葉を持つノードに削除対象の葉が含まれているとき
 *  削除対象のノードは、子への枝のうち、生きのこる枝を親に渡して死ぬ
 * 2. 削除対象の葉を持つノードの祖先に削除対象の葉が含まれているとき
 *  1. に加えて、削除対象の葉をもつノードを殺して、代わりに削除
 *  対象のノードを削除対象の葉をもつノードの位置に移動させ生かす
 */
static void
trie_remove(struct trie_root *root, xstr *key,
	    int *nr_used, int *nr_sused)
{
  struct trie_node *p;
  struct trie_node *q;
  struct trie_node **pp = NULL; /* gcc の warning 回避 */
  struct trie_node **qq;
  p = &root->root;
  qq = &p->l;
  q = *qq;
  while (p->bit < q->bit) {
    pp = qq;
    p = q;
    qq = trie_key_nth_bit(key,p->bit) ? &p->r : &p->l;
    q = *qq;
  }
  if (trie_key_cmp(&q->row.key, key) != 0) {
    return ;
  }
  if (p != q) {
    /* case 2. */
    struct trie_node *r;
    struct trie_node *s;
    r = &root->root;
    s = r->l;
    while (s != q) {
      r = s;
      s = trie_key_nth_bit(key, r->bit) ? r->r : r->l;
    }
    assert(pp);
    *pp = (p->r == q) ? p->l : p->r;
    p->l = q->l;
    p->r = q->r;
    p->bit = q->bit;
    if (trie_key_nth_bit(key, r->bit)) {
      r->r = p;
    } else {
      r->l = p;
    }
    p = q;
  } else {
    *pp = (p->r == q) ? p->l : p->r;
  }
  p->lru_prev->lru_next = p->lru_next;
  p->lru_next->lru_prev = p->lru_prev;
  if (p->dirty == LRU_USED) {
    (*nr_used)--;
  } else if (p->dirty == LRU_SUSED) {
    (*nr_sused)--;
  }
  trie_row_free(&p->row);
  anthy_sfree(root->node_ator, p);
}

/* head以外のノードがなければ 0 を返す */
static struct trie_node *
trie_first (struct trie_root *root)
{
  return root->root.lru_next == &root->root ?
    NULL : root->root.lru_next;
}

/* 次のノードがなければ 0 を返す */
static struct trie_node *
trie_next (struct trie_root *root,
	   struct trie_node *cur)
{
  return cur->lru_next == &root->root ? 0 : cur->lru_next;
}

/*
 * head以外全てのノードを削除する
 * 内部でtrie_row_freeを呼び、キーを含むデータ部分をfreeする
 */
static void
trie_remove_all (struct trie_root *root,
		 int *nr_used, int *nr_sused)
{
  struct trie_node* p;
  for (p = root->root.lru_next; p != &root->root; p = p->lru_next) {
    trie_row_free(&p->row);
  }
  anthy_free_allocator(root->node_ator);
  init_trie_root(root);
  *nr_used = 0;
  *nr_sused = 0;
}

/*
 * LRU リストの先頭から count 番目までを残して残りを解放する
 */
static void
trie_remove_old (struct trie_root *root, int count,
		 int *nr_used, int *nr_sused)
{
  struct trie_node *p;
  struct trie_node *q;

  if (*nr_used > count) {
    for (p = root->root.lru_next; count; count--, p = p->lru_next)
      ;
    /* p から head までを消す */
    for ( ; p != &root->root; p = q) {
      q = p->lru_next;
      trie_remove(root, &p->row.key, nr_used, nr_sused);
    }
  } else if (*nr_used + *nr_sused > count) {
    for (p = root->root.lru_next; p->dirty == LRU_USED; p = p->lru_next)
      ;
    /*
     * p から root まで  sused    -> dirty := 0
     *                   それ以外 -> 消す
     */
    for ( ; p != &root->root; p = q) {
      q = p->lru_next;
      if (p->dirty == LRU_SUSED) {
	p->dirty = 0;
      } else {
	trie_remove(root, &p->row.key, nr_used, nr_sused);
      }
    }
    *nr_sused = 0;
  }
}

static void
trie_mark_used (struct trie_root *root, struct trie_node *n,
		int *nr_used, int *nr_sused)
{
  switch(n->dirty) {
  case LRU_USED:
    break;
  case LRU_SUSED:
    (*nr_sused)--;
    /* fall through */
  default:
    n->dirty = LRU_USED;
    (*nr_used)++;
    break;
  }
  n->lru_prev->lru_next = n->lru_next;
  n->lru_next->lru_prev = n->lru_prev;
  root->root.lru_next->lru_prev = n;
  n->lru_next = root->root.lru_next;
  root->root.lru_next = n;
  n->lru_prev = &root->root;
}

/*
 * トライの実装はここまで
 */

static xstr *
do_get_index_xstr(struct record_stat *rec)
{
  if (!rec->cur_row) {
    return 0;
  }
  return &rec->cur_row->row.key;
}

static struct record_section*
do_select_section(struct record_stat *rst, const char *name, int flag)
{
  struct record_section *rsc;

  for (rsc = rst->section_list.next; rsc; rsc = rsc->next) {
    if (!strcmp(name, rsc->name)) {
      return rsc;
    }
  }

  if (flag) {
    rsc = malloc(sizeof(struct record_section));
    rsc->name = strdup(name);
    rsc->next = rst->section_list.next;
    rst->section_list.next = rsc;
    rsc->lru_nr_used = 0;
    rsc->lru_nr_sused = 0;
    init_trie_root(&rsc->cols);
    return rsc;
  }

  return NULL;
}

static struct trie_node*
do_select_longest_row(struct record_section *rsc, xstr *name)
{
  struct trie_node *mark, *found;
  xstr xs;
  int i;

  if ((NULL == name) || (NULL == name->str) || (name->len < 1) || (0 == name->str[0])) {
    /* 辞書もしくは学習データが壊れていた時の対策 */
    return NULL;
  }

  mark = trie_find_longest(&rsc->cols, name);
  xs.str = name->str;
  for (i = (mark->row.key.len <= name->len) ? mark->row.key.len : name->len; i > 1; i--) {  /* 不正なメモリアクセスの修正 */
    /* ルートノードは i == 1 でマッチするので除外
     * trie_key_nth_bit 参照
     */
    xs.len = i;
    found = trie_find(&rsc->cols, &xs);
    if (found) {
      return found;
    }
  }
  return NULL;
}

static struct trie_node*
do_select_row(struct record_section* rsc, xstr *name,
		 int flag, int dirty)
{
  struct trie_node *node;

  if (flag) {
    node = trie_insert(&rsc->cols, name, dirty,
		       &rsc->lru_nr_used, &rsc->lru_nr_sused);
    if (node) {
      node->row.nr_vals = 0;
      node->row.vals = 0;
    } else {
      node = trie_find(&rsc->cols, name);
    }
  } else {
    node = trie_find(&rsc->cols, name);
  }
  return node;
}

static void
do_mark_row_used(struct record_section* rsc, struct trie_node* node)
{
  trie_mark_used(&rsc->cols, node, &rsc->lru_nr_used, &rsc->lru_nr_sused);
}

static void
do_truncate_section(struct record_stat *s, int count)
{
  if (!s->cur_section) {
    return;
  }

  trie_remove_old(&s->cur_section->cols, count,
		  &s->cur_section->lru_nr_used,
		  &s->cur_section->lru_nr_sused);
}


static struct trie_node*
do_select_first_row(struct record_section *rsc)
{
  return trie_first(&rsc->cols);
}

static struct trie_node*
do_select_next_row(struct record_section *rsc,
  struct trie_node* node)
{
  return trie_next(&rsc->cols, node);
}


static int
do_get_nr_values(struct trie_node *node)
{
  if (!node)
    return 0;
  return node->row.nr_vals;
}

static struct record_val *
get_nth_val_ent(struct trie_node *node, int n, int f)
{
  struct record_row *col;
  col = &node->row;
  if (n < 0) {
    return NULL;
  }
  if (n < do_get_nr_values(node)) {
    return &col->vals[n];
  }
  if (f) {
    int i;
    col->vals = realloc(col->vals, sizeof(struct record_val)*(n + 1));
    for (i = col->nr_vals; i < n+1; i++) {
      col->vals[i].type = RT_EMPTY;
    }
    col->nr_vals = n + 1;
    return &col->vals[n];
  }
  return NULL;
}

static void
free_val_contents(struct record_val* v)
{
  switch (v->type) {
  case RT_XSTR:
    anthy_free_xstr_str(&v->u.str);
    break;
  case RT_XSTRP:
  case RT_VAL:
  case RT_EMPTY:
  default:
    break;
  }
}

static void
do_set_nth_value(struct trie_node *node, int nth, int val)
{
  struct record_val *v = get_nth_val_ent(node, nth, 1);
  if (!v) {
    return ;
  }
  free_val_contents(v);
  v->type = RT_VAL;
  v->u.val = val;
}

static int
do_get_nth_value(struct trie_node *node, int n)
{
  struct record_val *v = get_nth_val_ent(node, n, 0);
  if (v && v->type == RT_VAL) {
    return v->u.val;
  }
  return 0;
}

static xstr*
intern_xstr (struct trie_root* xstrs, xstr* xs)
{
  struct trie_node* node;
  int dummy;

  if ((NULL == xs) || (NULL == xs->str) || (xs->len < 1) || (0 == xs->str[0])) {
    /* 辞書もしくは学習データが壊れていた時の対策 */
    return NULL;
  }
  node = trie_find(xstrs, xs);
  if (!node)
    node = trie_insert(xstrs, xs, 0, &dummy, &dummy);
  return &node->row.key;
}

static void
do_set_nth_xstr (struct trie_node *node, int nth, xstr *xs,
		 struct trie_root* xstrs)
{
  struct record_val *v = get_nth_val_ent(node, nth, 1);
  if (!v){
    return ;
  }
  free_val_contents(v);
  v->type = RT_XSTRP;
  v->u.strp = intern_xstr(xstrs, xs);
}

static void
do_truncate_row (struct trie_node* node, int n)
{
  int i;
  if (n < node->row.nr_vals) {
    for (i = n; i < node->row.nr_vals; i++) {
      free_val_contents(node->row.vals + i);
    }
    node->row.vals = realloc(node->row.vals,
				sizeof(struct record_val)* n);
    node->row.nr_vals = n;
  }
}

static void
do_remove_row (struct record_section* rsc,
		  struct trie_node* node)
{
  xstr* xs;
  xs = anthy_xstr_dup(&node->row.key);
  trie_remove(&rsc->cols, &node->row.key,
	      &rsc->lru_nr_used, &rsc->lru_nr_sused);

  anthy_free_xstr(xs);
}

static xstr *
do_get_nth_xstr(struct trie_node *node, int n)
{
  struct record_val *v = get_nth_val_ent(node, n, 0);
  if (v) {
    if (v->type == RT_XSTR) {
      return &v->u.str;
    } else if (v->type == RT_XSTRP) {
      return v->u.strp;
    }
  }
  return 0;
}

static void
lock_record (struct record_stat* rs)
{
  if (rs->is_anon) {
    return ;
  }
  anthy_priv_dic_lock();
}

static void
unlock_record (struct record_stat* rs)
{
  if (rs->is_anon) {
    return ;
  }
  anthy_priv_dic_unlock();
}

/* 再読み込みの必要があるかをチェックする
 * 必要があれば返り値が1になる */
static int
check_base_record_uptodate(struct record_stat *rst)
{
  struct stat st;
  if (rst->is_anon) {
    return 1;
  }
  anthy_check_user_dir();
  if (stat(rst->base_fn, &st) < 0) {
    return 0;
  } else if (st.st_mtime != rst->base_timestamp) {
    return 0;
  }
  return 1;
}


/*
 * row format:
 *  ROW := OPERATION SECTION KEY VALUE*
 *  OPERATION := "ADD"    (追加またはLRU更新)
 *               "DEL"    (削除)
 *  SECTION := (文字列)
 *  KEY     := TD
 *  VALUE   := TD
 *  TD      := TYPE DATA  (空白をあけずに書く)
 *  TYPE    := "S"        (xstr)
 *             "N"        (number)
 *  DATA    := (型ごとにシリアライズしたもの)
 */

static char*
read_1_token (FILE* fp, int* eol)
{
  int c;
  char* s;
  int in_quote;
  int len;

  in_quote = 0;
  s = NULL;
  len = 0;
  while (1) {
    c = fgetc(fp);
    switch (c) {
    case EOF: case '\n':
      goto out;
    case '\\':
      c = fgetc(fp);
      if (c == EOF || c == '\n') {
	goto out;
      }
      break;
    case '\"':
      in_quote = !in_quote;
      continue;
    case ' ': case '\t': case '\r':
      if (in_quote) {
	break;
      }
      if (s != NULL) {
	goto out;
      }
      break;
    default:
      break;
    }

    s = (char*) realloc(s, len + 2);
    s[len] = c;
    len ++;
  }
out:
  if (s) {
    s[len] = '\0';
  }
  *eol = (c == '\n') || (c == EOF);
  return s;
}

/* journalからADDの行を読む */
static void
read_add_row(FILE *fp, struct record_stat* rst,
	     struct record_section* rsc)
{
  int n;
  xstr* xs;
  char *token;
  int eol;
  struct trie_node* node;

  token = read_1_token(fp, &eol);
  if (!token) {
    return ;
  }

  xs = anthy_cstr_to_xstr(/* xstr 型を表す S を読み捨てる */
			  token + 1,
			  rst->encoding);
  node = do_select_row(rsc, xs, 1, LRU_USED);
  anthy_free_xstr(xs);
  free(token);

  if (node->dirty & PROTECT) {
    /* 保存すべき row なので、差分ファイルを読み捨てる */
    while (!eol) {
      free(read_1_token(fp, &eol));
    }
    return ;
  }

  n = 0;
  while (!eol) {
    token = read_1_token(fp, &eol);
    if (token) {
      switch(*token) {
	/* String 文字列 */
      case 'S':
	{
	  xstr* xs;
	  xs = anthy_cstr_to_xstr(token + 1, rst->encoding);
	  do_set_nth_xstr(node, n, xs, &rst->xstrs);
	  anthy_free_xstr(xs);
	}
	break;
	/* Number 数値 */
      case 'N':
	do_set_nth_value(node, n, atoi(token + 1));
	break;
      }
      free(token);
      n++;
    }
  }
  do_truncate_row(node, n);
}

/* journalからDELの行を読む */
static void
read_del_row(FILE *fp, struct record_stat* rst,
	     struct record_section* rsc)
{
  struct trie_node* node;
  char* token;
  xstr* xs;
  int eol;

  token = read_1_token(fp, &eol);
  if (!token) {
    return ;
  }

  xs = anthy_cstr_to_xstr(/* xstr 型を表す S を読み飛ばす */
			  token + 1,
			  rst->encoding);
  if ((node = do_select_row(rsc, xs, 0, 0)) != NULL) {
    do_remove_row(rsc, node);
  }
  anthy_free_xstr(xs);
  free(token);
}

/** 差分ファイルから1行読み込む */
static void
read_1_row(struct record_stat* rst, FILE* fp, char *op)
{
  char* sec_name;
  struct record_section* rsc;
  int eol;

  sec_name = read_1_token(fp, &eol);
  if (!sec_name || eol) {
    free(sec_name);
    return ;
  }
  rsc = do_select_section(rst, sec_name, 1);
  free(sec_name);
  if (!rsc) {
    return ;
  }

  if (strcmp(op, "ADD") == 0) {
    read_add_row(fp, rst, rsc);
  } else if (strcmp(op, "DEL") == 0) {
    read_del_row(fp, rst, rsc);
  }
}

/*
 * journal(差分)ファイルを読む
 */
static void
read_journal_record(struct record_stat* rs)
{
  FILE* fp;
  struct stat st;

  if (rs->is_anon) {
    return ;
  }
  fp = fopen(rs->journal_fn, "r");
  if (fp == NULL) {
    return;
  }
  if (fstat(fileno(fp), &st) == -1) {
    fclose(fp);
    return ;
  }
  errno = 0;
  if (st.st_size < rs->last_update) {

    /* ファイルサイズが小さくなっているので、
     * 最初から読み込む */
    if (fseek(fp, 0, SEEK_SET)) {
      anthy_log(0, "Failed fseek in %s:%d: %s\n",
                __FILE__, __LINE__, anthy_strerror(errno));
    }
  } else {
    if (fseek(fp, rs->last_update, SEEK_SET)) {
      anthy_log(0, "Failed fseek in %s:%d: %s\n",
                __FILE__, __LINE__, anthy_strerror(errno));
    }
  }
  rs->journal_timestamp = st.st_mtime;
  while (!feof(fp)) {
    char *op;
    int eol;
    op = read_1_token(fp, &eol);
    if (op && !eol) {
      read_1_row(rs, fp, op);
    }
    free(op);
  }
  rs->last_update = ftell(fp);
  fclose(fp);
}

static void
write_string(FILE* fp, const char* str)
{
  fprintf(fp, "%s", str);
}

/* ダブルクオートもしくはバックスラッシュにバックスラッシュを付ける */
static void
write_quote_string(FILE* fp, const char* str)
{
  const char* p;

  for (p = str; *p; p++) {
    if (*p == '\"' || *p == '\\') {
      fputc('\\', fp);
    }
    fputc(*p, fp);
  }
}

static void
write_quote_xstr(FILE* fp, xstr* xs, int encoding)
{
  char* buf;

  if ((NULL == xs) || (NULL == xs->str) || (xs->len < 1) || (0 == xs->str[0])) {
    /* 辞書もしくは学習データが壊れていた時の対策 */
    return;
  }

  buf = (char*) alloca(xs->len * 6 + 2); /* EUC またはUTF8を仮定 */
  anthy_sputxstr(buf, xs, encoding);
  write_quote_string(fp, buf);
}

static void
write_number(FILE* fp, int x)
{
  fprintf(fp, "%d", x);
}

/* journalに1行追記する */
static void
commit_add_row(struct record_stat* rst,
	       const char* sname, struct trie_node* node)
{
  FILE* fp;
  int i;

  fp = fopen(rst->journal_fn, "a");
  if (fp == NULL) {
    return;
  }

  write_string(fp, "ADD \"");
  write_quote_string(fp, sname);
  write_string(fp, "\" S\"");
  write_quote_xstr(fp, &node->row.key, rst->encoding);
  write_string(fp, "\"");

  for (i = 0; i < node->row.nr_vals; i++) {
    switch (node->row.vals[i].type) {
    case RT_EMPTY:
      write_string(fp, " E");
      break;
    case RT_VAL:
      write_string(fp, " N");
      write_number(fp, node->row.vals[i].u.val);
      break;
    case RT_XSTR:
      write_string(fp, " S\"");
      write_quote_xstr(fp, &node->row.vals[i].u.str, rst->encoding);
      write_string(fp, "\"");
      break;
    case RT_XSTRP:
      write_string(fp, " S\"");
      write_quote_xstr(fp, node->row.vals[i].u.strp, rst->encoding);
      write_string(fp, "\"");
      break;
    }
  }
  write_string(fp, "\n");
  rst->last_update = ftell(fp);
  fclose(fp);
}

/* 全ての row を解放する */
static void
clear_record(struct record_stat* rst)
{
  struct record_section *rsc;
  for (rsc = rst->section_list.next; rsc; rsc = rsc->next) {
    trie_remove_all(&rsc->cols, &rsc->lru_nr_used, &rsc->lru_nr_sused);
  }
  rst->cur_row = NULL;
}

/* 基本ファイルを読む */
static void
read_session(struct record_stat *rst)
{
  char **tokens;
  int nr;
  int in_section = 0;
  while (!anthy_read_line(&tokens, &nr)) {
    xstr *xs;
    int i;
    int dirty = 0;
    struct trie_node* node;

    if (!strcmp(tokens[0], "---") && nr > 1) {
      /* セクションの切れ目 */
      in_section = 1;
      rst->cur_section = do_select_section(rst, tokens[1], 1);
      goto end;
    }
    if (!in_section || nr < 2) {
      /* セクションが始まってない or 行が不完全 */
      goto end;
    }
    /* 行頭のLRUのマークを読む */
    if (tokens[0][0] == '-') {
      dirty = 0;
    } else if (tokens[0][0] == '+') {
      dirty = LRU_SUSED;
    }
    /* 次にindex */
    xs = anthy_cstr_to_xstr(&tokens[0][1], rst->encoding);
    node = do_select_row(rst->cur_section, xs, 1, dirty);
    anthy_free_xstr(xs);
    if (!node) {
      goto end;
    }
    rst->cur_row = node;
    /**/
    for (i = 1; i < nr; i++) {
      if (tokens[i][0] == '"') {
	char *str;
	str = strdup(&tokens[i][1]);
	str[strlen(str) - 1] = 0;
	xs = anthy_cstr_to_xstr(str, rst->encoding);
	free(str);
	do_set_nth_xstr(rst->cur_row, i-1, xs, &rst->xstrs);
	anthy_free_xstr(xs);
      }else if (tokens[i][0] == '*') {
	/* EMPTY entry */
	get_nth_val_ent(rst->cur_row, i-1, 1);
      } else {
	do_set_nth_value(rst->cur_row, i-1, atoi(tokens[i]));
      }
    }
  end:
    anthy_free_line();
  }
}

/* いまのデータベースを解放した後にファイルから読み込む */
static void
read_base_record(struct record_stat *rst)
{
  struct stat st;
  if (rst->is_anon) {
    clear_record(rst);
    return ;
  }
  anthy_check_user_dir();

  if (anthy_open_file(rst->base_fn) == -1) {
    return ;
  }

  clear_record(rst);
  read_session(rst);
  anthy_close_file();
  if (stat(rst->base_fn, &st) == 0) {
    rst->base_timestamp = st.st_mtime;
  }
  rst->last_update = 0;
}

static FILE *
open_tmp_in_recorddir(void)
{
  char *pn;
  const char *hd;
  const char *sid;
  sid = anthy_conf_get_str("SESSION-ID");
  hd = anthy_get_user_dir(0);
  pn = alloca(strlen(hd)+strlen(sid) + 2);
  sprintf(pn, "%s/%s", hd, sid);
  return fopen(pn, "w");
}

/*
 * 一時ファイルからbaseファイルへrenameする
 */
static void
update_file(const char *fn)
{
  const char *hd;
  char *tmp_fn;
  const char *sid;
  hd = anthy_get_user_dir(0);
  sid = anthy_conf_get_str("SESSION-ID");
  tmp_fn = alloca(strlen(hd)+strlen(sid) + 2);

  sprintf(tmp_fn, "%s/%s", hd, sid);
  if (rename(tmp_fn, fn)){
    anthy_log(0, "Failed to update record file %s -> %s.\n", tmp_fn, fn);
  }
}

/* カラムを保存する */
static void
save_a_row(FILE *fp, struct record_stat* rst,
	   struct record_row *c, int dirty)
{
  int i;
  char *buf = alloca(c->key.len * 6 + 2);
  /* LRUのマークを出力 */
  if (dirty == 0) {
    fputc('-', fp);
  } else {
    fputc('+', fp);
  }
  anthy_sputxstr(buf, &c->key, rst->encoding);
  /* index を出力 */
  fprintf(fp, "%s ", buf);
  /**/
  for (i = 0; i < c->nr_vals; i++) {
    struct record_val *val = &c->vals[i];
    switch (val->type) {
    case RT_EMPTY:
      fprintf(fp, "* ");
      break;
    case RT_XSTR:
      /* should not happen */
      fprintf(fp, "\"");
      write_quote_xstr(fp, &val->u.str, rst->encoding);
      fprintf(fp, "\" ");
      abort();
      break;
    case RT_XSTRP:
      fprintf(fp, "\"");
      write_quote_xstr(fp, val->u.strp, rst->encoding);
      fprintf(fp, "\" ");
      break;
    case RT_VAL:
      fprintf(fp, "%d ", val->u.val);
      break;
    default:
      anthy_log(0, "Failed to save an unkonwn record. (in record.c)\n");
      break;
    }
  }
  fprintf(fp, "\n");
}

static void
update_base_record(struct record_stat* rst)
{
  struct record_section *sec;
  struct trie_node *col;
  FILE *fp;
  struct stat st;

  /* 一時ファイルを作ってrecordを書き出す */
  anthy_check_user_dir();
  fp = open_tmp_in_recorddir();
  if (!fp) {
    anthy_log(0, "Failed to open temporaly session file.\n");
    return ;
  }
  /* 各セクションに対して */
  for (sec = rst->section_list.next;
       sec; sec = sec->next) {
    if (!trie_first(&sec->cols)) {
      /*このセクションは空*/
      continue;
    }
    /* セクション境界の文字列 */
    fprintf(fp, "--- %s\n", sec->name);
    /* 各カラムを保存する */
    for (col = trie_first(&sec->cols); col;
	 col = trie_next(&sec->cols, col)) {
      save_a_row(fp, rst, &col->row, col->dirty);
    }
  }
  fclose(fp);

  /* 本来の名前にrenameする */
  update_file(rst->base_fn);

  if (stat(rst->base_fn, &st) == 0) {
    rst->base_timestamp = st.st_mtime;
  }
  /* journalファイルを消す */
  unlink(rst->journal_fn);
  rst->last_update = 0;
}

static void
commit_del_row(struct record_stat* rst,
	       const char* sname, struct trie_node* node)
{
  FILE* fp;

  fp = fopen(rst->journal_fn, "a");
  if (fp == NULL) {
    return;
  }
  write_string(fp, "DEL \"");
  write_quote_string(fp, sname);
  write_string(fp, "\" S\"");
  write_quote_xstr(fp, &node->row.key, rst->encoding);
  write_string(fp, "\"");
  write_string(fp, "\n");
  fclose(fp);
}

/*
 * sync_add: ADD の書き込み
 * sync_del_and_del: DEL の書き込みと削除
 *   どちらも書き込みの前に、他のプロセスによってディスク上に保存された
 *   更新をメモリ上に読み込む。
 *   このとき、データベースをフラッシュする可能性もある。データベースの
 *   フラッシュがあると、 cur_row と全ての xstr は無効になる。
 *   ただし、 cur_section の有効性は保存される。
 */
static void
sync_add(struct record_stat* rst, struct record_section* rsc,
	 struct trie_node* node)
{
  lock_record(rst);
  if (check_base_record_uptodate(rst)) {
    node->dirty |= PROTECT;
    /* 差分ファイルだけ読む */
    read_journal_record(rst);
    node->dirty &= ~PROTECT;
    commit_add_row(rst, rsc->name, node);
  } else {
    /* 再読み込み */
    commit_add_row(rst, rsc->name, node);
    read_base_record(rst);
    read_journal_record(rst);
  }
  if (rst->last_update > FILE2_LIMIT) {
    update_base_record(rst);
  }
  unlock_record(rst);
}

static void
sync_del_and_del(struct record_stat* rst, struct record_section* rsc,
		 struct trie_node* node)
{
  lock_record(rst);
  commit_del_row(rst, rsc->name, node);
  if (!check_base_record_uptodate(rst)) {
    read_base_record(rst);
  }
  read_journal_record(rst);
  if (rst->last_update > FILE2_LIMIT) {
    update_base_record(rst);
  }
  unlock_record(rst);
}


/*
 * prediction関係
 */

static int
read_prediction_node(struct trie_node *n, struct prediction_t* predictions, int index)
{
  int i;
  int nr_predictions = do_get_nr_values(n);
  for (i = 0; i < nr_predictions; i += 2) {
    time_t t = do_get_nth_value(n, i);
    xstr* xs = do_get_nth_xstr(n, i + 1);
    if (t && xs) {
      if (predictions) {
	predictions[index].timestamp = t;
	predictions[index].src_str = anthy_xstr_dup(&n->row.key);
	predictions[index].str = anthy_xstr_dup(xs);
      }
      ++index;
    }
  }
  return index;
}


/*
 * trie中をたどり、prefixがマッチしたらread_prediction_nodeを
 * 呼んでpredictionsの配列に結果を追加する。
 */
static int
traverse_record_for_prediction(xstr* key, struct trie_node *n,
			       struct prediction_t* predictions, int index)
{
  if (n->l->bit > n->bit) {
    index = traverse_record_for_prediction(key, n->l, predictions, index);
  } else {
    if (n->l->row.key.len != -1) {
      if (anthy_xstrncmp(&n->l->row.key, key, key->len) == 0) {
	index = read_prediction_node(n->l, predictions, index);
      }
    }
  }
  if (n->r->bit > n->bit) {
    index = traverse_record_for_prediction(key, n->r, predictions, index);
  } else {
    if (n->r->row.key.len != -1) {
      if (anthy_xstrncmp(&n->r->row.key, key, key->len) == 0) {
	index = read_prediction_node(n->r, predictions, index);
      }
    }
  }
  return index;
}

/*
 * key で探索
 * key の文字列長を越えるか、ノードが無くなったら探索打ち切り
 * trieのkeyが格納されているところでななくて葉を返す
 */
static struct trie_node *
trie_find_for_prediction (struct trie_root* root, xstr *key)
{
  struct trie_node *p;
  struct trie_node *q;

  p = &root->root;
  q = p->l;

  while (p->bit < q->bit) {
    if (q->bit >= 2) {
      if ((q->bit - 2) / (int)(sizeof(xchar) << 3) >= key->len) {
	break;
      }
    }
    p = q;
    q = trie_key_nth_bit(key, p->bit) ? p->r : p->l;
  }
  return p;
}

static int
prediction_cmp(const void* lhs, const void* rhs)
{
  struct prediction_t *lpre = (struct prediction_t*)lhs;
  struct prediction_t *rpre = (struct prediction_t*)rhs;
  return rpre->timestamp - lpre->timestamp;
}

int
anthy_traverse_record_for_prediction(xstr* key, struct prediction_t* predictions)
{
  struct trie_node* mark;
  int nr_predictions;
  if (anthy_select_section("PREDICTION", 0)) {
    return 0;
  }

  /* 指定された文字列をprefixに持つnodeを探す */
  mark = trie_find_for_prediction(&anthy_current_record->cur_section->cols, key);
  if (!mark) {
    return 0;
  }
  nr_predictions = traverse_record_for_prediction(key, mark, predictions, 0);
  if (predictions) {
    /* タイムスタンプで予測候補をソートする */
    qsort(predictions, nr_predictions, sizeof(struct prediction_t), prediction_cmp);
  }
  return nr_predictions;
}

/* Wrappers begin.. */
int
anthy_select_section(const char *name, int flag)
{
  struct record_stat* rst;
  struct record_section* rsc;

  rst = anthy_current_record;
  if (rst->row_dirty && rst->cur_section && rst->cur_row) {
    sync_add(rst, rst->cur_section, rst->cur_row);
  }
  rst->cur_row = NULL;
  rst->row_dirty = 0;
  rsc = do_select_section(rst, name, flag);
  if (!rsc) {
    return -1;
  }
  rst->cur_section = rsc;
  return 0;
}

int
anthy_select_row(xstr *name, int flag)
{
  struct record_stat* rst;
  struct trie_node* node;

  rst = anthy_current_record;
  if (!rst->cur_section) {
    return -1;
  }
  if (rst->row_dirty && rst->cur_row) {
    sync_add(rst, rst->cur_section, rst->cur_row);
    rst->row_dirty = 0;
  }
  node = do_select_row(rst->cur_section, name, flag, LRU_USED);
  if (!node) {
    return -1;
  }
  rst->cur_row = node;
  rst->row_dirty = flag;
  return 0;
}

int
anthy_select_longest_row(xstr *name)
{
  struct record_stat* rst;
  struct trie_node* node;

  rst = anthy_current_record;
  if (!rst->cur_section)
    return -1;

  if (rst->row_dirty && rst->cur_row) {
    sync_add(rst, rst->cur_section, rst->cur_row);
    rst->row_dirty = 0;
  }
  node = do_select_longest_row(rst->cur_section, name);
  if (!node) {
    return -1;
  }

  rst->cur_row = node;
  rst->row_dirty = 0;
  return 0;
}

void
anthy_truncate_section(int count)
{
  do_truncate_section(anthy_current_record, count);
}

void
anthy_truncate_row(int nth)
{
  struct trie_node *cur_row = anthy_current_record->cur_row;
  if (!cur_row) {
    return ;
  }
  do_truncate_row(cur_row, nth);

}

int
anthy_mark_row_used(void)
{
  struct record_stat* rst = anthy_current_record;
  if (!rst->cur_row) {
    return -1;
  }

  do_mark_row_used(rst->cur_section, rst->cur_row);
  sync_add(rst, rst->cur_section, rst->cur_row);
  rst->row_dirty = 0;
  return 0;
}

void
anthy_set_nth_value(int nth, int val)
{
  struct record_stat* rst;

  rst = anthy_current_record;
  if (!rst->cur_row) {
    return;
  }
  do_set_nth_value(rst->cur_row, nth, val);
  rst->row_dirty = 1;
}

void
anthy_set_nth_xstr(int nth, xstr *xs)
{
  struct record_stat* rst = anthy_current_record;
  if (!rst->cur_row) {
    return;
  }
  do_set_nth_xstr(rst->cur_row, nth, xs, &rst->xstrs);
  rst->row_dirty = 1;
}

int
anthy_get_nr_values(void)
{
  return do_get_nr_values(anthy_current_record->cur_row);
}

int
anthy_get_nth_value(int n)
{
  return do_get_nth_value(anthy_current_record->cur_row, n);
}

xstr *
anthy_get_nth_xstr(int n)
{
  return do_get_nth_xstr(anthy_current_record->cur_row, n);
}

int
anthy_select_first_row(void)
{
  struct record_stat* rst;
  struct trie_node* node;

  rst = anthy_current_record;
  if (!rst->cur_section)
    return -1;

  if (rst->row_dirty && rst->cur_row) {
    sync_add(rst, rst->cur_section, rst->cur_row);
    rst->row_dirty = 0;
  }
  node = do_select_first_row(rst->cur_section);
  if (!node) {
    return -1;
  }
  rst->cur_row = node;
  rst->row_dirty = 0;
  return 0;
}

int
anthy_select_next_row(void)
{
  struct record_stat* rst;
  struct trie_node* node;

  rst = anthy_current_record;
  if (!rst->cur_section || !rst->cur_row)
    return -1;

  /* sync_add() で cur_row が無効になることがあるので、
   * たとえ row_dirty でも sync_add() しない
   */
  rst->row_dirty = 0;
  node = do_select_next_row(rst->cur_section, rst->cur_row);
  if (!node)
    return -1;
  rst->cur_row = node;
  rst->row_dirty = 0;
  return 0;
}

xstr *
anthy_get_index_xstr(void)
{
  return do_get_index_xstr(anthy_current_record);
}
/*..Wrappers end*/

/*
 * trie_row_init は何回よんでもいい
 */
static void
trie_row_init(struct record_row* rc)
{
  rc->nr_vals = 0;
  rc->vals = NULL;
}

static void
trie_row_free(struct record_row *rc)
{
  int i;
  for (i = 0; i < rc->nr_vals; i++)
    free_val_contents(rc->vals + i);
  free(rc->vals);
  free(rc->key.str);
}

/* あるセクションのデータを全て解放する */
static void
free_section(struct record_stat *r, struct record_section *rs)
{
  struct record_section *s;
  trie_remove_all(&rs->cols, &rs->lru_nr_used, &rs->lru_nr_sused);
  if (r->cur_section == rs) {
    r->cur_row = 0;
    r->cur_section = 0;
  }
  for (s = &r->section_list; s && s->next; s = s->next) {
    if (s->next == rs) {
      s->next = s->next->next;
    }
  }
  if (rs->name){
    free((void *)rs->name);
  }
  free(rs);
}

/* すべてのデータを解放する */
static void
free_record(struct record_stat *rst)
{
  struct record_section *rsc;
  for (rsc = rst->section_list.next; rsc; ){
    struct record_section *tmp;
    tmp = rsc;
    rsc = rsc->next;
    free_section(rst, tmp);
  }
  rst->section_list.next = NULL;
}

void
anthy_release_section(void)
{
  struct record_stat* rst;

  rst = anthy_current_record;
  if (!rst->cur_section) {
    return ;
  }
  free_section(rst, rst->cur_section);
  rst->cur_section = 0;
}

void
anthy_release_row(void)
{
  struct record_stat* rst;

  rst = anthy_current_record;
  if (!rst->cur_section || !rst->cur_row) {
    return;
  }

  rst->row_dirty = 0;
  /* sync_del_and_del で削除もする */
  sync_del_and_del(rst, rst->cur_section, rst->cur_row);
  rst->cur_row = NULL;
}

static void
check_record_encoding(struct record_stat *rst)
{
  FILE *fp;
  if (anthy_open_file(rst->base_fn) == 0) {
    /* EUCの履歴ファイルがあった */
    anthy_close_file();
    return ;
  }
  fp = fopen(rst->journal_fn, "r");
  if (fp) {
    /* EUCの差分ファイルがあった */
    fclose(fp);
    return ;
  }
  rst->encoding = ANTHY_UTF8_ENCODING;
  strcat(rst->base_fn, ENCODING_SUFFIX);
  strcat(rst->journal_fn, ENCODING_SUFFIX);
}

static void
record_dtor(void *p)
{
  int dummy;
  struct record_stat *rst = (struct record_stat*) p;
  free_record(rst);
  if (rst->id) {
    free(rst->base_fn);
    free(rst->journal_fn);
  }
  trie_remove_all(&rst->xstrs, &dummy, &dummy);
}

void
anthy_reload_record(void)
{
  struct stat st;
  struct record_stat *rst = anthy_current_record;

  if (stat(rst->journal_fn, &st) == 0 &&
      rst->journal_timestamp == st.st_mtime) {
    return ;
  }

  lock_record(rst);
  read_base_record(rst);
  read_journal_record(rst);
  unlock_record(rst);
}

void
anthy_init_record(void)
{
  record_ator = anthy_create_allocator(sizeof(struct record_stat),
				       record_dtor);
}

static void
setup_filenames(const char *id, struct record_stat *rst)
{
  const char *home = anthy_get_user_dir(0);
  int base_len = strlen (home) + strlen (id) + strlen (ENCODING_SUFFIX) + 1;

  /* 基本ファイル */
  rst->base_fn = (char*) malloc(base_len +
				strlen("/last-record1_"));
  if (!rst->base_fn) {
      anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
      return;
  }
  sprintf(rst->base_fn, "%s/last-record1_%s",
	  home, id);
  /* 差分ファイル */
  rst->journal_fn = (char*) malloc(base_len +
				   strlen("/last-record2_"));
  if (!rst->journal_fn) {
      anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
      return;
  }
  sprintf(rst->journal_fn, "%s/last-record2_%s",
	  home, id);
}

struct record_stat *
anthy_create_record(const char *id)
{
  struct record_stat *rst;

  if (!id) {
    return NULL;
  }

  rst = anthy_smalloc(record_ator);
  rst->id = id;
  rst->section_list.next = 0;
  init_trie_root(&rst->xstrs);
  rst->cur_section = 0;
  rst->cur_row = 0;
  rst->row_dirty = 0;
  rst->encoding = 0;

  /* ファイル名の文字列を作る */
  setup_filenames(id, rst);

  rst->last_update = 0;

  if (!strcmp(id, ANON_ID)) {
    rst->is_anon = 1;
  } else {
    rst->is_anon = 0;
    anthy_check_user_dir();
  }

  /* ファイルから読み込む */
  lock_record(rst);
  check_record_encoding(rst);
  read_base_record(rst);
  read_journal_record(rst);
  unlock_record(rst);

  return rst;
}

void
anthy_release_record(struct record_stat *rs)
{
  anthy_sfree(record_ator, rs);
}
