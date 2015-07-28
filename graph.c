#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <histedit.h>

#include "common.h"
#include "command.h"
#include "modules/sqlite.h"
#include "struct/xtring.h"
#include "struct/hashtable.h"
#include "struct/dptrarray.h"

#define STRING_APPEND_STRING(dest, suffix) \
    do { \
        string_append_string_len(dest, suffix, STR_LEN(suffix)); \
    } while (0);

typedef enum {
    ARG_TYPE_END, // dummy
    ARG_TYPE_LITERAL, // "list", "add", ...
    ARG_TYPE_NUMBER,
    ARG_TYPE_CHOICES, // a value among predefined choices (like a DNS record type, on/off, ...)
    ARG_TYPE_STRING // free string (eventually with help of completion)
} argument_type_t;

// méthode toString pour intégrer un élément quelconque (record_t *, server_t *, ...) à la ligne de commande (ie le résultat de la complétion) ?
// méthode toString pour représenter un élément quelconque (record_t *, server_t *, ...) parmi les propositions à faire à l'utilisateur ?
struct argument_t {
    size_t offset;           // result of offsetof, address to copy value of argument
    argument_type_t type;    // one of the ARG_TYPE_* constant
    complete_t complete;     // ARG_TYPE_STRING and ARG_TYPE_CHOICES specific
    const char *string;      // a (short) string to represent the argument in help, eg: "help" (literal), "<on/off>" (limited choices), "<endpoint>" (string)
    handle_t handle;         // the function to run
    DPtrArray *children;     // dynamic array of possible children (arguments we can find next)
    void *command_data;      // user data to run command
    void *completion_data;   // user data for completion
    const char *description; // short description of command (only for ARG_TYPE_STRING)
//     graph_t *owner;
};

struct graph_t {
    HashTable *roots;
    HashTable *nodes;
    graph_node_t *end;
    completer_t *possibilities;
};

static model_t argument_model = { /* dummy model */
    0/*sizeof(argument_t)*/,
#if 0
    (const model_field_t []) {
        MODEL_FIELD_SENTINEL
    }
#else
    NULL
#endif
};

/**
 * Abstraction layer to store possibilities for current completion
 */

// wrap DPtrArray even if it is alone, future change should be simpler
struct completer_t {
    DPtrArray *ary;
};

typedef struct {
    const char *string;
    bool delegated;
} possibility_t;

static void possibility_destroy(void *data)
{
    possibility_t *p;

    assert(NULL != data);
    p = (possibility_t *) data;
    if (p->delegated) {
        free((void *) p->string);
    }
}

static completer_t *completer_new(void)
{
    completer_t *c;

    c = mem_new(*c);
    c->ary = dptrarray_new(NULL, NULL, NULL);

    return c;
}

static void completer_clear(completer_t *c)
{
    dptrarray_clear(c->ary);
}

void completer_push(completer_t *c, const char *string, bool UNUSED(delegate))
{
    dptrarray_push(c->ary, (void *) string);
}

void completer_push_modelized(completer_t *c, model_t model, void *ptr)
{
    // TODO
}

static int strcmpp(const void *p1, const void *p2, void *UNUSED(arg))
{
    return strcmp(*(char * const *) p1, *(char * const *) p2);
}

static void completer_sort(completer_t *c)
{
    dptrarray_sort(c->ary, strcmpp, NULL);
}

static void completer_destroy(completer_t *c)
{
    dptrarray_destroy(c->ary);
    free(c);
}

static void completer_to_iterator(Iterator *it, completer_t *c)
{
    return dptrarray_to_iterator(it, c->ary);
}

static size_t completer_length(completer_t *c)
{
    return dptrarray_length(c->ary);
}

static const char *completer_at(completer_t *c, size_t offset)
{
    return dptrarray_at_unsafe(c->ary, offset, const char);
}

#define CREATE_ARG(node, arg_type, string_or_hint) \
    do { \
        node = mem_new(*node); \
        node->command_data = NULL; \
        node->completion_data = NULL; \
        node->handle = NULL; \
        node->type = arg_type; \
        node->complete = NULL; \
        node->offset = (size_t) -1; \
        node->string = string_or_hint; \
        node->children = NULL; \
        node->description = NULL; \
    } while (0);

static void graph_node_destroy(void *data)
{
    graph_node_t *node;

    node = (graph_node_t *) data;
    if (NULL != node->children) {
        dptrarray_destroy(node->children);
    }
    free(node);
}

graph_t *graph_new(void)
{
    graph_t *g;

    g = mem_new(*g);
    g->possibilities = completer_new();
    CREATE_ARG(g->end, ARG_TYPE_END, "(END)");
    g->roots = hashtable_ascii_cs_new(NULL, NULL, graph_node_destroy);
    g->nodes = hashtable_new(value_hash, value_equal, NULL, NULL, graph_node_destroy);

    return g;
}

static bool complete_literal(void *UNUSED(parsed_arguments), const char *current_argument, size_t current_argument_len, completer_t *possibilities, void *data)
{
    if (0 == strncmp(current_argument, (const char *) data, current_argument_len)) {
        completer_push(possibilities, (void *) data, FALSE);
    }

    return TRUE;
}

argument_t *argument_create_uint(size_t offset, const char *string)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_NUMBER, string);
    node->offset = offset;

    return node;
}

argument_t *argument_create_literal(const char *string, handle_t handle, const char *description)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_LITERAL, string);
    node->handle = handle;
    node->completion_data = (void *) string;
    node->complete = complete_literal;
    node->description = description;

    return node;
}

argument_t *argument_create_relevant_literal(size_t offset, const char *string, handle_t handle)
{
    argument_t *node;

    node = argument_create_literal(string, handle, NULL);
    node->offset = offset;

    return node;
}

static bool complete_choices(void *UNUSED(parsed_arguments), const char *current_argument, size_t current_argument_len, completer_t *possibilities, void *data)
{
    const char * const *v;

    for (v = (const char * const *) data; NULL != *v; v++) {
        if (0 == strncmp(current_argument, *v, current_argument_len)) {
            completer_push(possibilities, *v, FALSE);
        }
    }

    return TRUE;
}

argument_t *argument_create_choices(size_t offset, const char *hint, const char * const * values)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_CHOICES, hint);
    node->offset = offset;
    node->complete = complete_choices;
    node->completion_data = (void *) values;

    return node;
}

static const char * const off_on[] = {
    "off",
    "on",
    NULL
};

argument_t *argument_create_choices_off_on(size_t offset, handle_t handle)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_CHOICES, "<on/off>");
    node->handle = handle;
    node->offset = offset;
    node->complete = complete_choices;
    node->completion_data = (void *) off_on;

    return node;
}

static const char * const disable_enable[] = {
    "disable",
    "enable",
    NULL
};

argument_t *argument_create_choices_disable_enable(size_t offset, handle_t handle)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_CHOICES, "<enable/disable>");
    node->handle = handle;
    node->offset = offset;
    node->complete = complete_choices;
    node->completion_data = (void *) disable_enable;

    return node;
}

argument_t *argument_create_string(size_t offset, const char *hint, complete_t complete, void *data)
{
    argument_t *node;

    CREATE_ARG(node, ARG_TYPE_STRING, hint);
    node->offset = offset;
    node->complete = complete;
    node->completion_data = data;

    return node;
}

void graph_destroy(graph_t *g)
{
    assert(NULL != g);

    hashtable_destroy(g->roots);
    hashtable_destroy(g->nodes);
    completer_destroy(g->possibilities);
    free(g);
}

static int graph_node_compare(graph_node_t *a, graph_node_t *b)
{
    if (a == b) {
        return 0;
    } else if (NULL == a || NULL == b) {
        return (NULL != a) - (NULL != b);
    } else {
        int diff;

        if (0 == (diff = a->type - b->type)) {
            return strcmp(a->string, b->string);
        } else {
            return diff;
        }
    }
}

static void graph_node_insert_child(graph_t *g, graph_node_t *parent, graph_node_t *child)
{
    size_t i, l;
    graph_node_t *current;

    current = NULL;
    if (NULL == parent->children) {
        parent->children = dptrarray_new(NULL, NULL, NULL);
        hashtable_direct_put(g->nodes, HT_PUT_ON_DUP_KEY_PRESERVE, child, child, NULL);
        dptrarray_push(parent->children, child);
    } else {
        for (i = 0, l = dptrarray_length(parent->children); i < l; i++) {
            current = dptrarray_at_unsafe(parent->children, i, graph_node_t);
            if (!(graph_node_compare(child, current) > 0)) {
                break;
            }
        }
        if (0 == graph_node_compare(child, current)) {
//             if (child->type == ARG_TYPE_END) {
//                 free(child);
//             }
            return;
        }
        hashtable_direct_put(g->nodes, HT_PUT_ON_DUP_KEY_PRESERVE, child, child, NULL);
        if (NULL == current) {
            dptrarray_push(parent->children, child);
        } else {
            dptrarray_insert(parent->children, i, child);
        }
    }
}

// créer un chemin complet
void graph_create_full_path(graph_t *g, graph_node_t *start, ...) /* SENTINEL */
{
    va_list nodes;
    graph_node_t /**end, */*node, *parent;

    assert(NULL != g);
    assert(NULL != start);
    assert(ARG_TYPE_LITERAL == start->type);

    parent = start;
    hashtable_put(g->roots, HT_PUT_ON_DUP_KEY_PRESERVE, start->string, start, NULL);
    va_start(nodes, start);
    while (NULL != (node = va_arg(nodes, graph_node_t *))) {
        graph_node_insert_child(g, parent, node);
        parent = node;
    }
    va_end(nodes);
//     CREATE_ARG(end, ARG_TYPE_END, "(END)");
    graph_node_insert_child(g, parent, g->end);
}

// créer un chemin entre 2 sommets (end peut être NULL)
// vu que le point de départ doit exister, ça ne peut être une "racine"
void graph_create_path(graph_t *g, graph_node_t *start, graph_node_t *end, ...) /* SENTINEL */
{
    va_list nodes;
    graph_node_t *node, *parent;

    assert(NULL != g);
    assert(NULL != start);

    parent = start;
    va_start(nodes, end);
    while (NULL != (node = va_arg(nodes, graph_node_t *))) {
        graph_node_insert_child(g, parent, node);
        parent = node;
    }
    va_end(nodes);
    if (NULL == end) {
//         CREATE_ARG(end, ARG_TYPE_END, "(END)");
        graph_node_insert_child(g, parent, g->end);
    } else {
        graph_node_insert_child(g, parent, end);
    }
//     graph_node_insert_child(g, parent, end);
}

#define MAX_ALTERNATE_PATHS 12
// créer tous les chemins/permutations possibles entre start et end (1 - 2 - 3, 1 - 3 - 2, 2 - 1 - 3, 2 - 3 - 1, 3 - 1 - 2, 3 - 2 - 1)
/**
 * Creates all possible subpaths (permutations) of groups of nodes between start and end.
 *
 * Each group of nodes should be given as: number of nodes in the group then the list of the nodes from left to right.
 * The end of the list (sentinel) is represented by a subgroup of 0 nodes.
 *
 * end can be NULL to represent the end of the command line.
 * start and end (except if last one is NULL) have to be already inserted in the graph.
 *
 * Example:
 *   graph_create_all_path(g, start, end, 2, a, b, 1, c, 2, d, e, 0);
 *
 * Creates all these paths:
 *   - start - (a - b) - (c) - (d - e) - end
 *   - start - (a - b) - (d - e) - (c) - end
 *   - start - (c) - (a - b) - (d - e) - end
 *   - start - (c) - (d - e) - (a - b) - end
 *   - start - (d - e) - (a - b) - (c) - end
 *   - start - (d - e) - (c) - (a - b) - end
 */
#if 0
void graph_create_all_path(graph_t *g, graph_node_t *start, graph_node_t *end, ...)
{
    va_list ap;
    size_t i, group_count, subpaths_count;
    graph_node_t *real_end, *parent, *node, *starts[MAX_ALTERNATE_PATHS], *ends[MAX_ALTERNATE_PATHS];

    assert(NULL != start);

    real_end = end;
    if (NULL == end) {
        real_end = g->end;
    }
    va_start(ap, end);
    subpaths_count = 0;
    while (0 != (group_count = va_arg(ap, int))) {
        assert(subpaths_count < MAX_ALTERNATE_PATHS);
        starts[subpaths_count] = node = va_arg(ap, graph_node_t *);
        assert(NULL != node);
        graph_node_insert_child(g, start, node);
        parent = node;
        // link nodes of subgroup between them if not yet done
        for (i = 2; i <= group_count; i++) {
            node = va_arg(ap, graph_node_t *);
            assert(NULL != node);
            graph_node_insert_child(g, parent, node);
            parent = node;
        }
        ends[subpaths_count] = parent;
#if 0
        graph_node_insert_child(g, parent, real_end);
#endif
        ++subpaths_count;
    }
    va_end(ap);
    for (i = 0; i < subpaths_count; i++) {
        size_t j;

        for (j = 0; j < subpaths_count; j++) {
            if (j != i) {
                graph_node_insert_child(g, ends[i], starts[j]);
            }
#if 1
//             graph_node_insert_child(g, ends[i], real_end);
#endif
        }
    }
}
#endif

struct subpath {
    graph_node_t *start, *end;
};

static void swap_subpaths(struct subpath *a, struct subpath *b)
{
    struct subpath tmp;

    tmp = *a;
    *a = *b;
    *b = tmp;
}

static void graph_generate_subpath(graph_t *g, graph_node_t *start, graph_node_t *end, struct subpath *subpaths, size_t subpaths_len, size_t n)
{
    size_t i;

    if (1 == n) {
        graph_node_insert_child(g, start, subpaths[0].start);
//         printf(" > %s", subpaths[0].start->string);
        for (i = 1; i < subpaths_len; i++) {
//             printf(" > %s", subpaths[i].start->string);
            graph_node_insert_child(g, subpaths[i - 1].end, subpaths[i].start);
        }
        graph_node_insert_child(g, subpaths[i - 1].end, end);
//         printf("\n");
    } else {
        for (i = 0; i < n - 1; i++) {
            graph_generate_subpath(g, start, end, subpaths, subpaths_len, n - 1);
            if (n & 1) {
                swap_subpaths(&subpaths[0], &subpaths[n - 1]);
            } else {
                swap_subpaths(&subpaths[i], &subpaths[n - 1]);
            }
        }
        graph_generate_subpath(g, start, end, subpaths, subpaths_len, n - 1);
    }
}

void graph_create_all_path(graph_t *g, graph_node_t *start, graph_node_t *end, ...)
{
    va_list ap, aq;
    struct subpath *subpaths;
    graph_node_t *node, *parent;
    size_t i, group_count, subpaths_count;

    va_start(ap, end);
    va_copy(aq, ap);
    subpaths_count = 0;
    while (0 != (group_count = va_arg(aq, int))) {
        for (i = 1; i <= group_count; i++) {
            va_arg(aq, graph_node_t *);
        }
        ++subpaths_count;
    }
    va_end(aq);
    subpaths = mem_new_n(*subpaths, subpaths_count);
    subpaths_count = 0;
    while (0 != (group_count = va_arg(ap, int))) {
        subpaths[subpaths_count].start = node = va_arg(ap, graph_node_t *);
        assert(NULL != node);
        graph_node_insert_child(g, start, node);
        parent = node;
        // link nodes of subgroup between them if not yet done
        for (i = 2; i <= group_count; i++) {
            node = va_arg(ap, graph_node_t *);
            assert(NULL != node);
            graph_node_insert_child(g, parent, node);
            parent = node;
        }
        subpaths[subpaths_count].end = parent;
        ++subpaths_count;
    }
    va_end(ap);
    if (NULL == end) {
        end = g->end;
    }
    graph_generate_subpath(g, start, end, subpaths, subpaths_count, subpaths_count);
    free(subpaths);
}

static void traverse_graph_node_ex(graph_node_t *node, HashTable *visited, int depth, bool indent, const char *description)
{
    size_t i, l;
    bool has_end;
    size_t children_count;
    graph_node_t *current;

    if (ARG_TYPE_LITERAL == node->type && !hashtable_direct_put(visited, HT_PUT_ON_DUP_KEY_PRESERVE, node, node, NULL)) {
        return;
    }
    has_end = FALSE;
    children_count = 0;
    for (i = 0, l = dptrarray_length(node->children); i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        ++children_count;
        has_end |= ARG_TYPE_END == current->type;
    }
    if (indent) {
        printf("%*c", depth * 4, ' ');
    }
    putchar(' ');
    fputs(node->string, stdout);
    if (has_end) {
        if (NULL != node->description) {
            description = node->description;
        }
        if (NULL != description) {
            fputs(" => ", stdout);
            fputs(description, stdout);
        }
        description = NULL;
    } else {
        description = node->description;
    }
    if (children_count > 1 || has_end) {
        putchar('\n');
    }
    for (i = 0/*, l = dptrarray_length(node->children)*/; i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        if (ARG_TYPE_END != current->type) {
            traverse_graph_node_ex(current, visited, 1 == children_count ? depth : depth + 1, 1 != children_count, description);
        }
    }
}

static void traverse_graph_node(graph_node_t *node, int depth, bool indent)
{
    size_t i, l;
    bool has_end;
    size_t children_count;
    graph_node_t *current;

    has_end = FALSE;
    children_count = 0;
    for (i = 0, l = dptrarray_length(node->children); i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        ++children_count;
        has_end |= ARG_TYPE_END == current->type;
    }
    if (indent) {
        printf("%*c", depth * 4, ' ');
    }
    putchar(' ');
    fputs(node->string, stdout);
    if (children_count > 1 || has_end) {
        putchar('\n');
    }
    for (i = 0/*, l = dptrarray_length(node->children)*/; i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        if (ARG_TYPE_END != current->type) {
            traverse_graph_node(current, 1 == children_count ? depth : depth + 1, 1 != children_count);
        }
    }
}

/* <TODO: DRY (share this with main.c)> */
typedef struct {
    graph_t *graph;
    Tokenizer *tokenizer;
} editline_data_t;
/* </TODO: DRY> */

bool complete_from_statement(void *UNUSED(parsed_arguments), const char *current_argument, size_t UNUSED(current_argument_len), completer_t *possibilities, void *data)
{
    char *v;
    Iterator it;
    sqlite_statement_t *stmt;

    assert(NULL != data);
    stmt = (sqlite_statement_t *) data;
    statement_bind(stmt, NULL, current_argument);
    statement_to_iterator(&it, stmt, &v); // TODO: bind only current_argument_len first characters of current_argument?
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        iterator_current(&it, NULL);
        completer_push(possibilities, v, TRUE);
    }
    iterator_close(&it);

    return TRUE;
}

bool complete_from_hashtable_keys(void *UNUSED(parsed_arguments), const char *current_argument, size_t current_argument_len, completer_t *possibilities, void *data)
{
    Iterator it;

    assert(NULL != data);
    hashtable_to_iterator(&it, (HashTable *) data);
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        void *k;

        iterator_current(&it, &k);
        if (0 == strncmp(current_argument, (const char *) k, current_argument_len)) {
            completer_push(possibilities, k, FALSE);
        }
    }
    iterator_close(&it);

    return TRUE;
}

typedef struct {
    const char *name; // unused
    bool (*matcher)(argument_t *, const char *); // exact match (use strcmp, not strncmp - this is not intended for completion)
} argument_description_t;

static bool argument_literal_match(argument_t *arg, const char *value)
{
    return '\0' != *value && 0 == strcmp(arg->string, value);
}

static bool argument_choices_match(argument_t *arg, const char *value)
{
    const char * const *v;

    if ('\0' != *value) {
        for (v = (const char * const *) arg->completion_data; NULL != *v; v++) {
            if (0 == strcmp(value, *v)) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

static bool argument_number_match(argument_t *arg, const char *value)
{
    bool match;

    // ARG_TYPE_NUMBER acts as ^\d+$
    for (match = '\0' != *value; match && '\0' != *value; value++) {
        match &= *value >= '0' && *value <= '9';
    }

    return match;
}

static bool argument_string_match(argument_t *UNUSED(arg), const char *UNUSED(value))
{
    return TRUE; // ARG_TYPE_STRING match everything (acts as .*)
}

static bool argument_end_match(argument_t *UNUSED(arg), const char *UNUSED(value))
{
    return FALSE; // ARG_TYPE_END match nothing, it's a virtual dummy node
}

static argument_description_t descriptions[] = {
    [ ARG_TYPE_END ]     = { "",              argument_end_match },
    [ ARG_TYPE_NUMBER ]  = { "number",        argument_number_match },
    [ ARG_TYPE_LITERAL ] = { "literal",       argument_literal_match },
    [ ARG_TYPE_CHOICES ] = { "choices",       argument_choices_match },
    [ ARG_TYPE_STRING ]  = { "a free string", argument_string_match }
};

static graph_node_t *graph_node_find(graph_node_t *parent, const char *value)
{
    if (NULL != parent->children) {
        size_t i, l;

        for (i = 0, l = dptrarray_length(parent->children); i < l; i++) {
            graph_node_t *child;

            child = dptrarray_at_unsafe(parent->children, i, graph_node_t);
            if (NULL == descriptions[child->type].matcher || descriptions[child->type].matcher(child, value)) {
                return child;
            }
        }
    }

    return NULL;
}

#define SEPARATOR ", "
static graph_node_t *graph_node_find_ex(graph_node_t *parent, const char *value, error_t **error)
{
    if (NULL != parent->children) {
        size_t i, l;

        for (i = 0, l = dptrarray_length(parent->children); i < l; i++) {
            graph_node_t *child;

            child = dptrarray_at_unsafe(parent->children, i, graph_node_t);
            if (NULL == descriptions[child->type].matcher || descriptions[child->type].matcher(child, value)) {
                return child;
            }
        }
        if (1 == l) {
            graph_node_t *child;

            child = dptrarray_at_unsafe(parent->children, 0, graph_node_t);
            if (ARG_TYPE_END == child->type) {
                error_set(error, NOTICE, _("too many arguments (%s)"), value);
                return NULL;
            }
        }/* else */
        {
            String *buffer;

            buffer = string_new();
            for (i = 0/*, l = dptrarray_length(parent->children)*/; i < l; i++) {
                graph_node_t *child;

                child = dptrarray_at_unsafe(parent->children, i, graph_node_t);
                switch (child->type) {
                    case ARG_TYPE_LITERAL:
                        STRING_APPEND_STRING(buffer, SEPARATOR);
                        string_append_string(buffer, child->string);
                        break;
                    case ARG_TYPE_CHOICES:
                    {
                        const char * const *v;

                        for (v = (const char * const *) child->completion_data; NULL != *v; v++) {
                            STRING_APPEND_STRING(buffer, SEPARATOR);
                            string_append_string(buffer, *v);
                        }
                        break;
                    }
                    default:
                        // NOP?
                        break;
                }
            }
            error_set(error, NOTICE, _("got %s, expect one of: %s"), value, buffer->ptr + STR_LEN(SEPARATOR));
            string_destroy(buffer);
        }
    }

    return NULL;
}

static bool graph_node_end_in_children(graph_node_t *node)
{
    if (NULL != node->children) {
        size_t i, l;

        for (i = 0, l = dptrarray_length(node->children); i < l; i++) {
            graph_node_t *child;

            child = dptrarray_at_unsafe(node->children, i, graph_node_t);
            if (ARG_TYPE_END == child->type) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

#if 0
#include <math.h>
static size_t my_vsnprintf(void *args, char *dst, size_t dst_size, const char *fmt, va_list ap)
{
    char *w;
    va_list cpy;
    const char *r;
    size_t dst_len;

    w = dst;
    r = fmt;
    dst_len = 0;
    va_copy(cpy, ap);
    while ('\0' != *r) {
        if ('%' == *r) {
            ++r;
            switch (*r) {
                case '%':
                    ++dst_len;
                    if (dst_size > dst_len) {
                        *w++ = '%';
                    }
                    break;
                case 'S':
                {
                    const char *s;
                    size_t offset, s_len;

                    offset = va_arg(cpy, size_t);
                    s = *((const char **) (args + offset));
                    s_len = strlen(s);
                    dst_len += s_len;
                    if (dst_size > dst_len) {
                        memcpy(w, s, s_len);
                        w += s_len;
                    }
                    break;
                }
                case 's':
                {
                    size_t s_len;
                    const char *s;

                    s = va_arg(cpy, const char *);
                    s_len = strlen(s);
                    dst_len += s_len;
                    if (dst_size > dst_len) {
                        memcpy(w, s, s_len);
                        w += s_len;
                    }
                    break;
                }
                case 'U':
                {
                    // TODO
                    break;
                }
                case 'u':
                {
                    uint32_t num;
                    size_t num_len;

                    num = va_arg(cpy, uint32_t);
                    num_len = log10(num) + 1;
                    dst_len += num_len;
                    if (dst_size > dst_len) {
                        size_t i;

                        i = num_len;
                        do {
                            w[i--] = '0' + num % 10;
                            num /= 10;
                        } while (0 != num);
                        w += num_len;
                    }
                    break;
                }
            }
        } else {
            ++dst_len;
            if (dst_size > dst_len) {
                *w++ = *r;
            }
        }
        ++r;
    }
    va_end(cpy);
    if (dst_size > dst_len) {
        *w++ = '\0';
    }

    return dst_len;
}
#endif

static int string_array_to_index(argument_t *arg, const char *value)
{
    const char * const *v;
    const char * const *values;

    assert(ARG_TYPE_CHOICES == arg->type);
    values = (const char * const *) arg->completion_data;
    for (v = values; NULL != *v; v++) {
        if (0 == strcmp(value, *v)) {
            return v - values;
        }
    }

    return -1;
}

static size_t longest_prefix(const char *s, char *prefix)
{
    char *p;

    for (p = prefix; *s++ == *p && '\0' != *p; ++p)
        ;
    *p = '\0';

    return p - prefix;
}

// TODO:
// - separate completion (2 DPtrArray ?) to distinguish commands to values (eg: domain<tab> will propose "bar.tld", "foo.tld", "list", "xxx.tld")
// - indicate the command is complete? (eg: domain list<tab>)
unsigned char graph_complete(EditLine *el, int UNUSED(ch))
{
    const char **argv;
    const LineInfo *li;
    editline_data_t *client_data;
    int argc, res, cursorc, cursoro;

    res = CC_ERROR;
    li = el_line(el);
    if (0 != el_get(el, EL_CLIENTDATA, &client_data)) {
        return res;
    }
    tok_reset(client_data->tokenizer);
    completer_clear(client_data->graph->possibilities);
    if (-1 == tok_line(client_data->tokenizer, li, &argc, &argv, &cursorc, &cursoro)) { // TODO: handle cases tok_line returns value > 0
        return res;
    } else {
        argument_t *arg;
        char arguments[8192];

        bzero(arguments, ARRAY_SIZE(arguments));
        if (0 == cursorc) {
            Iterator it;

            hashtable_to_iterator(&it, client_data->graph->roots);
            for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
                arg = iterator_current(&it, NULL);
                assert(ARG_TYPE_LITERAL == arg->type);
                if (0 == strncmp(arg->string, argv[0], cursoro)) {
                    completer_push(client_data->graph->possibilities, (void *) arg->string, FALSE);
                }
            }
            iterator_close(&it);
        } else {
            if (hashtable_get(client_data->graph->roots, argv[0], &arg)) {
                int depth;

                for (depth = 1; depth < cursorc && NULL != arg; depth++) {
                    if (NULL == (arg = graph_node_find(arg, NULL == argv[depth] ? "" : argv[depth]))) {
                        // too much parameters
                        break;
                    }
                    if (((size_t) -1) != arg->offset) {
                        if (ARG_TYPE_LITERAL == arg->type) {
                            *((bool *) (arguments + arg->offset)) = TRUE;
                        } else if (ARG_TYPE_CHOICES == arg->type) {
                            *((int *) (arguments + arg->offset)) = string_array_to_index(arg, argv[depth]);
                        } else if (ARG_TYPE_NUMBER == arg->type) {
                            *((uint32_t *) (arguments + arg->offset)) = (uint32_t) strtoull(argv[depth], NULL, 10);
                        } else {
                            *((const char **) (arguments + arg->offset)) = argv[depth];
                        }
                    }
                }
                if (NULL != arg) {
                    size_t i, l;

                    for (i = 0, l = dptrarray_length(arg->children); i < l; i++) {
                        graph_node_t *child;

                        child = dptrarray_at_unsafe(arg->children, i, graph_node_t);
                        if (NULL != child->complete) {
                            child->complete((void *) arguments, NULL == argv[cursorc] ? "" : argv[cursorc], cursoro, client_data->graph->possibilities, child->completion_data);
                        }
                    }
                }
            }
        }
    }
    switch (completer_length(client_data->graph->possibilities)) {
        case 0:
            res = CC_ERROR;
            break;
        case 1:
            if (-1 == el_insertstr(el, completer_at(client_data->graph->possibilities, 0) + cursoro)) {
                res = CC_ERROR;
            } else {
                res = CC_REFRESH;
            }
#if TODO
            if (FALSE) { // TODO: add space if more arguments are expected
                if (-1 == el_insertstr(el, " ")) {
                    res = CC_ERROR;
                } else {
                    res = CC_REFRESH;
                }
            }
#endif
            break;
        default:
        {
            Iterator it;
            char prefix[1024];
            size_t prefix_len;
            const char *v;

            puts("");
            *prefix = '\0';
            prefix_len = 0;
            completer_sort(client_data->graph->possibilities);
            completer_to_iterator(&it, client_data->graph->possibilities);
#if 1
            iterator_first(&it);
            v = iterator_current(&it, NULL); // this is safe because if we are here, we know client_data->graph->possibilities has at least 2 entries
            prefix_len = strlen(v);
            strncpy(prefix, v, ARRAY_SIZE(prefix));
            do {
                fputc('\t', stdout);
                v = iterator_current(&it, NULL);
                if (0 != prefix_len) { // if there is no common prefix, don't continue to look for one
                    prefix_len = longest_prefix(v, prefix);
                }
                fputs(v, stdout);
                fputc('\n', stdout);
                iterator_next(&it);
            } while (iterator_is_valid(&it));
            if (prefix_len) {
                el_insertstr(el, prefix + cursoro);
            }
#else
            for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
                fputc('\t', stdout);
                fputs(iterator_current(&it, NULL), stdout);
                fputc('\n', stdout);
            }
#endif
            iterator_close(&it);
            res = CC_REDISPLAY;
            break;
        }
    }

    return res;
}

command_status_t graph_dispatch_command(graph_t *g, int args_count, const char **args, const main_options_t *mainopts, error_t **error)
{
    char arguments[8192];
    command_status_t ret;
    argument_t *prev_arg, *arg;

    ret = COMMAND_USAGE;
    if (args_count < 1) {
        return 0;
    }
    bzero(arguments, ARRAY_SIZE(arguments));
    if (hashtable_get(g->roots, args[0], &arg)) {
        int depth;
        handle_t handle;

        handle = arg->handle;
        for (depth = 1; depth < args_count && NULL != arg; depth++) {
            prev_arg = arg;
            if (NULL == (arg = graph_node_find_ex(arg, args[depth], error))) {
//                 error_set(error, NOTICE, "too many arguments"); // also a literal or a choice does not match what is planned (eg: domain foo.tld dnssec on - "on" instead of status/enable/disable)
//                 traverse_graph_node(prev_arg, 0, TRUE);
                handle = NULL;
                return COMMAND_USAGE;
            }
            if (((size_t) -1) != arg->offset) {
                if (ARG_TYPE_LITERAL == arg->type) {
                    *((bool *) (arguments + arg->offset)) = TRUE;
                } else if (ARG_TYPE_CHOICES == arg->type) {
                    *((int *) (arguments + arg->offset)) = string_array_to_index(arg, args[depth]); // this is safe: graph_node_find_ex already checked that the value is one among the values we predefined
                } else if (ARG_TYPE_NUMBER == arg->type) {
                    *((uint32_t *) (arguments + arg->offset)) = (uint32_t) strtoull(args[depth], NULL, 10); // TODO: check this is a valid integer
                } else {
                    *((const char **) (arguments + arg->offset)) = args[depth];
                }
            }
            if (NULL != arg && NULL != arg->handle) {
                handle = arg->handle;
            }
        }
        if (NULL == handle || !graph_node_end_in_children(arg)) {
            error_set(error, NOTICE, _("unterminated command: argument(s) missing"));
            traverse_graph_node(arg, 0, TRUE);
        } else {
            ret = handle((void *) arguments, /* TODO: arg->command_data, */ mainopts, error);
        }
    } else {
        graph_display(g);
        error_set(error, NOTICE, _("unknown command"));
    }

    return ret;
}

void graph_display(graph_t *g)
{
    Iterator it;
    HashTable *visited;

    visited = hashtable_new(value_hash, value_equal, NULL, NULL, NULL);
    hashtable_to_iterator(&it, g->roots);
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        argument_t *arg;

        arg = iterator_current(&it, NULL);
        traverse_graph_node_ex(arg, visited, 0, TRUE, arg->description);
    }
    iterator_close(&it);
    hashtable_destroy(visited);
}

static void traverse_graph_node_for_bash(graph_node_t *node, HashTable *visited, String *content, String *path)
{
    bool has_end;
    size_t i, l, path_len;
    size_t children_count;
    graph_node_t *current;

    if (ARG_TYPE_LITERAL == node->type && !hashtable_direct_put(visited, HT_PUT_ON_DUP_KEY_PRESERVE, node, node, NULL)) {
        return;
    }
    has_end = FALSE;
    children_count = 0;
    for (i = 0, l = dptrarray_length(node->children); i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        ++children_count;
        has_end |= ARG_TYPE_END == current->type;
    }
//     if (indent) {
//         printf("%*c", depth * 4, ' ');
//     }
//     putchar(' ');
//     fputs(node->string, stdout);
//     if (children_count > 1 || has_end) {
//         putchar('\n');
//     }
    path_len = path->len;
    STRING_APPEND_STRING(content, "        [\"");
    switch (node->type) {
        default:
        case ARG_TYPE_STRING:
        case ARG_TYPE_CHOICES:
            string_append_char(path, '*');
            break;
        case ARG_TYPE_LITERAL:
            string_append_string(path, node->string);
            break;
//         default:
//             assert(FALSE);
//             break;
    }
    string_append_char(path, '/');
    string_append_string_len(content, path->ptr, path->len);
    STRING_APPEND_STRING(content, /*"/"*/"\"]=\" ");
    for (i = 0/*, l = dptrarray_length(node->children)*/; i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        switch (current->type) {
            case ARG_TYPE_CHOICES:
            {
                const char * const *v;

                for (v = (const char * const *) current->completion_data; NULL != *v; v++) {
                    string_append_string(content, *v);
                    string_append_char(content, ' ');
                }
                break;
            }
            case ARG_TYPE_LITERAL:
                string_append_string(content, current->string);
                string_append_char(content, ' ');
                break;
            default:
                // NOP: nothing to append to the buffer as this is not a known value
                break;
        }
    }
    STRING_APPEND_STRING(content, "\"\n");
    for (i = 0/*, l = dptrarray_length(node->children)*/; i < l; i++) {
        current = dptrarray_at_unsafe(node->children, i, graph_node_t);
        if (ARG_TYPE_END != current->type) {
            traverse_graph_node_for_bash(current, visited, content, path);
        }
    }
    path->ptr[path->len = path_len] = '\0';
}

char *graph_bash(graph_t *g)
{
    Iterator it;
    HashTable *visited;
    String *content, *path;

    content = string_new();
    path = string_dup_string_len("/", STR_LEN("/"));
    STRING_APPEND_STRING(content, "_ovh() {\n\
    declare -rA X=(\n");
    visited = hashtable_new(value_hash, value_equal, NULL, NULL, NULL);
    hashtable_to_iterator(&it, g->roots);
    STRING_APPEND_STRING(content, "        [\"/\"]=\"");
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        argument_t *arg;

        arg = iterator_current(&it, NULL);
        string_append_string(content, arg->string);
        string_append_char(content, ' ');
    }
    STRING_APPEND_STRING(content, "\"\n");
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        argument_t *arg;

        arg = iterator_current(&it, NULL);
        traverse_graph_node_for_bash(arg, visited, content, path);
    }
    iterator_close(&it);
    hashtable_destroy(visited);
    STRING_APPEND_STRING(content, "\
    )\n\
    local cur=${COMP_WORDS[COMP_CWORD]}\n\
\n\
    i=1\n\
    COMP_PATH=\"/\"\n\
    while [ $i -lt $COMP_CWORD ]; do\n\
        if [ -n \"${X[${COMP_PATH}${COMP_WORDS[i]}/]}\" ]; then\n\
            COMP_PATH=\"${COMP_PATH}${COMP_WORDS[i]}/\"\n\
        else\n\
            COMP_PATH=\"${COMP_PATH}*/\"\n\
        fi\n\
        let i=i+1\n\
    done\n\
\n\
    if [ -n \"${X[$COMP_PATH]}\" ]; then\n\
        COMPREPLY=( $(compgen -W \"${X[$COMP_PATH]}\" -- $cur) )\n\
    fi\n\
}\n\
\n\
complete -F _ovh ovh");
    string_destroy(path);

    return string_orphan(content);
}
