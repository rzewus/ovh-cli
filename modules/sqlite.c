#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include "common.h"
#include "command.h"
#include "struct/xtring.h"
#include "modules/home.h"
#include "modules/table.h"
#include "modules/sqlite.h"
#include "commands/account.h"

#ifdef HAVE_LIBBSD_STRLCPY
# include <bsd/string.h>
#endif /* HAVE_LIBBSD_STRLCPY */

typedef enum {
    SQLITE_TYPE_BOOL,
    SQLITE_TYPE_BOOLEAN = SQLITE_TYPE_BOOL,
    SQLITE_TYPE_INT,
    SQLITE_TYPE_INT64,
    SQLITE_TYPE_STRING,
    SQLITE_TYPE_IGNORE
} sqlite_bind_type_t;

typedef struct {
    sqlite_bind_type_t type;
    void *ptr;
} sqlite_statement_bind_t;

typedef enum {
    SSST_MODEL_BASED,
    SSST_INDIVIDUAL_BINDS,
} sqlite_statement_state_type_t;

typedef struct {
    /**
     * Keep result of last sqlite3_step call
     */
    int ret;
    sqlite_statement_state_type_t type;
    union {
        struct {
            size_t output_binds_count;
            sqlite_statement_bind_t *output_binds;
        };
        struct {
            bool copy;
            const model_t *model;
        };
    };
} sqlite_statement_state_t;

static sqlite3 *db;
static int user_version;
static char db_path[MAXPATHLEN];

static void bool_input_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field)
{
    sqlite3_bind_int(stmt, no, VOIDP_TO_X(ptr, field->offset, bool));
}

static void bool_output_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field, bool UNUSED(copy))
{
    VOIDP_TO_X(ptr, field->offset, bool) = /*!!*/sqlite3_column_int(stmt, no);
}

static void int_input_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field)
{
    sqlite3_bind_int(stmt, no, VOIDP_TO_X(ptr, field->offset, int));
}

static void int_output_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field, bool UNUSED(copy))
{
    VOIDP_TO_X(ptr, field->offset, int) = sqlite3_column_int(stmt, no);
}

static void time_t_intput_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field)
{
    sqlite3_bind_int64(stmt, no, VOIDP_TO_X(ptr, field->offset, time_t));
}

static void time_t_output_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field, bool UNUSED(copy))
{
    VOIDP_TO_X(ptr, field->offset, time_t) = sqlite3_column_int64(stmt, no);
}

static void string_intput_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field)
{
    sqlite3_bind_text(stmt, no, VOIDP_TO_X(ptr, field->offset, char *), -1, SQLITE_TRANSIENT);
}

static void string_output_bind(sqlite3_stmt *stmt, int no, void *ptr, const model_field_t *field, bool copy)
{
    char *uv;
    const unsigned char *sv;

    sv = sqlite3_column_text(stmt, no);
    if (NULL == sv) {
        uv = NULL;
    } else {
        uv = copy ? strdup((char *) sv) : (char *) sv;
    }
    VOIDP_TO_X(ptr, field->offset, char *) = uv;
}

#define STR(s) \
    s, STR_LEN(s)

static struct {
    const char *sqlite_type;
    size_t sqlite_type_len;
    void (*set_input_bind)(sqlite3_stmt *, int, void *, const model_field_t *);
    void (*set_output_bind)(sqlite3_stmt *, int, void *, const model_field_t *, bool);
} model_types_callbacks[] = {
    [ MODEL_TYPE_INT ]      = { STR("INT"),  int_input_bind,     int_output_bind },
    [ MODEL_TYPE_BOOL ]     = { STR("INT"),  bool_input_bind,    bool_output_bind, },
    [ MODEL_TYPE_DATE ]     = { STR("INT"),  time_t_intput_bind, time_t_output_bind },
    [ MODEL_TYPE_ENUM ]     = { STR("INT"),  int_input_bind,     int_output_bind },
    [ MODEL_TYPE_STRING ]   = { STR("TEXT"), string_intput_bind, string_output_bind },
    [ MODEL_TYPE_DATETIME ] = { STR("INT"),  time_t_intput_bind, time_t_output_bind },
};

/**
 * TODO:
 * - merge sqlite_statement_state_t into sqlite_statement_t ?
 */

/**
 * NOTE:
 * - for sqlite3_column_* functions, the first column is 0
 * - in the other hand, for sqlite3_bind_* functions, the first parameter is 1
 */

enum {
    // account
    STMT_GET_USER_VERSION,
    STMT_SET_USER_VERSION,
    // count
    STMT_COUNT
};

static sqlite_statement_t statements[STMT_COUNT] = {
    [ STMT_GET_USER_VERSION ] = DECL_STMT("PRAGMA user_version", "", ""),
    [ STMT_SET_USER_VERSION ] = DECL_STMT("PRAGMA user_version = " STRINGIFY_EXPANDED(OVH_CLI_VERSION_NUMBER), "", ""), // PRAGMA doesn't permit parameter
};

static bool statement_iterator_is_valid(const void *UNUSED(collection), void **state)
{
    sqlite_statement_state_t *sss;

    assert(NULL != state);
    sss = *(sqlite_statement_state_t **) state;

    return SQLITE_ROW == sss->ret;
}

static void statement_iterator_first(const void *collection, void **state)
{
    sqlite_statement_t *stmt;
    sqlite_statement_state_t *sss;

    assert(NULL != state);
    assert(NULL != collection);

    stmt = (sqlite_statement_t *) collection;
    sss = *(sqlite_statement_state_t **) state;
    sss->ret = sqlite3_step(stmt->prepared);
}

static void statement_iterator_next(const void *collection, void **state)
{
    sqlite_statement_t *stmt;
    sqlite_statement_state_t *sss;

    assert(NULL != state);
    assert(NULL != collection);

    stmt = (sqlite_statement_t *) collection;
    sss = *(sqlite_statement_state_t **) state;
    sss->ret = sqlite3_step(stmt->prepared);
}

static void statement_iterator_current(const void *collection, void **state, void **UNUSED(value), void **UNUSED(key))
{
    sqlite_statement_t *stmt;
    sqlite_statement_state_t *sss;

    assert(NULL != state);
    assert(NULL != collection);

    stmt = (sqlite_statement_t *) collection;
    sss = *(sqlite_statement_state_t **) state;
    if (0 != sss->output_binds_count) {
        size_t i;

        for (i = 0; i < sss->output_binds_count; i++) {
            switch (sss->output_binds[i].type) {
                case SQLITE_TYPE_BOOL:
                    *((bool *) sss->output_binds[i].ptr) = /*!!*/sqlite3_column_int(stmt->prepared, i);
                    break;
                case SQLITE_TYPE_INT:
                    *((int *) sss->output_binds[i].ptr) = sqlite3_column_int(stmt->prepared, i);
                    break;
                case SQLITE_TYPE_INT64:
                    *((int64_t *) sss->output_binds[i].ptr) = sqlite3_column_int64(stmt->prepared, i);
                    break;
                case SQLITE_TYPE_STRING:
                {
                    const unsigned char *v;

                    if (NULL == (v = sqlite3_column_text(stmt->prepared, i))) {
                        *((char **) sss->output_binds[i].ptr) = NULL;
                    } else {
                        *((char **) sss->output_binds[i].ptr) = strdup((char *) v);
                    }
                    break;
                }
                case SQLITE_TYPE_IGNORE:
                    // NOP
                    break;
                default:
                    assert(FALSE);
                    break;
            }
        }
    }
}

static void statement_iterator_close(void *state)
{
    sqlite_statement_state_t *sss;

    assert(NULL != state);

    sss = (sqlite_statement_state_t *) state;
    if (0 != sss->output_binds_count) {
        free(sss->output_binds);
    }
    free(sss);
}

/**
 * Initializes the given iterator to loop on the result set associated
 * to the given prepared statement. The value of each column for each
 * row are dup to the given address.
 *
 * @param it the iterator to set
 * @param stmt the result set of the statement on which to iterate
 * @param ... a list of address where value are copied
 *
 * @note for strings, the value - if not NULL - is strdup. You have to
 * free it when you don't need it anymore.
 */
void statement_to_iterator(Iterator *it, sqlite_statement_t *stmt, ...)
{
    va_list ap;
    sqlite_statement_state_t *sss;

    va_start(ap, stmt);
    sss = mem_new(*sss);
    sss->type = SSST_INDIVIDUAL_BINDS;
    sss->output_binds_count = strlen(stmt->outbinds);
    if (0 == sss->output_binds_count) {
        sss->output_binds = NULL;
    } else {
        size_t i;

        sss->output_binds = mem_new_n(*sss->output_binds, sss->output_binds_count);
        for (i = 0; i < sss->output_binds_count; i++) {
            switch (stmt->outbinds[i]) {
                case 'b':
                    sss->output_binds[i].type = SQLITE_TYPE_BOOL;
                    break;
                case 'i':
                case 'e':
                    sss->output_binds[i].type = SQLITE_TYPE_INT;
                    break;
                case 'd':
                case 't':
                    sss->output_binds[i].type = SQLITE_TYPE_INT64;
                    break;
                case 's':
                    sss->output_binds[i].type = SQLITE_TYPE_STRING;
                    break;
                case ' ':
                case '-':
                    // ignore
                    sss->output_binds[i].type = SQLITE_TYPE_IGNORE;
                    break;
                default:
                    assert(FALSE);
                    break;
            }
            sss->output_binds[i].ptr = va_arg(ap, void *);
        }
    }
    va_end(ap);
    iterator_init(
        it, stmt, sss,
        statement_iterator_first, NULL,
        statement_iterator_current,
        statement_iterator_next, NULL,
        statement_iterator_is_valid,
        statement_iterator_close
    );
}

static void _statement_model_set_output_bind(sqlite_statement_t *stmt, const model_t *model, modelized_t *ptr, bool copy)
{
    int i, l;

    for (i = 0, l = sqlite3_column_count(stmt->prepared); i < l; i++) {
        size_t ovh_name_len;
        const char *ovh_name;
        const model_field_t *f;

        ovh_name = sqlite3_column_name(stmt->prepared, i);
        ovh_name_len = strlen(ovh_name);
        if (NULL != (f = model_find_field_by_name(model, ovh_name, ovh_name_len))) {
            assert(f->type >= 0 && f->type <= _MODEL_TYPE_LAST);
            assert(NULL != model_types_callbacks[f->type].set_output_bind);

            model_types_callbacks[f->type].set_output_bind(stmt->prepared, i, ptr, f, copy);
        }
#ifdef DEBUG
        if (NULL == f && 0 != strcmp(ovh_name, "accountId")) {
            debug(_("Column '%s' unmapped for output (query: %s)"), ovh_name, sqlite3_sql(stmt->prepared));
        }
#endif /* DEBUG */
    }
}

static void statement_model_iterator_current(const void *collection, void **state, void **value, void **key)
{
    bool copy;
    modelized_t *obj;
    sqlite_statement_t *stmt;
    sqlite_statement_state_t *sss;

    assert(NULL != collection);
    assert(NULL != state);
    assert(NULL != value);

    stmt = (sqlite_statement_t *) collection;
    sss = *(sqlite_statement_state_t **) state;
    if (NULL == key) {
        copy = TRUE; // override
        *value = obj = modelized_new(sss->model);
    } else {
        *value = NULL;
        copy = sss->copy;
        obj = (modelized_t *) key;
        modelized_init(sss->model, obj);
    }
    _statement_model_set_output_bind(stmt, sss->model, obj, copy);
}

/**
 * Initialize an iterator to loop on the result set of a statement
 *
 * @param it the iterator to initialize
 * @param stmt the statement to execute
 * @param model the associated model that represents the data
 * @param obj TODO
 */
void statement_model_to_iterator(Iterator *it, sqlite_statement_t *stmt, const model_t *model, bool copy)
{
    sqlite_statement_state_t *sss;

    sss = mem_new(*sss);
    sss->copy = copy;
    sss->model = model;
    sss->type = SSST_MODEL_BASED;
    iterator_init(
        it, stmt, sss,
        statement_iterator_first, NULL,
        statement_model_iterator_current,
        statement_iterator_next, NULL,
        statement_iterator_is_valid,
        free
    );
}

/**
 * Prepares an array of *count* sqlite_statement_t
 *
 * @param statements a group of statements to pre-prepare
 * @param count the number of *statements*
 * @param error the error to populate when failing
 *
 * @return FALSE and set *error* if an error occur
 */
bool statement_batched_prepare(sqlite_statement_t *statements, size_t count, bool allocated, error_t **error)
{
    size_t i;
    bool ret;

    ret = TRUE;
    for (i = 0; ret && i < count; i++) {
        ret &= SQLITE_OK == sqlite3_prepare_v2(db, statements[i].statement, -1, &statements[i].prepared, NULL);
    }
    if (!ret) {
        --i; // return on the statement which actually failed
        error_set(error, FATAL, _("%s for %s"), sqlite3_errmsg(db), statements[i]);
        while (i-- != 0) { // finalize the initialized ones
            sqlite3_finalize(statements[i].prepared);
            statements[i].prepared = NULL;
            if (allocated) {
                free((void *) statements[i].statement);
            }
        }
    }

    return ret;
}

/**
 * Frees an array of *count* sqlite_statement_t previouly preprepared
 * with statement_batched_prepare.
 *
 * @param statements a group of statements to free
 * @param count the number of *statements*
 */
void statement_batched_finalize(sqlite_statement_t *statements, size_t count, bool allocated)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (NULL != statements[i].prepared) {
            sqlite3_finalize(statements[i].prepared);
        }
        if (allocated) {
            free((void *) statements[i].statement);
        }
    }
}

/**
 * Returns the value of the sequence for the last inserted row
 *
 * @return the ROWID of the last inserted row
 */
int sqlite_last_insert_id(void)
{
    return sqlite3_last_insert_rowid(db);
}

/**
 * Returns the number of rows affected (ie inserted, modified or deleted)
 * by the last query
 *
 * @return the number of rows affected by the query
 */
int sqlite_affected_rows(void)
{
    return sqlite3_changes(db);
}

/**
 * Runs SQL migrations if needed to create new tables or update schemas
 * of existant ones.
 *
 * @param table_name the name of the table to create or update
 * @param create_stmt the complete statement (CREATE TABLE) to create it
 *   if it doesn't already exist
 * @param migrations an array of *migrations_count* migrations to run if
 *   their *version* is lower than the current ovh-cli version number.
 *   It can safely be set to NULL when *migrations_count* = 0.
 * @param migrations_count the number of migrations
 * @param error the error to populate when failing
 *
 * @return FALSE and set *error* if an error occur
 */
bool create_or_migrate(const char *table_name, const char *create_stmt, sqlite_migration_t *migrations, size_t migrations_count, error_t **error)
{
    int ret;
    size_t i;
    sqlite3_stmt *stmt;
    char buffer[2048], *errmsg;

    ret = snprintf(buffer, ARRAY_SIZE(buffer), "PRAGMA table_info(\"%s\")", table_name);
    if (ret < 0 || ((size_t) ret) >= ARRAY_SIZE(buffer)) {
        error_set(error, FATAL, _("can't create table %s: %s"), table_name, _("buffer overflow"));
        return FALSE;
    }
    if (SQLITE_OK == (ret = sqlite3_prepare_v2(db, buffer, -1, &stmt, NULL))) {
        int step;

        step = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_finalize(stmt);
        switch (step) {
            case SQLITE_DONE:
                ret = sqlite3_exec(db, create_stmt, NULL, NULL, &errmsg);
                break;
            case SQLITE_ROW:
                for (i = 0; SQLITE_OK == ret && i < migrations_count; i++) {
                    if (migrations[i].version > user_version) {
                        ret = sqlite3_exec(db, migrations[i].statement, NULL, NULL, &errmsg);
                    }
                }
                break;
            default:
                // NOP: error is handled below
                break;
        }
    }
    if (SQLITE_OK != ret) {
        error_set(error, FATAL, "%s", errmsg);
        sqlite3_free(errmsg);
    }

    return SQLITE_OK == ret;
}

/**
 * Associates (input) values to parameters (binds) to the statement before its execution.
 * It only affects values to parameters, the query is not executed.
 *
 * @param stmt the prepared statement
 * @param nulls values of the input parameters to override with NULL (the SQL "value")
 *   Set it to NULL to ignore this parameter (same as if *nulls* was filled with FALSE)
 * @param ... list of values to bind (in the same order as the statement)
 */
void statement_bind(sqlite_statement_t *stmt, const bool *nulls, ...)
{
    va_list ap;
    const char *p;
#if 0
    int no, count;

    sqlite3_reset(stmt->prepared);
    for (no = 1, count = sqlite3_bind_parameter_count(stmt->prepared); no <= count; no++) {
        sqlite3_bind_null(stmt->prepared, no);
    }
#else
    sqlite3_reset(stmt->prepared);
    sqlite3_clear_bindings(stmt->prepared);
#endif
    assert(strlen(stmt->inbinds) == ((size_t) sqlite3_bind_parameter_count(stmt->prepared)));
    va_start(ap, nulls);
    for (p = stmt->inbinds; '\0' != *p; p++) {
        bool dobind;

        dobind = NULL == nulls || !nulls[p - stmt->inbinds];
        switch (*p) {
            case 'n':
                va_arg(ap, void *);
                sqlite3_bind_null(stmt->prepared, p - stmt->inbinds + 1);
                break;
            case 'r':
            {
                double v;

                v = va_arg(ap, double);
                if (dobind) {
                    sqlite3_bind_double(stmt->prepared, p - stmt->inbinds + 1, v);
                }
                break;
            }
            case 'b':
            {
                bool v;

                v = va_arg(ap, bool);
                if (dobind) {
                    sqlite3_bind_int(stmt->prepared, p - stmt->inbinds + 1, v);
                }
                break;
            }
            case 'e':
            case 'i':
            {
                int v;

                v = va_arg(ap, int);
                if (dobind) {
                    sqlite3_bind_int(stmt->prepared, p - stmt->inbinds + 1, v);
                }
                break;
            }
            case 'd':
            case 't':
            {
                time_t v;

                v = va_arg(ap, time_t);
                if (dobind) {
                    sqlite3_bind_int64(stmt->prepared, p - stmt->inbinds + 1, v);
                }
                break;
            }
            case 's':
            {
                char *v;

                v = va_arg(ap, char *);
                if (dobind) {
                    sqlite3_bind_text(stmt->prepared, p - stmt->inbinds + 1, v, -1, SQLITE_TRANSIENT);
                }
                break;
            }
            default:
                assert(FALSE);
                break;
        }
    }
    va_end(ap);
}

/**
 * TODO
 *
 * @param stmt
 * @param nulls
 * @param ptr
 */
void statement_bind_from_model(sqlite_statement_t *stmt, modelized_t *ptr)
{
    char placeholder[512];
    const model_field_t *f;

#if 0
    int no, count;

    sqlite3_reset(stmt->prepared);
    for (no = 1, count = sqlite3_bind_parameter_count(stmt->prepared); no <= count; no++) {
        sqlite3_bind_null(stmt->prepared, no);
    }
#else
    sqlite3_reset(stmt->prepared);
    sqlite3_clear_bindings(stmt->prepared);
#endif
    placeholder[0] = ':';
    for (f = ptr->model->fields; NULL != f->ovh_name; f++) {
        int paramno;

        strlcpy(placeholder + 1, f->ovh_name, ARRAY_SIZE(placeholder) - 1);
        if (0 != (paramno = sqlite3_bind_parameter_index(stmt->prepared, placeholder))) {
            if (FIELD_NOT_NULL(ptr, f)) { // if <field_name>_not_null is TRUE
                assert(f->type >= 0 && f->type <= _MODEL_TYPE_LAST);

//                 if (FIELD_CHANGED(ptr, f)) { // if <field_name>_changed is TRUE
                    assert(NULL != model_types_callbacks[f->type].set_input_bind);

                    model_types_callbacks[f->type].set_input_bind(stmt->prepared, paramno, ptr, f);
#if 0
                    debug("[BfM] %s is bound (changed = %s)", f->ovh_name, FIELD_CHANGED(ptr, f) ? "TRUE" : "FALSE");
//                 }
            } else {
                debug("[BfM] %s is bound with null (%s_not_null is FALSE)", f->ovh_name, f->ovh_name);
#endif
            }
#if 0
        } else {
            debug("[BfM] %s not bound", f->ovh_name);
#endif
        }
    }
}

/**
 * Executes the query (on the first call after a statement_bind*) and copies
 * the value of each column of the current line of the result set to the
 * given addresses.
 *
 * @param stmt the statement to execute and read
 * @param error the error to populate when failing
 * @param ... the list of output addresses which receive the different values
 *
 * @return TRUE if a line was read ; FALSE on error (error is set with it) or
 * if there was no remaining lines to read (caller can test if NULL == *error
 * to distinguish both cases)
 */
bool statement_fetch(sqlite_statement_t *stmt, error_t **error, ...)
{
    bool ret;
    va_list ap;

    ret = FALSE;
    va_start(ap, error);
    switch (sqlite3_step(stmt->prepared)) {
        case SQLITE_ROW:
        {
            const char *p;

            assert(((size_t) sqlite3_column_count(stmt->prepared)) >= strlen(stmt->outbinds)); // allow unused result columns at the end
            for (p = stmt->outbinds; '\0' != *p; p++) {
                switch (*p) {
                    case 'b':
                    {
                        bool *v;

                        v = va_arg(ap, bool *);
                        *v = /*!!*/sqlite3_column_int(stmt->prepared, p - stmt->outbinds);
                        break;
                    }
                    case 'e':
                    case 'i':
                    {
                        int *v;

                        v = va_arg(ap, int *);
                        *v = sqlite3_column_int(stmt->prepared, p - stmt->outbinds);
                        break;
                    }
                    case 'd':
                    case 't':
                    {
                        time_t *v;

                        v = va_arg(ap, time_t *);
                        *v = sqlite3_column_int64(stmt->prepared, p - stmt->outbinds);
                        break;
                    }
                    case 's':
                    {
                        char **uv;
                        const unsigned char *sv;

                        uv = va_arg(ap, char **);
                        sv = sqlite3_column_text(stmt->prepared, p - stmt->outbinds);
                        if (NULL == sv) {
                            *uv = NULL;
                        } else {
                            *uv = strdup((char *) sv);
                        }
                        break;
                    }
                    case ' ':
                    case '-':
                        // ignore
                        break;
                    default:
                        assert(FALSE);
                        break;
                }
            }
            ret = TRUE;
            break;
        }
        case SQLITE_DONE:
            // empty result set (no error and return FALSE to caller, it should known it doesn't remain any data to read)
            break;
        default:
            error_set(error, WARN, _("%s for %s"), sqlite3_errmsg(db), sqlite3_sql(stmt->prepared));
            break;
    }
    va_end(ap);

    return ret;
}

/**
 * TODO
 *
 * @param stmt
 * @param obj
 * @param error
 *
 * @return
 */
bool statement_fetch_to_model(sqlite_statement_t *stmt, modelized_t *obj, bool copy, error_t **error)
{
    bool ret;

    ret = FALSE;
    switch (sqlite3_step(stmt->prepared)) {
        case SQLITE_ROW:
        {
            _statement_model_set_output_bind(stmt, obj->model, obj, copy);
            ret = TRUE;
            break;
        }
        case SQLITE_DONE:
            // empty result set (no error and return FALSE to caller, it should known it doesn't remain any data to read)
            break;
        default:
            error_set(error, WARN, _("%s for %s"), sqlite3_errmsg(db), sqlite3_sql(stmt->prepared));
            break;
    }

    return ret;
}

/**
 * Helper for completion
 *
 * TODO: move it to model.c after removing sqlite specifics?
 *
 * @param model the description of data
 * @param stmt the statement to execute from which to retrieve the data
 * @param possibilities a recipient which contains all possible values
 *
 * @return TRUE (can't fail)
 */
bool complete_from_modelized_statement(const model_t *model, sqlite_statement_t *stmt, completer_t *possibilities)
{
    Iterator it;

    statement_model_to_iterator(&it, stmt, model, TRUE/* unused */);
    complete_from_modelized(&it, possibilities);

    return TRUE;
}

/*static */char *model_to_sql_create_table(const model_t *model)
{
    String *buffer;
    size_t primaries_length;
    const model_field_t *f, *primaries[12];

    primaries_length = 0;
    buffer = string_new();
    STRING_APPEND_STRING(buffer, "CREATE TABLE \"");
    string_append_string(buffer, model->name);
    STRING_APPEND_STRING(buffer, "\"(\n");
    for (f = model->fields; NULL != f->ovh_name; f++) {
        if (f != model->fields) {
            STRING_APPEND_STRING(buffer, ",\n");
        }
        assert(f->type >= 0 && f->type <= _MODEL_TYPE_LAST);
        assert(NULL != model_types_callbacks[f->type].sqlite_type);

        STRING_APPEND_STRING(buffer, "\t\"");
        if (HAS_FLAG(f->flags, MODEL_FLAG_PRIMARY)) {
            primaries[primaries_length++] = f;
        }
        string_append_string(buffer, f->ovh_name);
        STRING_APPEND_STRING(buffer, "\" ");
        string_append_string_len(buffer, model_types_callbacks[f->type].sqlite_type, model_types_callbacks[f->type].sqlite_type_len);
        if (!HAS_FLAG(f->flags, MODEL_FLAG_NULLABLE)) {
            STRING_APPEND_STRING(buffer, " NOT NULL");
        }
        if (HAS_FLAG(f->flags, MODEL_FLAG_UNIQUE)) {
            STRING_APPEND_STRING(buffer, " UNIQUE");
        }
    }
    if (0 != primaries_length) {
        STRING_APPEND_STRING(buffer, ",\n\tPRIMARY(");
        while (0 != primaries_length--) {
            string_append_string(buffer, primaries[primaries_length]->ovh_name);
            if (0 != primaries_length) {
                STRING_APPEND_STRING(buffer, ", ");
            }
        }
        STRING_APPEND_STRING(buffer, ")\n");
    } else {
        string_append_char(buffer, '\n');
    }
    STRING_APPEND_STRING(buffer, ");");

    return string_orphan(buffer);
}

static char *model_to_sql_select(const model_t *model)
{
    String *buffer;

    buffer = string_new();
    STRING_APPEND_STRING(buffer, "SELECT * FROM \"");
    string_append_string(buffer, model->name);
#if 1
    string_append_char(buffer, '"');
#else
    STRING_APPEND_STRING(buffer, "\" WHERE accountId = ?");
#endif

    return string_orphan(buffer);
}

enum {
    INSERT,
    INSERT_OR_IGNORE,
    INSERT_OR_REPLACE,
    UPSERT = INSERT_OR_REPLACE,
};

static char *model_to_sql_xsert(const model_t *model, int type)
{
    String *buffer;
    const model_field_t *f;

    buffer = string_new();
    STRING_APPEND_STRING(buffer, "INSERT ");
    switch (type) {
        case INSERT:
            break;
        case INSERT_OR_IGNORE:
            STRING_APPEND_STRING(buffer, "OR IGNORE ");
            break;
        case INSERT_OR_REPLACE:
            STRING_APPEND_STRING(buffer, "OR REPLACE ");
            break;
        default:
            assert(FALSE);
            break;
    }
    STRING_APPEND_STRING(buffer, "INTO \"");
    string_append_string(buffer, model->name);
    STRING_APPEND_STRING(buffer, "\"(");
    for (f = model->fields; NULL != f->ovh_name; f++) {
        if (f != model->fields) {
            STRING_APPEND_STRING(buffer, "\", \"");
        } else {
            string_append_char(buffer, '"');
        }
        string_append_string(buffer, f->ovh_name);
    }
    STRING_APPEND_STRING(buffer, "\") VALUES(");
    for (f = model->fields; NULL != f->ovh_name; f++) {
        if (f != model->fields) {
            STRING_APPEND_STRING(buffer, ", ");
        }
        string_append_char(buffer, ':');
        string_append_string(buffer, f->ovh_name);
    }
    string_append_char(buffer, ')');

    return string_orphan(buffer);
}

static char *model_to_sql_upsert(const model_t *model)
{
    return model_to_sql_xsert(model, UPSERT);
}

static char *model_to_sql_insert(const model_t *model)
{
    return model_to_sql_xsert(model, INSERT);
}

static void sql_append_pk_where_clause(String *buffer, const model_t *model)
{
    assert(NULL != model->pk);

    string_append_char(buffer, '"');
    string_append_string(buffer, model->pk->ovh_name);
    STRING_APPEND_STRING(buffer, "\" = :");
    string_append_string(buffer, model->pk->ovh_name);
}

static char *model_to_sql_update(const model_t *model)
{
    String *buffer;
    const model_field_t *f;

    buffer = string_new();
    STRING_APPEND_STRING(buffer, "UPDATE \"");
    string_append_string(buffer, model->name);
    STRING_APPEND_STRING(buffer, "\" SET ");
    for (f = model->fields; NULL != f->ovh_name; f++) {
        if (f != model->fields) {
            STRING_APPEND_STRING(buffer, ", ");
        }
        string_append_char(buffer, '"');
        string_append_string(buffer, f->ovh_name);
        STRING_APPEND_STRING(buffer, "\" = IFNULL(:");
        string_append_string(buffer, f->ovh_name);
        STRING_APPEND_STRING(buffer, ", \"");
        string_append_string(buffer, f->ovh_name);
        STRING_APPEND_STRING(buffer, "\")");
    }
    STRING_APPEND_STRING(buffer, " WHERE ");
    if (NULL == model->pk) {
        for (f = model->fields; NULL != f->ovh_name; f++) {
            if (f != model->fields) {
                STRING_APPEND_STRING(buffer, " AND ");
            }
            string_append_char(buffer, '"');
            string_append_string(buffer, f->ovh_name);
            STRING_APPEND_STRING(buffer, "\" = :");
            string_append_string(buffer, f->ovh_name);
        }
    } else {
        sql_append_pk_where_clause(buffer, model);
    }

    return string_orphan(buffer);
}

static char *model_to_sql_delete(const model_t *model)
{
    String *buffer;
    const model_field_t *f;

    buffer = string_new();
    STRING_APPEND_STRING(buffer, "DELETE FROM \"");
    string_append_string(buffer, model->name);
    STRING_APPEND_STRING(buffer, "\" WHERE ");
    if (NULL == model->pk) {
        for (f = model->fields; NULL != f->ovh_name; f++) {
            if (f != model->fields) {
                STRING_APPEND_STRING(buffer, " AND ");
            }
            string_append_char(buffer, '"');
            string_append_string(buffer, f->ovh_name);
            STRING_APPEND_STRING(buffer, "\" = :");
            string_append_string(buffer, f->ovh_name);
        }
    } else {
        sql_append_pk_where_clause(buffer, model);
    }

    return string_orphan(buffer);
}

enum {
    STMT_BACKEND_SELECT,
    STMT_BACKEND_INSERT,
    STMT_BACKEND_UPSERT,
    STMT_BACKEND_UPDATE,
    STMT_BACKEND_DELETE,
    STMT_BACKEND_COUNT
};

static char *(*to_sql[STMT_BACKEND_COUNT])(const model_t *) = {
    [ STMT_BACKEND_SELECT ] = model_to_sql_select,
    [ STMT_BACKEND_INSERT ] = model_to_sql_insert,
    [ STMT_BACKEND_UPSERT ] = model_to_sql_upsert,
    [ STMT_BACKEND_UPDATE ] = model_to_sql_update,
    [ STMT_BACKEND_DELETE ] = model_to_sql_delete,
};

static void *sqlite_backend_init(const model_t *model, error_t **error)
{
    size_t i;
    sqlite_statement_t *stmts;

    stmts = mem_new_n(*stmts, STMT_BACKEND_COUNT);
    for (i = 0; i < STMT_BACKEND_COUNT; i++) {
        stmts[i].statement = to_sql[i](model);
        stmts[i].inbinds = stmts[i].outbinds = "";
    }
    if (!statement_batched_prepare(stmts, STMT_BACKEND_COUNT, TRUE, error)) {
        free(stmts);
        stmts = NULL;
    }

    return stmts;
}

static void sqlite_backend_free(void *data)
{
    sqlite_statement_t *stmts;

    assert(NULL != data);
    stmts = (sqlite_statement_t *) data;
    statement_batched_finalize(stmts, STMT_BACKEND_COUNT, TRUE);
    free(stmts);
}

static bool sqlite_backend_save(modelized_t *obj, void *data, error_t **error)
{
    int stmt;
    bool setAI, success;
    sqlite_statement_t *stmts;

    assert(NULL != obj);
    assert(NULL != obj->model);
    assert(NULL != data);

    setAI = FALSE;
    stmts = (sqlite_statement_t *) data;
    if (NULL != obj->model->pk && MODEL_TYPE_INT == obj->model->pk->type) {
//         if (!FIELD_NOT_NULL(obj, obj->model->pk)) {
//         if (0 == VOIDP_TO_X(obj, obj->model->pk->offset, int)) {
        if (!obj->persisted) {
            // insert + set id with auto-increment value
//             setAI = TRUE;
            setAI = !FIELD_NOT_NULL(obj, obj->model->pk);
            stmt = STMT_BACKEND_UPSERT;
        } else {
            // update (ou pas si on utilise l'ID OVH)
            stmt = STMT_BACKEND_UPDATE;
        }
    } else {
        // upsert
        stmt = STMT_BACKEND_UPSERT;
    }
    statement_bind_from_model(&stmts[stmt], obj);
    success = SQLITE_DONE == sqlite3_step(stmts[stmt].prepared);
// debug("QUERY %s", stmts[stmt].statement);
    if (success) {
        obj->persisted = TRUE;
    } else {
        error_set(error, WARN, _("%s for %s"), sqlite3_errmsg(db), sqlite3_sql(stmts[stmt].prepared));
    }
    if (setAI) {
        VOIDP_TO_X(obj, obj->model->pk->offset, int) = sqlite_last_insert_id();
    }

    return success;
}

static bool sqlite_backend_delete(modelized_t *obj, void *data, error_t **error)
{
    bool success;
    sqlite_statement_t *stmts;

    assert(NULL != obj);
    assert(NULL != obj->model);
    assert(NULL != data);

    stmts = (sqlite_statement_t *) data;
    statement_bind_from_model(&stmts[STMT_BACKEND_DELETE], obj);
    success = SQLITE_DONE == sqlite3_step(stmts[STMT_BACKEND_DELETE].prepared);
    if (success) {
        obj->persisted = FALSE;
    } else {
        error_set(error, WARN, _("%s for %s"), sqlite3_errmsg(db), sqlite3_sql(stmts[STMT_BACKEND_DELETE].prepared));
    }

    return success;
}

static bool sqlite_backend_all(Iterator *it, const model_t *model, void *data, error_t **UNUSED(error))
{
    sqlite_statement_t *stmts;

    assert(NULL != it);
    assert(NULL != model);
    assert(NULL != data);

    stmts = (sqlite_statement_t *) data;
    statement_bind(&stmts[STMT_BACKEND_SELECT], NULL, current_account->id);
    statement_model_to_iterator(it, &stmts[STMT_BACKEND_SELECT], model, TRUE); // TODO: leaks

    return TRUE;
}

model_backend_t sqlite_backend = {
    sqlite_backend_init,
    sqlite_backend_free,
    sqlite_backend_all,
    sqlite_backend_save,
    sqlite_backend_delete,
};

static void sqlite_startswith(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    size_t string_len, prefix_len;
    const unsigned char *string, *prefix;

    assert(2 == argc);
    string = sqlite3_value_text(argv[0]);
    prefix = sqlite3_value_text(argv[1]);
    string_len = strlen((const char *) string);
    prefix_len = strlen((const char *) prefix);
    if (prefix_len > string_len) {
        sqlite3_result_int(context, 0);
    } else {
        sqlite3_result_int(context, 0 == strncmp((const char *) string, (const char *) prefix, prefix_len));
    }
}

#ifdef SQLITE_DEBUG
static void sqlite_trace_callback(void *UNUSED(data), const char *stmt)
{
    debug("[TRACE] %s", stmt);
}
#endif /* SQLITE_DEBUG */

static bool sqlite_early_ctor(error_t **error)
{
    int ret;
    mode_t old_umask;

    *db_path = '\0';
    if (build_path_from_home(OVH_DB_FILENAME, db_path, ARRAY_SIZE(db_path))) {
        error_set(error, FATAL, _("buffer overflow"));
        return FALSE;
    }
    if ('\0' == *db_path) {
        error_set(error, FATAL, _("path to database is empty"));
        return FALSE;
    }
    // open database
    old_umask = umask(077);
    if (SQLITE_OK != (ret = sqlite3_open(db_path, &db))) {
        error_set(error, FATAL, _("can't open sqlite database %s: %s"), db_path, sqlite3_errmsg(db));
        return FALSE;
    }
    umask(old_umask);
    // preprepare own statement
    if (!statement_batched_prepare(statements, STMT_COUNT, FALSE, error)) {
        return FALSE;
    }
    // fetch user_version
    if (SQLITE_ROW != sqlite3_step(statements[STMT_GET_USER_VERSION].prepared)) {
        error_set(error, FATAL, _("can't retrieve database version: %s"), sqlite3_errmsg(db));
        return FALSE;
    }
    user_version = sqlite3_column_int(statements[STMT_GET_USER_VERSION].prepared, 0);
    sqlite3_reset(statements[STMT_GET_USER_VERSION].prepared);

    sqlite3_create_function_v2(db, "startswith", 2, SQLITE_UTF8/* | SQLITE_DETERMINISTIC*/, NULL, sqlite_startswith, NULL, NULL, NULL);
#ifdef SQLITE_DEBUG
    sqlite3_trace(db, sqlite_trace_callback, NULL);
#endif /* SQLITE_DEBUG */

    return TRUE;
}

static bool sqlite_late_ctor(error_t **error)
{
    if (OVH_CLI_VERSION_NUMBER > user_version) {
        statement_bind(&statements[STMT_SET_USER_VERSION], NULL);
        statement_fetch(&statements[STMT_SET_USER_VERSION], error);
    }

    return NULL == *error;
}

static void sqlite_dtor(void)
{
    statement_batched_finalize(statements, STMT_COUNT, FALSE);
    sqlite3_close(db);
}

DECLARE_MODULE(sqlite) = {
    "sqlite",
    NULL,
    NULL,
    sqlite_early_ctor,
    sqlite_late_ctor,
    sqlite_dtor
};
