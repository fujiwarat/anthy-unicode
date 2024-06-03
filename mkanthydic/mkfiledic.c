/*
 * ファイルをまとめて辞書ファイルを生成する
 *
 * デフォルトではひとつ上のディレクトリ「..」に各ファイルの
 * パス名を付けるが、このコマンドに対する -p オプションで
 * 変更することができる。
 *
 * entry_num個のファイルに対して
 *  0: entry_num ファイルの個数
 *  1: 各ファイルの情報
 *    n * 3    : name_offset
 *    n * 3 + 1: strlen(key)
 *    n * 3 + 2: contents_offset
 *  [name_of_section]*entry_num
 *   : 各ファイルの名前
 *  [file]*entry_num
 *   : 各ファイルの内容
 *
 * Copyright (C) 2005-2006 YOSHIDA Yuichi
 * Copyright (C) 2006-2007 TABATA Yusuke
 * Copyright (C) 2021 Takao Fujiwara <takao.fujiwara1@gmail.com>
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <anthy/diclib.h>
#include <anthy/logger.h>
#include <anthy/xstr.h>

#define SECTION_ALIGNMENT 64
#define DIC_NAME "anthy.dic"

struct header_entry {
  const char* key;
  const char* file_name;
};

static void
write_nl(FILE* fp, int i)
{
  i = anthy_dic_htonl(i);
  fwrite(&i, sizeof(int), 1, fp);
}


/** ファイルのサイズを取得する */
static int
get_file_size(const char* fn)
{
  struct stat st;
  if (!fn)
    return -1;
  if (stat(fn, &st) < 0)
    return -1;
  return (st.st_size + SECTION_ALIGNMENT - 1) & (-SECTION_ALIGNMENT);
}

static char *
get_file_name(const char *prefix, struct header_entry* entry)
{
  char *fn = malloc(strlen(prefix) + strlen(entry->file_name) + 4);
  if (!fn) {
    anthy_log(0, "Failed malloc in %s:%d\n", __FILE__, __LINE__);
    return NULL;
  }
  sprintf(fn, "%s/%s", prefix, entry->file_name);
  return fn;
}

static int
write_header(FILE* fp, const char *prefix,
	     int entry_num, struct header_entry* entries)
{
  int i;
  int name_offset;
  int contents_offset;

  name_offset = sizeof(int) * (1 + entry_num * 3);
  contents_offset = name_offset;

  for (i = 0; i < entry_num; ++i) {
    contents_offset += strlen(entries[i].key);
  }
  contents_offset =
    (contents_offset + SECTION_ALIGNMENT - 1) & (-SECTION_ALIGNMENT);

  /* ファイルの数 */
  write_nl(fp, entry_num);

  /* 各ファイルの場所を出力する */
  for (i = 0; i < entry_num; ++i) {
    char *fn = get_file_name(prefix, &entries[i]);
    int file_size = get_file_size(fn);
    if (file_size == -1) {
      fprintf(stderr, "failed to get file size of (%s).\n",
	      fn);
      free(fn);
      return -1;
    }
    free(fn);
    /**/
    write_nl(fp, name_offset);
    write_nl(fp, strlen(entries[i].key));
    write_nl(fp, contents_offset);
    /**/
    name_offset += strlen(entries[i].key);
    contents_offset += file_size;
  }

  /* 各ファイルの名前を出力する */
  for (i = 0; i < entry_num; ++i) {
    fprintf(fp, "%s", entries[i].key);
  }
  return 0;
}



static void
copy_file(FILE *in, FILE *out)
{
  int i;
  size_t nread;
  char buf[BUFSIZ];

  /* Pad OUT to the next aligned offset.  */
  for (i = ftell (out); i & (SECTION_ALIGNMENT - 1); i++) {
    fputc (0, out);
  }

  /* Copy the contents.  */
  rewind(in);
  while ((nread = fread (buf, 1, sizeof buf, in)) > 0) {
    if (fwrite (buf, 1, nread, out) < nread) {
      exit (1);
    }
  }
}

static void
write_contents(FILE* fp, const char *prefix,
	       int entry_num, struct header_entry* entries)
{
  int i;
  for (i = 0; i < entry_num; ++i) {
    FILE* in_fp;
    char *fn = get_file_name(prefix, &entries[i]);

    if (!fn)
      break;
    in_fp = fopen(fn, "r");
    if (in_fp == NULL) {
      printf("failed to open %s\n", fn);
      free(fn);
      break;
    }
    printf("  copying %s (%s)\n", fn, entries[i].key);
    free(fn);
    copy_file(in_fp, fp);
    fclose(in_fp);
  }
}


static void
create_file_dic(const char* fn, const char *prefix,
		int entry_num, struct header_entry* entries)
{
  FILE* fp = fopen(fn, "w");
  int res;
  if (!fp) {
    fprintf(stderr, "failed to open file dictionary file (%s).\n", fn);
    exit(1);
  }
  /* ヘッダを書き出す */
  res = write_header(fp, prefix, entry_num, entries);
  if (res) {
    exit(1);
  }

  /* ファイルの中身を書き出す */
  write_contents(fp, prefix, entry_num, entries);
  fclose(fp);
}


int
main(int argc, char* argv[])
{
  int i;
  const char *prefix = "..";
  const char *prev_arg = "";
  const char *dic_out  = DIC_NAME;

  struct header_entry entries[] = {
    {"word_dic", "/mkworddic/anthy.wdic"},
    {"dep_dic", "/depgraph/anthy.dep"},
    {"trans_info", "/calctrans/anthy.trans_info"},
    {"cand_info", "/calctrans/anthy.cand_info"},
    {"weak_words", "/calctrans/anthy.weak_words"},
    {"corpus_bucket", "/calctrans/anthy.corpus_bucket"},
    {"corpus_array", "/calctrans/anthy.corpus_array"},
  };

  for (i = 1; i < argc; i++) {
    if (!strcmp("-p", prev_arg)) {
      prefix = argv[i];
    }
    if (!strcmp("-o", prev_arg)) {
      dic_out = argv[i];
    }
    /**/
    prev_arg = argv[i];
  }
  printf("file name prefix=[%s] you can change this by -p option.\n", prefix);

  create_file_dic(dic_out, prefix,
		  sizeof(entries)/sizeof(struct header_entry),
		  entries);

  printf("%s done.\n", argv[0]);
  return 0;
}
