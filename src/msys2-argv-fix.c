#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CW_GETPINFO 3
#define CW_USER_DATA 8
#define PID_CYGPARENT 0x80
#define GLOB_NOCHECK 0x0010
#define GLOB_BRACE 0x0080
#define GLOB_QUOTE 0x0400
#define GLOB_TILDE 0x0800
#define GLOB_NOSPACE (-1)
#define MAX_AT_FILE_LEVEL 10

typedef int (__cdecl *main_fn)(int, char **, char **);
typedef uintptr_t (__cdecl *cygwin_internal_fn)(int, ...);
typedef int (__cdecl *getpid_fn)(void);

typedef struct {
    size_t path_count;
    size_t match_count;
    size_t offset;
    int flags;
    char **paths;
    void *error;
    void *closedir;
    void *readdir;
    void *opendir;
    void *lstat;
    void *stat;
} runtime_glob_t;

typedef int (__cdecl *glob_fn)(const char *, int, int (__cdecl *)(const char *, int), runtime_glob_t *);
typedef void (__cdecl *globfree_fn)(runtime_glob_t *);
typedef char *(__cdecl *setlocale_fn)(int, const char *);
typedef void *(__cdecl *fopen_fn)(const char *, const char *);
typedef int (__cdecl *fseek_fn)(void *, int64_t, int);
typedef int64_t (__cdecl *ftell_fn)(void *);
typedef size_t (__cdecl *fread_fn)(void *, size_t, size_t, void *);
typedef int (__cdecl *fclose_fn)(void *);

struct runtime_api {
    cygwin_internal_fn cygwin_internal;
    getpid_fn getpid;
    glob_fn glob;
    globfree_fn globfree;
    setlocale_fn setlocale;
    fopen_fn fopen;
    fseek_fn fseek;
    ftell_fn ftell;
    fread_fn fread;
    fclose_fn fclose;
};

struct process_prefix {
    char *initial_stack;
    uint32_t size;
    uint32_t dll_major;
    uint32_t dll_minor;
    void *impure_ptr;
    void *malloc;
    void *free;
    void *realloc;
    int *fmode;
    main_fn main;
};

struct process_info {
    int32_t pid;
    int32_t parent_pid;
    uint32_t exit_code;
    uint32_t windows_pid;
    uint32_t spawned_pid;
    uint16_t user_id;
    uint16_t group_id;
    int32_t process_group;
    int32_t session;
    int32_t terminal;
    uint32_t mask;
    int64_t start_time;
    int64_t usage[36];
    char program[MAX_PATH];
    uint32_t trace_mask;
    uint32_t version;
    uint32_t state;
};

struct arguments {
    int count;
    int capacity;
    char **values;
};

_Static_assert(offsetof(struct process_prefix, main) == 64, "Unexpected per_process layout");
_Static_assert(offsetof(struct process_info, state) == 604, "Unexpected external_pinfo layout");

static struct runtime_api runtime;
static main_fn application_main;

static bool
is_separator(char value)
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

static bool
is_quote(char value)
{
    return value == '"' || value == '\'';
}

static int
next_argument(char **cursor, char **argument, unsigned char **quoted)
{
    char *input = *cursor;
    while (is_separator(*input))
        input++;
    if (!*input) {
        *cursor = input;
        return 0;
    }

    char *output = input;
    *argument = output;
    *quoted = calloc(strlen(input) + 1, 1);
    if (!*quoted)
        return -1;

    bool inside = false;
    char delimiter = 0;
    while (*input && (inside || !is_separator(*input))) {
        size_t slashes = 0;
        while (*input == '\\') {
            slashes++;
            input++;
        }

        bool at_quote = *input && (inside ? *input == delimiter : is_quote(*input));
        bool single_quote = at_quote && (inside ? delimiter : *input) == '\'';
        size_t literal_slashes = at_quote && !single_quote ? slashes / 2 : slashes;
        while (literal_slashes--) {
            (*quoted)[output - *argument] = inside;
            *output++ = '\\';
        }

        if (!*input)
            break;

        if (at_quote && (single_quote || !(slashes & 1))) {
            if (inside && input[1] == delimiter) {
                (*quoted)[output - *argument] = true;
                *output++ = *input++;
            } else {
                if (!inside)
                    delimiter = *input;
                inside = !inside;
            }
        } else {
            (*quoted)[output - *argument] = inside;
            *output++ = *input;
        }
        input++;
    }

    char *next = input + !!*input;
    *output = 0;
    *cursor = next;
    return 1;
}

static bool
append_argument(struct arguments *arguments, char *value)
{
    if (arguments->count == arguments->capacity) {
        int capacity = arguments->capacity + 10;
        char **values = realloc(arguments->values, (capacity + 1) * sizeof(*values));
        if (!values)
            return false;
        arguments->capacity = capacity;
        arguments->values = values;
    }

    arguments->values[arguments->count++] = value;
    arguments->values[arguments->count] = NULL;
    return true;
}

static bool
is_dos_path(const char *value)
{
    return (isalpha((unsigned char) value[0]) && value[1] == ':') ||
           (value[0] == '\\' && value[1] == '\\' && value[2]);
}

static size_t
utf8_character_length(const char *value, size_t remaining)
{
    unsigned char first = (unsigned char) *value;
    size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if (length > remaining)
        return 1;
    for (size_t i = 1; i < length; i++)
        if (((unsigned char) value[i] & 0xc0) != 0x80)
            return 1;
    return length;
}

static bool
expand_argument(struct arguments *arguments, char *word, const unsigned char *quoted)
{
    bool transformed = word[0] == '~' && !quoted[0];
    bool magic = transformed;
    for (size_t i = 0; word[i]; i++) {
        if (!quoted[i] && word[i] == '{')
            transformed = true;
        if (!quoted[i] && strchr("?*[(){}", word[i]))
            magic = true;
    }
    if (!magic)
        return append_argument(arguments, word);

    size_t length = strlen(word);
    char *pattern = malloc(length * 2 + 1);
    if (!pattern)
        return false;

    bool dos_path = is_dos_path(word);
    char *output = pattern;
    for (size_t i = 0; i < length;) {
        size_t bytes = utf8_character_length(word + i, length - i);
        if (quoted[i] || (dos_path && word[i] == '\\'))
            *output++ = '\\';
        memcpy(output, word + i, bytes);
        output += bytes;
        i += bytes;
    }
    *output = 0;

    runtime_glob_t matches = { 0 };
    int error = runtime.glob(pattern, GLOB_NOCHECK | GLOB_BRACE | GLOB_QUOTE | GLOB_TILDE, NULL,
                             &matches);
    free(pattern);
    if (error == GLOB_NOSPACE)
        return false;
    if (error || !matches.path_count || (!matches.match_count && !transformed)) {
        runtime.globfree(&matches);
        return append_argument(arguments, word);
    }

    for (size_t i = 0; i < matches.path_count; i++)
        if (!append_argument(arguments, matches.paths[i]))
            return false;
    return true;
}

static char *
read_file(const char *path)
{
    void *file = runtime.fopen(path, "rb");
    if (!file)
        return NULL;
    if (runtime.fseek(file, 0, SEEK_END)) {
        runtime.fclose(file);
        return NULL;
    }

    int64_t length = runtime.ftell(file);
    if (length < 0 || (uint64_t) length >= SIZE_MAX) {
        runtime.fclose(file);
        return NULL;
    }
    runtime.fseek(file, 0, SEEK_SET);

    char *contents = malloc((size_t) length + 1);
    if (!contents) {
        runtime.fclose(file);
        return NULL;
    }
    if (runtime.fread(contents, 1, (size_t) length, file) != (size_t) length) {
        free(contents);
        runtime.fclose(file);
        return NULL;
    }
    runtime.fclose(file);
    contents[length] = 0;
    return contents;
}

static bool
parse_command_line(char *line, struct arguments *arguments, int depth)
{
    char *cursor = line;
    char *word;
    unsigned char *quoted;
    for (;;) {
        int next = next_argument(&cursor, &word, &quoted);
        if (next <= 0)
            return next == 0;
        if (arguments->count && word[0] == '@' && !quoted[0] && depth < MAX_AT_FILE_LEVEL) {
            char *contents = read_file(word + 1);
            if (contents) {
                free(quoted);
                if (!parse_command_line(contents, arguments, depth + 1))
                    return false;
                continue;
            }
        }

        bool success = arguments->count ? expand_argument(arguments, word, quoted)
                                        : append_argument(arguments, word);
        free(quoted);
        if (!success)
            return false;
    }
}

static bool
glob_enabled(void)
{
    const char *options = getenv("MSYS");
    if (!options)
        return true;

    char *copy = _strdup(options);
    if (!copy)
        return true;

    bool enabled = true;
    char *context;
    for (char *option = strtok_s(copy, " \t", &context); option;
         option = strtok_s(NULL, " \t", &context)) {
        bool value = true;
        if (!_strnicmp(option, "no", 2)) {
            value = false;
            option += 2;
        } else if (*option == '-') {
            value = false;
            option++;
        }
        char *separator = strpbrk(option, ":=");
        if (separator)
            *separator++ = 0;
        if (!_stricmp(option, "glob"))
            enabled = value && (!separator || *separator);
    }
    free(copy);
    return enabled;
}

static bool
has_native_parent(void)
{
    struct process_info *process = (struct process_info *) runtime.cygwin_internal(
        CW_GETPINFO, runtime.getpid());
    return process && !(process->state & PID_CYGPARENT);
}

static char *
get_command_line(void)
{
    const wchar_t *wide = GetCommandLineW();
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!length)
        return NULL;
    char *line = malloc(length);
    if (!line)
        return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, line, length, NULL, NULL)) {
        free(line);
        return NULL;
    }
    return line;
}

static int __cdecl
fixed_main(int argc, char **argv, char **environment)
{
    if (!has_native_parent() || !glob_enabled())
        return application_main(argc, argv, environment);

    char *line = get_command_line();
    struct arguments arguments = { 0 };
    if (!line) {
        fputs("msys2-argv-fix: cannot read the command line\n", stderr);
        return 127;
    }

    const char *current_locale = runtime.setlocale(LC_CTYPE, NULL);
    char *locale = current_locale ? _strdup(current_locale) : NULL;
    if (!locale || !runtime.setlocale(LC_CTYPE, "")) {
        fputs("msys2-argv-fix: cannot initialize the locale\n", stderr);
        free(locale);
        return 127;
    }
    bool parsed = parse_command_line(line, &arguments, 0);
    runtime.setlocale(LC_CTYPE, locale);
    free(locale);
    if (!parsed || !arguments.count) {
        fputs("msys2-argv-fix: cannot parse the command line\n", stderr);
        return 127;
    }

    arguments.values[0] = argv[0];
    return application_main(arguments.count, arguments.values, environment);
}

#define LOAD_RUNTIME_FUNCTION(field, type, name) \
    do { \
        union { FARPROC address; type function; } symbol = { GetProcAddress(module, name) }; \
        runtime.field = symbol.function; \
        if (!runtime.field) \
            return FALSE; \
    } while (0)

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void) reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    (void) instance;
    HMODULE module = GetModuleHandleW(L"msys-2.0.dll");
    if (!module)
        return TRUE;

    LOAD_RUNTIME_FUNCTION(cygwin_internal, cygwin_internal_fn, "cygwin_internal");
    LOAD_RUNTIME_FUNCTION(getpid, getpid_fn, "getpid");
    LOAD_RUNTIME_FUNCTION(glob, glob_fn, "glob");
    LOAD_RUNTIME_FUNCTION(globfree, globfree_fn, "globfree");
    LOAD_RUNTIME_FUNCTION(setlocale, setlocale_fn, "setlocale");
    LOAD_RUNTIME_FUNCTION(fopen, fopen_fn, "fopen");
    LOAD_RUNTIME_FUNCTION(fseek, fseek_fn, "fseek");
    LOAD_RUNTIME_FUNCTION(ftell, ftell_fn, "ftell");
    LOAD_RUNTIME_FUNCTION(fread, fread_fn, "fread");
    LOAD_RUNTIME_FUNCTION(fclose, fclose_fn, "fclose");

    struct process_prefix *process = (struct process_prefix *) runtime.cygwin_internal(CW_USER_DATA);
    if (!process || process->size < sizeof(*process) || !process->main)
        return FALSE;

    application_main = process->main;
    process->main = fixed_main;
    return TRUE;
}
