#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>

#include <ctype.h>
#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
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
#define CCP_POSIX_TO_WIN_A 0
#define CCP_POSIX_TO_WIN_W 1

typedef int (__cdecl *main_fn)(int, char **, char **);
typedef uintptr_t (__cdecl *cygwin_internal_fn)(int, ...);
typedef ptrdiff_t (__cdecl *cygwin_conv_path_fn)(unsigned int, const void *, void *, size_t);
typedef int (__cdecl *getpid_fn)(void);
typedef void *(__cdecl *runtime_malloc_fn)(size_t);
typedef void (__cdecl *runtime_free_fn)(void *);
typedef void *(__cdecl *runtime_realloc_fn)(void *, size_t);
typedef char *(__cdecl *arg_converter_fn)(const char *, const char *, size_t);
typedef PVOID (WINAPI *virtual_alloc2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                         MEM_EXTENDED_PARAMETER *, ULONG);

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
    HMODULE module;
    cygwin_internal_fn cygwin_internal;
    cygwin_conv_path_fn cygwin_conv_path;
    getpid_fn getpid;
    runtime_free_fn free;
    runtime_realloc_fn realloc;
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
    runtime_malloc_fn malloc;
    runtime_free_fn free;
    runtime_realloc_fn realloc;
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
static arg_converter_fn original_arg_converter;

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

static bool
argument_is_excluded(const char *argument, const char *exclusions, size_t exclusion_count)
{
    for (size_t i = 0; i < exclusion_count; i++) {
        size_t length = strlen(exclusions);
        if ((length == 1 && exclusions[0] == '*') ||
            (length && !strncmp(argument, exclusions, length)))
            return true;
        exclusions += length + 1;
    }
    return false;
}

static const char *
path_value(const char *argument)
{
    const char *value = argument;
    if (argument[0] == '-' && argument[1] == '-') {
        const char *equals = strchr(argument, '=');
        if (!equals)
            return NULL;
        value = equals + 1;
    }

    if ((value[0] == '/' && value[1] != '/') ||
        (value[0] == '.' && value[1] == '/') ||
        (value[0] == '.' && value[1] == '.' && value[2] == '/'))
        return value;
    return NULL;
}

static bool
path_is_unambiguous(const char *path)
{
    return !strchr(path, ':') && !strpbrk(path, "*?[]{}()|^$+\\");
}

static bool
native_path_exists(const char *path)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (!length)
        return false;

    wchar_t *wide_path = malloc((size_t) length * sizeof(*wide_path));
    if (!wide_path)
        return false;

    bool exists = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, length) == length &&
                  GetFileAttributesW(wide_path) != INVALID_FILE_ATTRIBUTES;
    free(wide_path);
    return exists;
}

static char *
convert_relative_argument(const char *argument, const char *path)
{
    ptrdiff_t wide_size = runtime.cygwin_conv_path(CCP_POSIX_TO_WIN_W, path, NULL, 0);
    if (wide_size <= 0)
        return (char *) argument;

    wchar_t *wide_path = malloc((size_t) wide_size);
    if (!wide_path)
        return (char *) argument;
    if (runtime.cygwin_conv_path(CCP_POSIX_TO_WIN_W, path, wide_path, (size_t) wide_size) ||
        GetFileAttributesW(wide_path) == INVALID_FILE_ATTRIBUTES) {
        free(wide_path);
        return (char *) argument;
    }
    free(wide_path);

    ptrdiff_t converted_size = runtime.cygwin_conv_path(CCP_POSIX_TO_WIN_A, path, NULL, 0);
    if (converted_size <= 0)
        return (char *) argument;

    char *converted = malloc((size_t) converted_size);
    if (!converted)
        return (char *) argument;
    if (runtime.cygwin_conv_path(CCP_POSIX_TO_WIN_A, path, converted, (size_t) converted_size)) {
        free(converted);
        return (char *) argument;
    }

    size_t prefix_length = (size_t) (path - argument);
    if (prefix_length > SIZE_MAX - (size_t) converted_size) {
        free(converted);
        return (char *) argument;
    }

    char *result = runtime.realloc((char *) argument, prefix_length + (size_t) converted_size);
    if (result)
        memcpy(result + prefix_length, converted, (size_t) converted_size);
    free(converted);
    return result ? result : (char *) argument;
}

static char * __cdecl
convert_native_argument(const char *argument, const char *exclusions, size_t exclusion_count)
{
    if (!argument || argument_is_excluded(argument, exclusions, exclusion_count))
        return (char *) argument;

    const char *path = path_value(argument);
    if (!path || !path_is_unambiguous(path))
        return (char *) argument;

    if (path[0] == '.')
        return convert_relative_argument(argument, path);

    char *converted = original_arg_converter(argument, NULL, 0);
    if (!converted || converted == argument)
        return (char *) argument;

    size_t prefix_length = (size_t) (path - argument);
    if (!native_path_exists(converted + prefix_length)) {
        runtime.free(converted);
        return (char *) argument;
    }

    return converted;
}

static IMAGE_NT_HEADERS64 *
runtime_headers(void)
{
    BYTE *base = (BYTE *) runtime.module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return NULL;

    IMAGE_NT_HEADERS64 *headers = (IMAGE_NT_HEADERS64 *) (base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        !headers->FileHeader.NumberOfSections || headers->FileHeader.NumberOfSections > 96)
        return NULL;
    return headers;
}

static bool
section_range(const IMAGE_NT_HEADERS64 *headers, const IMAGE_SECTION_HEADER *section,
              BYTE **start, size_t *size)
{
    size_t image_size = headers->OptionalHeader.SizeOfImage;
    size_t section_size = section->Misc.VirtualSize ? section->Misc.VirtualSize
                                                    : section->SizeOfRawData;
    size_t section_start = section->VirtualAddress;
    if (section_start > image_size || section_size > image_size - section_start)
        return false;

    *start = (BYTE *) runtime.module + section_start;
    *size = section_size;
    return true;
}

static BYTE *
find_unique_string(const IMAGE_NT_HEADERS64 *headers, const char *value, size_t length)
{
    const IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(headers);
    BYTE *match = NULL;
    for (WORD i = 0; i < headers->FileHeader.NumberOfSections; i++) {
        BYTE *start;
        size_t size;
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_READ) ||
            (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            !section_range(headers, sections + i, &start, &size) || size < length)
            continue;

        for (size_t offset = 0; offset <= size - length; offset++)
            if (!memcmp(start + offset, value, length)) {
                if (match)
                    return NULL;
                match = start + offset;
            }
    }
    return match;
}

static const RUNTIME_FUNCTION *
runtime_functions(const IMAGE_NT_HEADERS64 *headers, size_t *count)
{
    const IMAGE_DATA_DIRECTORY *directory =
        headers->OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_EXCEPTION;
    size_t image_size = headers->OptionalHeader.SizeOfImage;
    if (directory->VirtualAddress > image_size ||
        directory->Size > image_size - directory->VirtualAddress ||
        directory->Size % sizeof(RUNTIME_FUNCTION))
        return NULL;

    *count = directory->Size / sizeof(RUNTIME_FUNCTION);
    return (const RUNTIME_FUNCTION *) ((BYTE *) runtime.module + directory->VirtualAddress);
}

static const RUNTIME_FUNCTION *
function_containing(const RUNTIME_FUNCTION *functions, size_t function_count, DWORD rva)
{
    for (size_t i = 0; i < function_count; i++)
        if (rva >= functions[i].BeginAddress && rva < functions[i].EndAddress)
            return functions + i;
    return NULL;
}

static const RUNTIME_FUNCTION *
function_referencing(const IMAGE_NT_HEADERS64 *headers, const RUNTIME_FUNCTION *functions,
                     size_t function_count, const BYTE *target)
{
    const IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(headers);
    const RUNTIME_FUNCTION *match = NULL;
    for (WORD i = 0; i < headers->FileHeader.NumberOfSections; i++) {
        BYTE *start;
        size_t size;
        if (!(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            !section_range(headers, sections + i, &start, &size) || size < 7)
            continue;

        for (size_t offset = 0; offset <= size - 7; offset++) {
            BYTE *instruction = start + offset;
            if ((instruction[0] & 0xf0) != 0x40 || instruction[1] != 0x8d ||
                (instruction[2] & 0xc7) != 0x05)
                continue;

            int32_t displacement;
            memcpy(&displacement, instruction + 3, sizeof(displacement));
            uintptr_t referenced = (uintptr_t) (instruction + 7) + (intptr_t) displacement;
            if (referenced != (uintptr_t) target)
                continue;

            const RUNTIME_FUNCTION *function = function_containing(
                functions, function_count, (DWORD) (instruction - (BYTE *) runtime.module));
            if (!function || (match && (match->BeginAddress != function->BeginAddress ||
                                        match->EndAddress != function->EndAddress)))
                return NULL;
            match = function;
        }
    }
    return match;
}

static BYTE *
find_conversion_call(void)
{
    static const char converter_message[] = "convert()'ed: %s (length %d)\n.....->: %s";
    static const char worker_message[] = "newargv[%d] = %s";
    IMAGE_NT_HEADERS64 *headers = runtime_headers();
    if (!headers)
        return NULL;

    size_t function_count;
    const RUNTIME_FUNCTION *functions = runtime_functions(headers, &function_count);
    if (!functions)
        return NULL;

    BYTE *converter_message_address =
        find_unique_string(headers, converter_message, sizeof(converter_message));
    BYTE *worker_message_address = find_unique_string(headers, worker_message, sizeof(worker_message));
    if (!converter_message_address || !worker_message_address)
        return NULL;

    const RUNTIME_FUNCTION *converter_function = function_referencing(
        headers, functions, function_count, converter_message_address);
    const RUNTIME_FUNCTION *worker_function = function_referencing(
        headers, functions, function_count, worker_message_address);
    if (!converter_function || !worker_function ||
        converter_function->BeginAddress == worker_function->BeginAddress)
        return NULL;

    BYTE *converter = (BYTE *) runtime.module + converter_function->BeginAddress;
    BYTE *start = (BYTE *) runtime.module + worker_function->BeginAddress;
    BYTE *end = (BYTE *) runtime.module + worker_function->EndAddress;
    BYTE *match = NULL;
    for (BYTE *instruction = start; instruction + 5 <= end; instruction++)
        if (instruction[0] == 0xe8) {
            int32_t displacement;
            memcpy(&displacement, instruction + 1, sizeof(displacement));
            if ((uintptr_t) (instruction + 5) + (intptr_t) displacement ==
                (uintptr_t) converter) {
                if (match)
                    return NULL;
                match = instruction;
            }
        }
    return match;
}

static void *
allocate_relay(const BYTE *call)
{
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (!kernelbase)
        return NULL;

    union { FARPROC address; virtual_alloc2_fn function; } symbol = {
        GetProcAddress(kernelbase, "VirtualAlloc2")
    };
    if (!symbol.function)
        return NULL;

    uintptr_t center = (uintptr_t) call + 5;
    if (center > UINTPTR_MAX - INT32_MAX)
        return NULL;

    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    uintptr_t granularity = system_info.dwAllocationGranularity;
    uintptr_t lowest = center > INT32_MAX ? center - INT32_MAX : 0;
    uintptr_t highest = center + INT32_MAX;
    if (lowest < (uintptr_t) system_info.lpMinimumApplicationAddress)
        lowest = (uintptr_t) system_info.lpMinimumApplicationAddress;
    if (highest > (uintptr_t) system_info.lpMaximumApplicationAddress)
        highest = (uintptr_t) system_info.lpMaximumApplicationAddress;
    lowest = (lowest + granularity - 1) & ~(granularity - 1);
    highest = ((highest + 1) & ~(granularity - 1)) - 1;
    if (lowest >= highest)
        return NULL;

    MEM_ADDRESS_REQUIREMENTS requirements = { 0 };
    requirements.LowestStartingAddress = (PVOID) lowest;
    requirements.HighestEndingAddress = (PVOID) highest;
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return symbol.function(GetCurrentProcess(), NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE, &parameter, 1);
}

static bool
install_argument_hook(void)
{
    BYTE *call = find_conversion_call();
    if (!call)
        return false;

    int32_t original_displacement;
    memcpy(&original_displacement, call + 1, sizeof(original_displacement));
    original_arg_converter = (arg_converter_fn) ((BYTE *) call + 5 + original_displacement);
    if (!original_arg_converter)
        return false;

    BYTE *relay = allocate_relay(call);
    if (!relay)
        return false;

    static const BYTE relay_prefix[] = { 0xff, 0x25, 0, 0, 0, 0 };
    union { arg_converter_fn function; uintptr_t address; } hook = { convert_native_argument };
    memcpy(relay, relay_prefix, sizeof(relay_prefix));
    memcpy(relay + sizeof(relay_prefix), &hook.address, sizeof(hook.address));

    DWORD old_protection;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, sizeof(relay_prefix) + sizeof(hook.address))) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    intptr_t distance = relay - (call + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }
    int32_t displacement = (int32_t) distance;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protection)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return false;
    }

    memcpy(call + 1, &displacement, sizeof(displacement));
    if (FlushInstructionCache(GetCurrentProcess(), call, 5)) {
        DWORD ignored;
        if (VirtualProtect(call, 5, old_protection, &ignored))
            return true;
    }

    memcpy(call + 1, &original_displacement, sizeof(original_displacement));
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD ignored;
    VirtualProtect(call, 5, old_protection, &ignored);
    VirtualFree(relay, 0, MEM_RELEASE);
    return false;
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
    runtime.module = module;

    LOAD_RUNTIME_FUNCTION(cygwin_internal, cygwin_internal_fn, "cygwin_internal");
    LOAD_RUNTIME_FUNCTION(cygwin_conv_path, cygwin_conv_path_fn, "cygwin_conv_path");
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
    if (!process || process->size < sizeof(*process) || !process->free || !process->realloc ||
        !process->main)
        return FALSE;

    application_main = process->main;
    runtime.free = process->free;
    runtime.realloc = process->realloc;
    if (!install_argument_hook())
        return FALSE;
    process->main = fixed_main;
    return TRUE;
}
