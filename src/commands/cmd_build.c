#include "commands/cmd_build.h"
#include "compiler/lexer.h"
#include "compiler/masm/emit.h"
#include "compiler/masm/lower.h"
#include "compiler/masm/target.h"
#include "compiler/parser.h"
#include "compiler/sema.h"
#include "config.h"
#include "filesystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix)
    {
        return false;
    }
    size_t sl = strlen(s);
    size_t su = strlen(suffix);
    if (su > sl)
    {
        return false;
    }
    return memcmp(s + (sl - su), suffix, su) == 0;
}

// build an object path under obj_base mirroring a dotted module FQN:
// FQN "a.b.c" -> "<obj_base>/a/b/c.o". the returned path is heap-allocated.
// the parent directory of the object is created with `mkdir -p`.
static char *build_module_obj_path(const char *obj_base, const char *fqn)
{
    size_t base_len = strlen(obj_base);
    size_t fqn_len  = strlen(fqn);
    // "<obj_base>" + '/' + fqn(dots->slashes) + ".o" + '\0'
    char  *path     = malloc(base_len + 1 + fqn_len + 2 + 1);
    if (!path)
    {
        return NULL;
    }

    memcpy(path, obj_base, base_len);
    path[base_len] = '/';

    char *dst = path + base_len + 1;
    for (const char *src = fqn; *src; src++)
    {
        *dst++ = (*src == '.') ? '/' : *src;
    }
    *dst = '\0';

    // ensure the object's parent directory exists (FQN may be nested)
    char *parent   = strdup(path);
    char *last_sep = strrchr(parent, '/');
    if (last_sep)
    {
        *last_sep = '\0';
        char *mkdir_cmd = malloc(strlen(parent) + 32);
        if (mkdir_cmd)
        {
            sprintf(mkdir_cmd, "mkdir -p %s 2>/dev/null", parent);
            system(mkdir_cmd);
            free(mkdir_cmd);
        }
    }
    free(parent);

    strcat(path, ".o");
    return path;
}

void cmd_build_help(FILE *stream)
{
    fprintf(stream, "usage: mach build <project|file> [options]\n");
    fprintf(stream, "\n");
    fprintf(stream, "build a Mach project from the specified directory or compile a single Mach source file\n");
    fprintf(stream, "\n");
    fprintf(stream, "options:\n");
    fprintf(stream, "  --target <name>      select target from mach.toml (required for projects)\n");
    fprintf(stream, "  --artifacts <name>   override artifacts directory (relative to dir_out)\n");
    fprintf(stream, "  -o <file>            output file (executable or object)\n");
    fprintf(stream, "  -m <path>            set module path (e.g. 'std.print')\n");
    fprintf(stream, "  -I n=dir             map module prefix 'n' to base directory 'dir'\n");
}

int cmd_build_handle(int argc, char **argv)
{
    // argv[0] = "cmach", argv[1] = "build", argv[2] = input file
    if (argc < 3)
    {
        fprintf(stderr, "error: no input file specified\n");
        cmd_build_help(stderr);
        return 1;
    }

    const char *input_file      = argv[2];
    const char *output_file     = NULL;
    const char *artifacts_override = NULL;

    // extra module roots for single-file mode: -I prefix=dir
    char *include_prefixes[64];
    char *include_dirs[64];
    int   include_count = 0;

    // parse options
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            output_file = argv[++i];
        }
        else if (strcmp(argv[i], "--artifacts") == 0 && i + 1 < argc)
        {
            artifacts_override = argv[++i];
        }
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc)
        {
            if (include_count >= 64)
            {
                fprintf(stderr, "error: too many -I mappings (max 64)\n");
                return 1;
            }

            const char *spec = argv[++i];
            const char *eq   = strchr(spec, '=');
            if (!eq || eq == spec || *(eq + 1) == '\0')
            {
                fprintf(stderr, "error: invalid -I mapping '%s' (expected prefix=dir)\n", spec);
                return 1;
            }

            size_t prefix_len = (size_t)(eq - spec);
            char  *prefix     = malloc(prefix_len + 1);
            if (!prefix)
            {
                fprintf(stderr, "error: out of memory\n");
                return 1;
            }
            memcpy(prefix, spec, prefix_len);
            prefix[prefix_len] = '\0';

            char *dir_abs = absolutize_path(eq + 1);
            if (!dir_abs)
            {
                fprintf(stderr, "error: failed to resolve include dir '%s'\n", eq + 1);
                free(prefix);
                return 1;
            }

            include_prefixes[include_count] = prefix;
            include_dirs[include_count]     = dir_abs;
            include_count++;
        }
    }

    // check if input is a directory
    bool        is_project      = is_directory(input_file);
    const char *project_root    = is_project ? input_file : NULL;
    const char *target_binary   = NULL;
    const char *target_artifacts = NULL;

    // module resolution info (stored for sema)
    char   *project_id = NULL;
    char   *src_root   = NULL;
    char   *dep_root   = NULL;
    Config *config     = NULL;

    if (is_project)
    {
        char *config_path = path_join(input_file, "mach.toml");
        if (!file_exists(config_path))
        {
            fprintf(stderr, "error: directory '%s' does not contain mach.toml\n", input_file);
            free(config_path);
            return 1;
        }

        config = config_load(config_path);
        free(config_path);

        if (!config)
        {
            return 1; // config_load prints error
        }

        // find target (default to first or specified)
        if (config->target_count == 0)
        {
            fprintf(stderr, "error: no targets defined in mach.toml\n");
            config_dnit(config);
            free(config);
            return 1;
        }

        // pick the configured default target for the project (fallback: first)
        ConfigTarget *target = NULL;
        if (config->target)
        {
            target = config_get_target(config, config->target);
        }
        if (!target)
        {
            target = config->targets[0];
        }
        if (!target->entrypoint)
        {
            fprintf(stderr, "error: target '%s' has no entrypoint\n", target->name);
            config_dnit(config);
            free(config);
            return 1;
        }

        // store target binary path for output location
        if (target->binary)
        {
            target_binary = strdup(target->binary);
        }

        // store artifacts directory (--artifacts override takes priority)
        if (artifacts_override)
        {
            target_artifacts = artifacts_override;
        }
        else if (target->artifacts)
        {
            target_artifacts = strdup(target->artifacts);
        }

        // store module resolution info
        if (config->id)
        {
            project_id = strdup(config->id);
        }

        // construct full path to entrypoint
        char *src_dir_path = path_join(input_file, config->dir_src ? config->dir_src : "src");
        char *entry_path   = path_join(src_dir_path, target->entrypoint);

        // store absolute src_root for module resolution
        src_root = absolutize_path(src_dir_path);

        // store dep_root if configured
        if (config->dir_dep)
        {
            char *dep_dir_path = path_join(input_file, config->dir_dep);
            dep_root           = absolutize_path(dep_dir_path);
            free(dep_dir_path);
        }

        input_file = entry_path;

        free(src_dir_path);
        // note: config is kept alive until after sema_set_module_roots
    }

    // determine output file
    if (!output_file && is_project && project_root && target_binary)
    {
        // build to <dir_out>/<binary path>
        const char *dir_out = (config && config->dir_out) ? config->dir_out : "out";
        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s/%s", project_root, dir_out, target_binary);
        output_file = strdup(out_path);
    }
    else if (!output_file)
    {
        output_file = "output";
    }

    // ensure output directory exists
    {
        char *out_dir  = strdup(output_file);
        char *last_sep = strrchr(out_dir, '/');
        if (last_sep)
        {
            *last_sep = '\0';
            char mkdir_cmd[1536];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s 2>/dev/null", out_dir);
            system(mkdir_cmd);
        }
        free(out_dir);
    }

    // read source file
    FILE *f = fopen(input_file, "r");
    if (!f)
    {
        fprintf(stderr, "error: could not open file '%s'\n", input_file);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = malloc(size + 1);
    if (!source)
    {
        fprintf(stderr, "error: out of memory\n");
        fclose(f);
        return 1;
    }

    fread(source, 1, size, f);
    source[size] = '\0';
    fclose(f);

    // lex
    Lexer lexer;
    lexer_init(&lexer, source);

    // parse
    Parser parser;
    parser_init(&parser, &lexer);

    AstNode *ast = parser_parse_program(&parser);
    if (!ast || parser.had_error)
    {
        fprintf(stderr, "error: parsing failed\n");
        parser_error_list_print(&parser.errors, &lexer, input_file);
        parser_dnit(&parser);
        lexer_dnit(&lexer);
        free(source);
        return 1;
    }

    // determine module path
    char *module_path = NULL;

    // check for -m flag
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
        {
            module_path = strdup(argv[++i]);
            break;
        }
    }

    // if not specified, try to derive from project
    if (!module_path)
    {
        char *project_root = find_project_root(input_file);
        if (project_root && strcmp(project_root, ".") != 0)
        {
            char   *config_path = path_join(project_root, "mach.toml");
            Config *config      = config_load(config_path);
            free(config_path);

            if (config)
            {
                // calculate relative path from src_dir
                char *src_dir   = path_join(project_root, config->dir_src ? config->dir_src : "src");
                char *abs_input = absolutize_path(input_file);

                if (strncmp(abs_input, src_dir, strlen(src_dir)) == 0)
                {
                    // file is inside src_dir
                    char *rel_path = abs_input + strlen(src_dir);
                    if (is_sep(*rel_path))
                    {
                        rel_path++; // skip leading separator
                    }

                    // construct module path: project_id + . + rel_path (with / -> .)
                    // remove extension .mach
                    char *rel_no_ext = strdup(rel_path);
                    char *dot        = strrchr(rel_no_ext, '.');
                    if (dot)
                    {
                        *dot = '\0';
                    }

                    // replace separators with dots
                    for (char *p = rel_no_ext; *p; p++)
                    {
                        if (is_sep(*p))
                        {
                            *p = '.';
                        }
                    }

                    size_t len  = strlen(config->id) + 1 + strlen(rel_no_ext) + 1;
                    module_path = malloc(len);
                    snprintf(module_path, len, "%s.%s", config->id, rel_no_ext);

                    free(rel_no_ext);
                }

                free(abs_input);
                free(src_dir);
                config_dnit(config);
                free(config);
            }
        }
        free(project_root);
    }

    // 3. Default to "main"
    if (!module_path)
    {
        module_path = strdup("main");
    }

    // semantic analysis
    Sema *sema = sema_create(module_path);
    free(module_path); // sema makes a copy
    if (!sema)
    {
        fprintf(stderr, "error: failed to create semantic analyzer\n");
        parser_dnit(&parser);
        lexer_dnit(&lexer);
        free(source);
        if (project_id)
        {
            free(project_id);
        }
        if (src_root)
        {
            free(src_root);
        }
        if (dep_root)
        {
            free(dep_root);
        }
        return 1;
    }

    // set module resolution roots if available
    if (project_id && src_root)
    {
        sema_set_module_roots(sema, project_id, src_root, dep_root, config ? config->deps : NULL, config ? config->dep_count : 0);
    }

    // apply explicit single-file module root mappings
    for (int i = 0; i < include_count; i++)
    {
        sema_add_module_root(sema, include_prefixes[i], include_dirs[i]);
        free(include_prefixes[i]);
        free(include_dirs[i]);
    }
    free(src_root);
    free(dep_root);

    // NOTE: config must stay alive until sema is destroyed, as sema holds pointers to dep configs

    // set file context for error reporting
    sema_set_file_context(sema, input_file, source);

    if (sema_analyze(sema, ast) < 0)
    {
        sema_print_errors(sema);
        sema_destroy(sema);

        // clean up config after sema is destroyed
        if (config)
        {
            config_dnit(config);
            free(config);
        }

        parser_dnit(&parser);
        lexer_dnit(&lexer);
        free(source);
        return 1;
    }

    // lower to MASM and emit objects.
    // - single-file mode: lower the main module only and emit one object at `-o`
    //   (or default "output").
    // - project mode: lower EACH module to its own Masm and emit one relocatable
    //   object per module under <dir_out>/<artifacts>/obj, with the path mirroring
    //   the module FQN (dots -> slashes); then link/archive the full set.
    const char *final_output = output_file;

    // collected object paths to link/archive (project mode); each is heap-allocated.
    char **obj_paths   = NULL;
    int    obj_count   = 0;
    int    obj_cap     = 0;
    char  *obj_base    = NULL;

    if (is_project && project_root)
    {
        const char *dir_out   = (config && config->dir_out) ? config->dir_out : "out";
        const char *artifacts = target_artifacts ? target_artifacts : "default";

        size_t base_len = strlen(project_root) + 1 + strlen(dir_out) + 1 + strlen(artifacts) + 5;
        obj_base        = malloc(base_len);
        if (!obj_base)
        {
            fprintf(stderr, "error: out of memory\n");
            sema_destroy(sema);
            if (config)
            {
                config_dnit(config);
                free(config);
            }
            parser_dnit(&parser);
            lexer_dnit(&lexer);
            free(source);
            free(project_id);
            return 1;
        }
        sprintf(obj_base, "%s/%s/%s/obj", project_root, dir_out, artifacts);
    }

    // collect (ast, table, fqn) for every module to lower: main first, then imports.
    SemaLoadedModule loaded[256];
    int              loaded_count = sema_get_loaded_modules(sema, loaded, 256);

    int total_modules = 1 + loaded_count;
    obj_cap           = total_modules;
    obj_paths         = malloc(sizeof(char *) * (obj_cap > 0 ? obj_cap : 1));
    if (!obj_paths)
    {
        fprintf(stderr, "error: out of memory\n");
        free(obj_base);
        sema_destroy(sema);
        if (config)
        {
            config_dnit(config);
            free(config);
        }
        parser_dnit(&parser);
        lexer_dnit(&lexer);
        free(source);
        free(project_id);
        return 1;
    }

    bool emit_failed = false;
    for (int i = 0; i < total_modules; i++)
    {
        AstNode     *mod_ast = (i == 0) ? ast : loaded[i - 1].ast;
        SymbolTable *mod_tab = (i == 0) ? sema_get_main_module_table(sema) : loaded[i - 1].table;
        const char  *mod_fqn = (i == 0) ? sema_get_main_module_path(sema) : loaded[i - 1].module_path;

        // determine the object output path for this module.
        char       *mod_obj_owned = NULL;
        const char *mod_obj       = NULL;
        if (is_project && project_root)
        {
            const char *fqn = mod_fqn;
            // fall back to the project id for the main module if its FQN is empty;
            // a non-main module without an FQN cannot be placed and is skipped.
            if (!fqn || fqn[0] == '\0')
            {
                if (i == 0 && project_id)
                {
                    fqn = project_id;
                }
                else
                {
                    fprintf(stderr, "warning: skipping module with empty module path\n");
                    continue;
                }
            }

            mod_obj_owned = build_module_obj_path(obj_base, fqn);
            if (!mod_obj_owned)
            {
                fprintf(stderr, "error: out of memory\n");
                emit_failed = true;
                break;
            }
            mod_obj = mod_obj_owned;
        }
        else
        {
            // single-file / non-project mode: emit the one object at the output path.
            mod_obj = output_file;
        }

        Masm *mod_masm = masm_lower_module(mod_ast, mod_tab);
        if (!mod_masm)
        {
            fprintf(stderr, "error: lowering to MASM failed\n");
            free(mod_obj_owned);
            emit_failed = true;
            break;
        }

        int rc = masm_emit_object(mod_masm, mod_obj);
        masm_destroy(mod_masm);
        if (rc < 0)
        {
            fprintf(stderr, "error: failed to emit object file\n");
            free(mod_obj_owned);
            emit_failed = true;
            break;
        }

        if (is_project && project_root)
        {
            obj_paths[obj_count++] = mod_obj_owned;
        }

        // single-file mode emits exactly one object; stop after the main module.
        if (!(is_project && project_root))
        {
            break;
        }
    }

    if (emit_failed)
    {
        for (int i = 0; i < obj_count; i++)
        {
            free(obj_paths[i]);
        }
        free(obj_paths);
        free(obj_base);
        sema_destroy(sema);
        if (config)
        {
            config_dnit(config);
            free(config);
        }
        parser_dnit(&parser);
        lexer_dnit(&lexer);
        free(source);
        free(project_id);
        return 1;
    }

    if (is_project && config)
    {
        // select the same target used above
        ConfigTarget *target = NULL;
        if (config->target)
        {
            target = config_get_target(config, config->target);
        }
        if (!target)
        {
            target = config->targets[0];
        }

        ConfigTargetModeKind mode = TARGET_MODE_EXECUTABLE;
        if (target && target->mode)
        {
            mode = target->mode->kind;
        }

        // assemble the link/archive command. the object list can be large
        // (~hundreds of modules), so size the buffer dynamically to avoid
        // truncation of the object set.
        size_t objs_len = 0;
        for (int i = 0; i < obj_count; i++)
        {
            objs_len += strlen(obj_paths[i]) + 1; // path + separating space
        }

        // prefix + output path + objects + slack for flags/null
        size_t cmd_cap = 64 + strlen(final_output) + objs_len + 64;
        char  *cmd     = malloc(cmd_cap);
        if (!cmd)
        {
            fprintf(stderr, "error: out of memory\n");
            for (int i = 0; i < obj_count; i++)
            {
                free(obj_paths[i]);
            }
            free(obj_paths);
            free(obj_base);
            sema_destroy(sema);
            if (config)
            {
                config_dnit(config);
                free(config);
            }
            parser_dnit(&parser);
            lexer_dnit(&lexer);
            free(source);
            free(project_id);
            return 1;
        }

        (void)remove(final_output);

        size_t pos = 0;
        if (mode == TARGET_MODE_LIBRARY)
        {
            pos += (size_t)snprintf(cmd + pos, cmd_cap - pos, "ar rcs %s", final_output);
        }
        else
        {
            // note: -no-pie is important on many distros defaulting to PIE;
            // we provide our own _start, so link without crt.
            pos += (size_t)snprintf(cmd + pos, cmd_cap - pos, "cc -nostdlib -no-pie -Wl,-e,_start -o %s", final_output);
        }
        for (int i = 0; i < obj_count; i++)
        {
            pos += (size_t)snprintf(cmd + pos, cmd_cap - pos, " %s", obj_paths[i]);
        }

        int rc = system(cmd);
        free(cmd);
        if (rc != 0)
        {
            fprintf(stderr, "error: %s failed (%d)\n", mode == TARGET_MODE_LIBRARY ? "archiving" : "linking", rc);
            for (int i = 0; i < obj_count; i++)
            {
                free(obj_paths[i]);
            }
            free(obj_paths);
            free(obj_base);
            sema_destroy(sema);
            if (config)
            {
                config_dnit(config);
                free(config);
            }
            parser_dnit(&parser);
            lexer_dnit(&lexer);
            free(source);
            free(project_id);
            return 1;
        }

        // ensure executable bit when linking through cc without crt
        if (mode != TARGET_MODE_LIBRARY && !str_ends_with(final_output, ".a") && !str_ends_with(final_output, ".o"))
        {
            char *chmod_cmd = malloc(strlen(final_output) + 32);
            if (chmod_cmd)
            {
                sprintf(chmod_cmd, "chmod +x %s 2>/dev/null", final_output);
                (void)system(chmod_cmd);
                free(chmod_cmd);
            }
        }
    }

    // cleanup
    for (int i = 0; i < obj_count; i++)
    {
        free(obj_paths[i]);
    }
    free(obj_paths);
    free(obj_base);
    sema_destroy(sema);

    // clean up config after sema is destroyed
    if (config)
    {
        config_dnit(config);
        free(config);
    }

    parser_dnit(&parser);
    lexer_dnit(&lexer);
    free(source);
    free(project_id);

    return 0;
}
