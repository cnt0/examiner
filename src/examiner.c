#include "examiner.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

const static double EPSILON = 0.0001;

static sigjmp_buf global_sig;
static exam_env_t global_env = {0};

static char *exam_concat_scope_name(const exam_test_t *test) {
  size_t buf_len = strlen(test->scope) + strlen(test->name) + 2;
  char *buf = malloc(sizeof(char) * buf_len);
  memset(buf, 0, buf_len);
  strcat(buf, test->scope);
  strcat(buf, ".");
  strcat(buf, test->name);

  return buf;
}

static bool exam_filter_test(const char *name, const char *filter) {
  int i = 0;
  while (true) {
    if (filter[i] == '\0') {
      break;
    }
    if (name[i] != filter[i]) {
      return false;
    }
    i++;
  }
  return true;
}

#define NONE global_env.color(0)
#define RED global_env.color(31)
#define GREEN global_env.color(32)
#define BLUE global_env.color(34)
#define GRAY global_env.color(90)

static const char *colored_matcher(int value) {
  switch (value) {
  case 0: return "\e[0m";
  case 31: return "\e[31m";
  case 32: return "\e[32m";
  case 34: return "\e[34m";
  case 90: return "\e[90m";
  }
  return "\e[0m";
}

static const char *non_colored_matcher(int value) {
  return "";
}

static void exam_print_running_all(size_t count) {
  if (!global_env.shortd) {
    printf("%s[==========] Running %zu test(s)%s\n", GRAY, count, NONE);
  }
}

static void exam_print_running_test(size_t len, const char *name) {
  if (!global_env.shortd) {
    printf("%s[==========] Running %zu test(s) in scope %s%s\n", GRAY, len,
           name, NONE);
  } else {
    printf("%*.*s: ", -global_env.longest_name_len, global_env.longest_name_len,
           name);
  }
}

static void exam_print_pending(const char *name) {
  if (!global_env.shortd) {
    printf("%s[ PENDING  ] %s%s\n", BLUE, NONE, name);
  } else {
    printf("%s%s", BLUE, NONE);
  }
}

static void exam_print_run(const char *name) {
  if (!global_env.shortd) {
    printf("%s[ RUN      ] %s%s\n", GRAY, NONE, name);
  }
}

static void exam_print_ok(const char *name, double diff) {
  if (!global_env.shortd) {
    printf("%s[       OK ] %s%s [%.2f s]\n", GREEN, NONE, name, diff);
  } else {
    printf("%s%s", GREEN, NONE);
  }
}

static void exam_print_failed(const char *name) {
  if (!global_env.shortd) {
    printf("%s[  FAILED  ] %s%s\n", RED, NONE, name);
  } else {
    printf("%s%s", RED, NONE);
  }
}

static void exam_print_passed_scope(size_t passed, const char *name) {
  if (!global_env.shortd) {
    printf("%s[  PASSED  ] %zu test(s) passed in scope %s%s\n", GREEN, passed,
           name, NONE);
  }
}

static void exam_print_passed_result(size_t passed) {
  if (!global_env.shortd) {
    printf("%s[  PASSED  ] %zu test(s) across all scopes%s\n", GREEN, passed,
           NONE);
  }
}

static void exam_print_final(size_t count) {
  if (!global_env.shortd) {
    printf("%s[==========] Ran %zu test(s) across all scopes%s\n", GRAY, count,
           NONE);
  }
}

static size_t *exam_create_shuffle(size_t n) {
  if (!global_env.shuffle) {
    return NULL;
  }

  size_t *array = malloc(sizeof(size_t) * global_env.tbl.len);
  for (size_t i = 0; i < n; ++i) {
    array[i] = i;
  }
  if (n > 1) {
    for (size_t i = 0; i < n - 1; i++) {
      size_t rnd = (size_t)rand();
      size_t j = i + rnd / (RAND_MAX / (n - i) + 1);
      size_t t = array[j];
      array[j] = array[i];
      array[i] = t;
    }
  }
  return array;
}

static void exam_free_shuffle(size_t *array) {
  if (array) {
    free(array);
  }
}

static void free_exam_env() {
  for (size_t i = 0; i < global_env.tbl.len; ++i) {
    if (global_env.tbl.scope[i].tests) {
      free(global_env.tbl.scope[i].tests);
    }
  }
  if (global_env.tbl.scope) {
    free(global_env.tbl.scope);
  }
}

void exam_init(int argc, char **argv) {
  srand((uint32_t)time(NULL));

  global_env.filter = NULL;
  global_env.color = &colored_matcher;
  global_env.longest_name_len = 0;
  global_env.repeat = 1;
  global_env.list = false;
  global_env.shuffle = false;
  global_env.shortd = false;
  global_env.die_on_fail = false;
  if (argc > 1) {
    for (size_t i = 1; i < argc; ++i) {
      if (strncmp(argv[i], "--list-tests", 12) == 0) {
        global_env.list = true;
      } else if (strncmp(argv[i], "--short", 7) == 0) {
        global_env.shortd = true;
      } else if (strncmp(argv[i], "--filter", 8) == 0) {
        if ((i + 1) == argc) {
          fprintf(stderr, "--filter requires a second parameter\n");
          free_exam_env();
          exit(1);
        }
        // TODO(conni2461): Allow to filter more than onces
        global_env.filter = argv[++i];
      } else if (strncmp(argv[i], "--shuffle", 9) == 0) {
        global_env.shuffle = true;
      } else if (strncmp(argv[i], "--repeat", 8) == 0) {
        if ((i + 1) == argc) {
          fprintf(stderr, "--repeat requires a second parameter\n");
          free_exam_env();
          exit(1);
        }
        global_env.repeat = atoi(argv[++i]);
        if (global_env.repeat == 0) {
          fprintf(stderr, "repeat is not a number or 0 is not a valid input. "
                          "Input has to be >= 1\n");
          free_exam_env();
          exit(1);
        }
      } else if (strncmp(argv[i], "--die-on-fail", 13) == 0) {
        global_env.die_on_fail = true;
      } else if (strncmp(argv[i], "--color", 7) == 0) {
        if ((i + 1) == argc) {
          fprintf(stderr, "--color requires a second parameter! on|off\n");
          free_exam_env();
          exit(1);
        }
        ++i;
        if (strncmp(argv[i], "on", 2) == 0) {
          global_env.color = &colored_matcher;
        } else if (strncmp(argv[i], "off", 3) == 0) {
          global_env.color = &non_colored_matcher;
        } else {
          fprintf(
              stderr,
              "second parameter passed to color is neither `on` or `off`\n");
          free_exam_env();
          exit(1);
        }
      } else if (strncmp(argv[i], "--help", 6) == 0 ||
                 strncmp(argv[i], "-h", 2) == 0) {
        printf("%s [options]\n"
               "  --list-tests      only list all tests\n"
               "  --short           short output\n"
               "  --filter [str]    filter for one or many tests (substr "
               "matching)\n"
               "  --shuffle         shuffle test execution order\n"
               "  --repeat [n]      repeat all tests n times\n"
               "  --die-on-fail     stop execution on failure\n"
               "\n"
               "  --color [on, off] color output. Default: on\n"
               "\n"
               "  -h | --help       print help page\n"
               "  -v | --version    print software version\n",
               argv[0]);
        free_exam_env();
        exit(0);
      } else if (strncmp(argv[i], "--version", 8) == 0 ||
                 strncmp(argv[i], "-v", 2) == 0) {
        printf("Version 0.1 License MIT (conni2461)\n");
        free_exam_env();
        exit(0);
      } else {
        fprintf(stderr, "Option %s not found! See -h for supported options",
                argv[i]);
        free_exam_env();
        exit(1);
      }
    }
  }
}

int exam_run() {
  int retValue = 0;
  size_t count = 0, passed = 0;
  for (size_t i = 0; i < global_env.tbl.len; ++i) {
    size_t scope_name_len = strlen(global_env.tbl.scope[i].name);
    if (scope_name_len > global_env.longest_name_len) {
      global_env.longest_name_len = (int32_t)scope_name_len;
    }
    if (global_env.filter != NULL || global_env.list) {
      for (size_t j = 0; j < global_env.tbl.scope[i].len; ++j) {
        char *buf = exam_concat_scope_name(&global_env.tbl.scope[i].tests[j]);
        if (global_env.filter != NULL) {
          if (exam_filter_test(buf, global_env.filter)) {
            if (global_env.list) {
              printf("%s\n", buf);
            }
            ++count;
          }
        } else {
          printf("%s\n", buf);
        }
        free(buf);
      }
    } else {
      count += global_env.tbl.scope[i].len;
    }
  }

  if (global_env.list) {
    free_exam_env();
    exit(0);
  }

  size_t *scopes_indices = exam_create_shuffle(global_env.tbl.len);
  exam_print_running_all(count);
  for (size_t i = 0; i < global_env.tbl.len; ++i) {
    size_t ii = i;
    if (global_env.shuffle) {
      ii = scopes_indices[i];
    }
    bool printed_scope = false;
    size_t scope_passed = 0;
    size_t *tests_indices = exam_create_shuffle(global_env.tbl.scope[ii].len);
    for (size_t j = 0; j < global_env.tbl.scope[ii].len; ++j) {
      exam_test_t *current_test = NULL;
      if (global_env.shuffle) {
        current_test = &global_env.tbl.scope[ii].tests[tests_indices[j]];
      } else {
        current_test = &global_env.tbl.scope[ii].tests[j];
      }
      char *buf = exam_concat_scope_name(current_test);
      if (global_env.filter != NULL &&
          !exam_filter_test(buf, global_env.filter)) {
        free(buf);
        continue;
      }
      if (!printed_scope) {
        exam_print_running_test(global_env.tbl.scope[ii].len,
                                global_env.tbl.scope[ii].name);
        printed_scope = true;
      }
      if (current_test->pending) {
        exam_print_pending(buf);
        free(buf);
        continue;
      }
      if (sigsetjmp(global_sig, 1) == 0) {
        exam_print_run(buf);
        double diff;
        for (size_t k = 0; k < global_env.repeat; k++) {
          if (global_env.tbl.scope[ii].before != NULL) {
            global_env.tbl.scope[ii].before();
          }
          clock_t start = clock();
          current_test->fn();
          clock_t end = clock();
          diff = ((double)(end - start)) / CLOCKS_PER_SEC;
          if (global_env.tbl.scope[ii].after != NULL) {
            global_env.tbl.scope[ii].after();
          }
        }
        ++passed;
        ++scope_passed;
        exam_print_ok(buf, diff);
      } else {
        exam_print_failed(buf);
        retValue = 1;
        if (global_env.die_on_fail) {
          free(buf);
          exam_free_shuffle(scopes_indices);
          exam_free_shuffle(tests_indices);
          free_exam_env();
          exit(1);
        }
      }
      free(buf);
    }
    if (printed_scope) {
      exam_print_passed_scope(scope_passed, global_env.tbl.scope[i].name);
      printf("\n");
    }
    exam_free_shuffle(tests_indices);
  }
  exam_free_shuffle(scopes_indices);

  exam_print_passed_result(passed);
  exam_print_final(count);

  free_exam_env();
  return retValue;
}

static void exam_insert_test_in_list(exam_scope_t *list, const char *scope,
                                     const char *name, void (*fn)(),
                                     bool pending) {
  if (list->tests == NULL) {
    list->cap = 16;
    list->tests = malloc(sizeof(exam_scope_t) * list->cap);
    list->len = 0;
  } else {
    if (list->len + 1 >= list->cap) {
      list->cap *= 2;
      list->tests = realloc(list->tests, sizeof(exam_scope_t) * list->cap);
    }
  }

  list->tests[list->len++] =
      (exam_test_t){.fn = fn, .scope = scope, .name = name, .pending = pending};
}

static void exam_ensure_tbl() {
  if (global_env.tbl.scope == NULL) {
    global_env.tbl.cap = 8;
    global_env.tbl.scope = malloc(sizeof(exam_scope_t) * global_env.tbl.cap);
    memset(global_env.tbl.scope, 0, sizeof(exam_scope_t) * global_env.tbl.cap);
    global_env.tbl.len = 0;
  }
}

static exam_scope_t *exam_insert_new_scope(const char *scope) {
  if (global_env.tbl.len + 1 >= global_env.tbl.cap) {
    global_env.tbl.cap *= 2;
    global_env.tbl.scope = realloc(global_env.tbl.scope,
                                   sizeof(exam_scope_t) * global_env.tbl.cap);
  }

  global_env.tbl.scope[global_env.tbl.len] = (exam_scope_t){.tests = NULL,
                                                            .len = 0,
                                                            .cap = 0,
                                                            .name = scope,
                                                            .before = NULL,
                                                            .after = NULL};
  return &global_env.tbl.scope[global_env.tbl.len++];
}

void _exam_register_test(const char *scope, const char *name, void (*fn)(),
                         bool pending) {
  exam_ensure_tbl();
  for (size_t i = 0; i < global_env.tbl.len; ++i) {
    if (strcmp(scope, global_env.tbl.scope[i].name) == 0) {
      exam_insert_test_in_list(&global_env.tbl.scope[i], scope, name, fn,
                               pending);
      return;
    }
  }
  exam_insert_test_in_list(exam_insert_new_scope(scope), scope, name, fn,
                           pending);
}

void _exam_register_each(const char *scope, void (*fn)(), bool before) {
  exam_ensure_tbl();
  for (size_t i = 0; i < global_env.tbl.len; ++i) {
    if (strcmp(scope, global_env.tbl.scope[i].name) == 0) {
      if (before) {
        global_env.tbl.scope[i].before = fn;
      } else {
        global_env.tbl.scope[i].after = fn;
      }
      return;
    }
  }
  exam_scope_t *newScope = exam_insert_new_scope(scope);
  if (before) {
    newScope->before = fn;
  } else {
    newScope->after = fn;
  }
}

void _exam_assert_true(bool value, const char *file, int line) {
  if (!value) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sexpected: true %sreceived: false %s\n", GREEN, RED, NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_false(bool value, const char *file, int line) {
  if (value) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sexpected: false %sreceived: true %s\n", GREEN, RED, NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_equal_double(double expected, double result, const char *file,
                               int line) {
  if (fabs(expected - result) >= EPSILON) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %f %sResult: %f%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_equal_float(float expected, float result, const char *file,
                              int line) {
  if (fabs(expected - result) >= EPSILON) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %f %sResult: %f%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_equal_int(int expected, int result, const char *file,
                            int line) {
  if (expected != result) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %d %sResult: %d%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_equal_str(const char *expected, const char *result,
                            const char *file, int line) {
  if (strcmp(expected, result) != 0) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %s %sResult: %s%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_equal_mem(void *expected, void *result, size_t len,
                            const char *file, int line) {
  const char *a = (const char *)expected;
  const char *b = (const char *)result;

  size_t differences = 0;
  for (size_t i = 0; i < len; i++) {
    const char l = a[i];
    const char r = b[i];
    if (l != r) {
      if (differences < 16) {
        if (differences == 0) {
          if (!global_env.shortd) {
            printf("  Error at line: %s:%d\n", file, line);
          }
        }
        if (!global_env.shortd) {
          printf("  difference at offset %zd 0x%02x != 0x%02x\n", i, l, r);
        }
      }
      differences++;
    }
  }
  if (differences > 0) {
    if (differences >= 16) {
      if (!global_env.shortd) {
        printf("  ...\n");
      }
    }
    if (!global_env.shortd) {
      printf("  %zd bytes of %p and %p are different\n", differences, (void *)a,
             (void *)b);
    }

    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_not_equal_double(double expected, double result,
                                   const char *file, int line) {
  if (fabs(expected - result) < EPSILON) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %f %sResult: %f%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_not_equal_float(float expected, float result,
                                  const char *file, int line) {
  if (fabs(expected - result) < EPSILON) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %f %sResult: %f%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_not_equal_int(int expected, int result, const char *file,
                                int line) {
  if (expected == result) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %d %sResult: %d%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_not_equal_str(const char *expected, const char *result,
                                const char *file, int line) {
  if (strcmp(expected, result) == 0) {
    if (!global_env.shortd) {
      printf("  Error at line: %s:%d\n", file, line);
      printf("  %sExpected: %s %sResult: %s%s\n", GREEN, expected, RED, result,
             NONE);
    }
    siglongjmp(global_sig, 1);
  }
}

void _exam_assert_not_equal_mem(void *expected, void *result, size_t len,
                                const char *file, int line) {
  const char *a = (const char *)expected;
  const char *b = (const char *)result;

  size_t differences = 0;
  for (size_t i = 0; i < len; i++) {
    const char l = a[i];
    const char r = b[i];
    if (l == r) {
      if (differences < 16) {
        if (differences == 0) {
          if (!global_env.shortd) {
            printf("  Error at line: %s:%d\n", file, line);
          }
        }
        if (!global_env.shortd) {
          printf("  same at offset %zd 0x%02x != 0x%02x\n", i, l, r);
        }
      }
      differences++;
    }
  }
  if (differences > 0) {
    if (differences >= 16) {
      if (!global_env.shortd) {
        printf("  ...\n");
      }
    }
    if (!global_env.shortd) {
      printf("  %zd bytes of %p and %p are same\n", differences, (void *)a,
             (void *)b);
    }

    siglongjmp(global_sig, 1);
  }
}
