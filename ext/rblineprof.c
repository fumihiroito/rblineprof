#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <ruby.h>
#include <node.h>
#include <intern.h>
#include <st.h>
#include <re.h>

static VALUE gc_hook;

typedef struct {
  char *filename;
  uint64_t *lines;
  long nlines;

  uint64_t last_time;
  long last_line;
} sourcefile_t;

static struct {
  bool enabled;

  // single file mode, store filename and line data directly
  char *source_filename;
  sourcefile_t file;

  // regex mode, store file data in hash table
  VALUE source_regex;
  st_table *files;
  sourcefile_t *last_file;
}
rblineprof = {
  .enabled = false,
  .source_filename = NULL,
  .source_regex = Qfalse,
  .files = NULL,
  .last_file = NULL
};

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec*1e6 +
         (uint64_t)tv.tv_usec;
}

static void
profiler_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  sourcefile_t *sourcefile = NULL;

  char *file = node->nd_file;
  long line  = nd_line(node);

  if (!file) return;
  if (line <= 0) return;

  if (rblineprof.source_filename) { // single file mode
    if (rblineprof.source_filename == file) {
      sourcefile = &rblineprof.file;
      sourcefile->filename = file;
    } else {
      return;
    }

  } else { // regex mode
    st_lookup(rblineprof.files, (st_data_t)file, (st_data_t *)&sourcefile);

    if ((VALUE)sourcefile == Qnil) // known negative match, skip
      return;

    if (!sourcefile) { // unknown file, check against regex
      if (rb_reg_search(rblineprof.source_regex, rb_str_new2(file), 0, 0) >= 0) {
        sourcefile = ALLOC_N(sourcefile_t, 1);
        MEMZERO(sourcefile, sourcefile_t, 1);
        sourcefile->filename = strdup(file);
        st_insert(rblineprof.files, (st_data_t)sourcefile->filename, (st_data_t)sourcefile);
      } else { // no match, insert Qnil to prevent regex next time
        st_insert(rblineprof.files, (st_data_t)strdup(file), (st_data_t)Qnil);
        return;
      }
    }
  }

  if (sourcefile) {
    uint64_t now = timeofday_usec();

    if (sourcefile->last_time) {
      /* allocate space for per-line data the first time */
      if (sourcefile->lines == NULL) {
        sourcefile->nlines = sourcefile->last_line + 100;
        sourcefile->lines = ALLOC_N(uint64_t, sourcefile->nlines);
        MEMZERO(sourcefile->lines, uint64_t, sourcefile->nlines);
      }

      /* grow the per-line array if necessary */
      if (sourcefile->last_line >= sourcefile->nlines) {
        long prev_nlines = sourcefile->nlines;
        sourcefile->nlines = sourcefile->last_line + 100;

        REALLOC_N(sourcefile->lines, uint64_t, sourcefile->nlines);
        MEMZERO(sourcefile->lines + prev_nlines, uint64_t, sourcefile->nlines - prev_nlines);
      }

      /* record the sample */
      sourcefile->lines[sourcefile->last_line] += (now - sourcefile->last_time);
    }

    sourcefile->last_time = now;
    sourcefile->last_line = line;

    if (rblineprof.last_file && rblineprof.last_file != sourcefile)
      rblineprof.last_file->last_time = 0;

    rblineprof.last_file = sourcefile;
  }
}

static int
cleanup_files(st_data_t key, st_data_t record, st_data_t arg)
{
  xfree((char *)key);

  sourcefile_t *sourcefile = (sourcefile_t*)record;
  if (!sourcefile || (VALUE)sourcefile == Qnil) return ST_DELETE;

  if (sourcefile->lines)
    xfree(sourcefile->lines);
  xfree(sourcefile);

  return ST_DELETE;
}

static int
summarize_files(st_data_t key, st_data_t record, st_data_t arg)
{
  sourcefile_t *sourcefile = (sourcefile_t*)record;
  if (!sourcefile || (VALUE)sourcefile == Qnil) return ST_CONTINUE;

  VALUE ret = (VALUE)arg;
  VALUE ary = rb_ary_new();
  long i;

  for (i=0; i<sourcefile->nlines; i++)
    rb_ary_store(ary, i, ULL2NUM(sourcefile->lines[i]));

  rb_hash_aset(ret, rb_str_new2(sourcefile->filename), ary);

  return ST_CONTINUE;
}

static VALUE
lineprof_ensure(VALUE self)
{
  rb_remove_event_hook(profiler_hook);
  rblineprof.enabled = false;
}

VALUE
lineprof(VALUE self, VALUE filename)
{
  if (!rb_block_given_p())
    rb_raise(rb_eArgError, "block required");

  if (rblineprof.enabled)
    rb_raise(rb_eArgError, "profiler is already enabled");

  VALUE filename_class = rb_obj_class(filename);

  if (filename_class == rb_cString) {
    rblineprof.source_filename = rb_source_filename(StringValuePtr(filename));
  } else if (filename_class == rb_cRegexp) {
    rblineprof.source_regex = filename;
    rblineprof.source_filename = NULL;
  } else {
    rb_raise(rb_eArgError, "argument must be String or Regexp");
  }

  // cleanup
  rblineprof.last_file = NULL;
  st_foreach(rblineprof.files, cleanup_files, 0);
  if (rblineprof.file.lines) {
    xfree(rblineprof.file.lines);
    rblineprof.file.lines = NULL;
    rblineprof.file.nlines = 0;
  }

  rblineprof.enabled = true;
  rb_add_event_hook(profiler_hook, RUBY_EVENT_LINE);
  rb_ensure(rb_yield, Qnil, lineprof_ensure, self);

  VALUE ret = rb_hash_new();
  VALUE ary = Qnil;

  if (rblineprof.source_filename) {
    long i;
    ary = rb_ary_new();
    for (i=0; i<rblineprof.file.nlines; i++)
      rb_ary_store(ary, i, ULL2NUM(rblineprof.file.lines[i]));
    rb_hash_aset(ret, rb_str_new2(rblineprof.source_filename), ary);
  } else {
    st_foreach(rblineprof.files, summarize_files, ret);
  }

  return ret;
}

static void
rblineprof_gc_mark()
{
  if (rblineprof.enabled)
    rb_gc_mark_maybe(rblineprof.source_regex);
}

void
Init_rblineprof()
{
  gc_hook = Data_Wrap_Struct(rb_cObject, rblineprof_gc_mark, NULL, NULL);
  rb_global_variable(&gc_hook);

  rblineprof.files = st_init_strtable();
  rb_define_global_function("lineprof", lineprof, 1);
}
