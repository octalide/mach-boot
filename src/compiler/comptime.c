#include "compiler/comptime.h"
#include "compiler/sema.h"
#include "compiler/type.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *detect_host_os(void);
static const char *detect_host_arch(void);
static const char *detect_host_abi(void);
static int64_t     detect_host_pointer_width(void);
static int         ensure_segment_capacity(const char ***segments_ptr, int *capacity, int count);
static int         flatten_path(AstNode *expr, const char ***out_segments, int *out_count);
static void        set_int_result(AstNode *node, int64_t value);
static void        set_string_result(AstNode *node, const char *value);

static void set_string_result(AstNode *node, const char *value)
{
    if (!node)
    {
        return;
    }

    const char *text = value ? value : "";
    node->comptime.value_kind   = COMPTIME_STRING;
    node->comptime.string_value = strdup(text);
    node->type                  = type_get_primitive(TYPE_PTR); // "..." is *u8
}

static void set_int_result(AstNode *node, int64_t value)
{
    if (!node)
    {
        return;
    }

    node->comptime.value_kind = COMPTIME_INT;
    node->comptime.int_value  = value;
    node->type                = type_get_primitive(TYPE_I64);
}

// host os tag, one of the locked $mach.os.* values
static const char *detect_host_os(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "freestanding";
#endif
}

// host arch tag, one of the locked $mach.arch.* values; null if unsupported
static const char *detect_host_arch(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return NULL;
#endif
}

// host abi, best-effort from the build toolchain
static const char *detect_host_abi(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return "msvc";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "gnu";
#else
    return "";
#endif
}

static int64_t detect_host_pointer_width(void)
{
    return (int64_t)sizeof(void *);
}

static int ensure_segment_capacity(const char ***segments_ptr, int *capacity, int count)
{
    if (count < *capacity)
    {
        return 0;
    }

    int new_capacity        = (*capacity) * 2;
    const char **new_buffer = realloc(*segments_ptr, sizeof(*new_buffer) * new_capacity);
    if (!new_buffer)
    {
        return -1;
    }

    *segments_ptr = new_buffer;
    *capacity     = new_capacity;
    return 0;
}

static int flatten_path(AstNode *expr, const char ***out_segments, int *out_count)
{
    if (!expr || !out_segments || !out_count)
    {
        return -1;
    }

    int          capacity = 4;
    const char **segments = malloc(sizeof(*segments) * capacity);
    int          count    = 0;
    AstNode     *curr     = expr;

    if (!segments)
    {
        return -1;
    }

    while (curr && curr->kind == AST_EXPR_FIELD)
    {
        if (ensure_segment_capacity(&segments, &capacity, count) < 0)
        {
            free(segments);
            return -1;
        }

        segments[count++] = curr->field_expr.field;
        curr              = curr->field_expr.object;
    }

    if (!curr || curr->kind != AST_EXPR_IDENT)
    {
        free(segments);
        return -1;
    }

    if (ensure_segment_capacity(&segments, &capacity, count) < 0)
    {
        free(segments);
        return -1;
    }

    segments[count++] = curr->ident_expr.name;

    for (int i = 0; i < count / 2; i++)
    {
        const char *tmp         = segments[i];
        segments[i]             = segments[count - 1 - i];
        segments[count - 1 - i] = tmp;
    }

    *out_segments = segments;
    *out_count    = count;
    return 0;
}

// $mach.target.* — what we are building for (host-derived in the bootstrap)
static int resolve_target(AstNode *node, const char **seg, int count)
{
    if (count != 3)
    {
        return -1;
    }

    if (strcmp(seg[2], "os") == 0)
    {
        set_string_result(node, detect_host_os());
        return 0;
    }
    if (strcmp(seg[2], "arch") == 0)
    {
        const char *arch = detect_host_arch();
        if (!arch)
        {
            return -1;
        }
        set_string_result(node, arch);
        return 0;
    }
    if (strcmp(seg[2], "abi") == 0)
    {
        set_string_result(node, detect_host_abi());
        return 0;
    }
    if (strcmp(seg[2], "pointer_width") == 0)
    {
        set_int_result(node, detect_host_pointer_width());
        return 0;
    }

    return -1;
}

// $mach.os.* — closed set of os tag path-values
static int resolve_os_tag(AstNode *node, const char **seg, int count)
{
    if (count != 3)
    {
        return -1;
    }

    if (strcmp(seg[2], "linux") == 0 || strcmp(seg[2], "darwin") == 0 || strcmp(seg[2], "windows") == 0 ||
        strcmp(seg[2], "freestanding") == 0)
    {
        set_string_result(node, seg[2]);
        return 0;
    }

    return -1;
}

// $mach.arch.* — closed set of arch tag path-values
static int resolve_arch_tag(AstNode *node, const char **seg, int count)
{
    if (count != 3)
    {
        return -1;
    }

    if (strcmp(seg[2], "x86_64") == 0 || strcmp(seg[2], "aarch64") == 0)
    {
        set_string_result(node, seg[2]);
        return 0;
    }

    return -1;
}

// $mach.build.* — properties of this build session
static int resolve_build(AstNode *node, const char **seg, int count)
{
    if (count == 3 && strcmp(seg[2], "timestamp") == 0)
    {
        char       buf[32];
        time_t     now = time(NULL);
        struct tm *utc = gmtime(&now);
        if (utc && strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", utc) > 0)
        {
            set_string_result(node, buf);
            return 0;
        }
        set_string_result(node, "");
        return 0;
    }

    if (count == 3 && strcmp(seg[2], "host") == 0)
    {
        const char *arch = detect_host_arch();
        char        buf[64];
        snprintf(buf, sizeof(buf), "%s-%s", arch ? arch : "unknown", detect_host_os());
        set_string_result(node, buf);
        return 0;
    }

    // build mode and git state are not tracked by the bootstrap compiler
    return -1;
}

// $mach.project.* — values the bootstrap derives from mach.toml / module roots
static int resolve_project(Sema *sema, AstNode *node, const char **seg, int count)
{
    if (count != 3)
    {
        return -1;
    }

    if (strcmp(seg[2], "name") == 0)
    {
        const char *name = sema_project_id(sema);
        if (!name)
        {
            return -1;
        }
        set_string_result(node, name);
        return 0;
    }
    if (strcmp(seg[2], "root") == 0)
    {
        const char *root = sema_src_root(sema);
        if (!root)
        {
            return -1;
        }
        set_string_result(node, root);
        return 0;
    }

    // version is not tracked by the bootstrap compiler
    return -1;
}

// $mach.source.* — current source position the bootstrap can report
static int resolve_source(Sema *sema, AstNode *node, const char **seg, int count)
{
    if (count != 3)
    {
        return -1;
    }

    if (strcmp(seg[2], "file") == 0)
    {
        const char *file = sema_current_file(sema);
        if (!file)
        {
            return -1;
        }
        set_string_result(node, file);
        return 0;
    }
    if (strcmp(seg[2], "module") == 0)
    {
        const char *module = sema_current_module(sema);
        if (!module)
        {
            return -1;
        }
        set_string_result(node, module);
        return 0;
    }

    // line and function are not tracked by the comptime evaluator
    return -1;
}

int comptime_lookup(Sema *sema, AstNode *node)
{
    if (!node || !node->comptime.inner)
    {
        return -1;
    }

    const char **segments = NULL;
    int          count    = 0;

    if (flatten_path(node->comptime.inner, &segments, &count) < 0)
    {
        return -1;
    }

    if (count < 2 || strcmp(segments[0], "mach") != 0)
    {
        free(segments);
        return -1;
    }

    int result = -1;

    if (strcmp(segments[1], "target") == 0)
    {
        result = resolve_target(node, segments, count);
    }
    else if (strcmp(segments[1], "os") == 0)
    {
        result = resolve_os_tag(node, segments, count);
    }
    else if (strcmp(segments[1], "arch") == 0)
    {
        result = resolve_arch_tag(node, segments, count);
    }
    else if (strcmp(segments[1], "build") == 0)
    {
        result = resolve_build(node, segments, count);
    }
    else if (strcmp(segments[1], "project") == 0)
    {
        result = resolve_project(sema, node, segments, count);
    }
    else if (strcmp(segments[1], "source") == 0)
    {
        result = resolve_source(sema, node, segments, count);
    }

    free(segments);
    return result;
}
