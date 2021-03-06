#include <stdio.h>
#include <string.h>

#include "common.h"
#include "command.h"
#include "date.h"
#include "endpoints.h"
#include "modules/api.h"
#include "modules/table.h"
#include "modules/sqlite.h"
#include "commands/account.h"
#include "struct/hashtable.h"
#include "account_api.h"

enum {
    DTOR_NOCALL,
    DTOR_CALL
};

#define NO_ACTIVE_ACCOUNT \
    0 == acd.current_account.id

typedef struct {
    account_t current_account;
    application_t current_application;
    HashTable *modules_data;
    HashTable *modules_callbacks;
} account_command_data_t;

typedef struct {
//     account_t account;
//     {
    int endpoint;
    char *account;
    char *password;
    char *expiration;
    char *consumer_key;
//     }
    int expires_in_at;
    bool endpoint_present;
} account_argument_t;

#define NULL_OR_EMPTY(s) \
    (NULL == (s) || '\0' == *(s))

#define UNEXISTANT_ACCOUNT \
    do { \
        error_set(error, WARN, _("no account named '%s'"), args->account); \
    } while (0);

typedef struct {
    DtorFunc dtor;
    void (*on_set_account)(void **);
} module_callbacks_t;

static account_command_data_t acd;

const account_t *current_account;
const application_t *current_application;

enum {
    // account
    STMT_ACCOUNT_LIST,
//     STMT_ACCOUNT_UPDATE,
//     STMT_ACCOUNT_INSERT,
    STMT_ACCOUNT_DELETE,
    // specialized read (select)
    STMT_ACCOUNT_LOAD,
    STMT_ACCOUNT_COMPLETION,
    STMT_ACCOUNT_LOAD_DEFAULT,
    // non-generalized updates
    STMT_ACCOUNT_UPDATE_KEY,
    STMT_ACCOUNT_UPDATE_DEFAULT,
    // application
    STMT_APPLICATION_LIST,
    STMT_APPLICATION_INSERT,
    STMT_APPLICATION_DELETE,
    // specific read (select)
    STMT_APPLICATION_LOAD,
    // module
    STMT_FETCH_SELECT,
    STMT_FETCH_UPSERT,
    // count
    STMT_COUNT
};

#define ACCOUNT_OUTPUT_BINDS "isssiii"
#define APPLICATION_OUTPUT_BINDS "ssi"

static sqlite_statement_t statements[STMT_COUNT] = {
    [ STMT_ACCOUNT_LIST ]           = DECL_STMT("SELECT * FROM accounts", "", ACCOUNT_OUTPUT_BINDS),
//     [ STMT_ACCOUNT_UPDATE ]         = DECL_STMT("UPDATE accounts SET password = IFNULL(?, password), consumer_key = IFNULL(?, consumer_key), expires_at = IFNULL(?, expires_at), endpoint_id = IFNULL(?, endpoint_id) WHERE name = ?", "sstis", ""),
//     [ STMT_ACCOUNT_INSERT ]         = DECL_STMT("INSERT INTO accounts(name, password, consumer_key, endpoint_id, is_default, expires_at) VALUES(:name, :password, :consumer_key, :endpoint_id, :is_default, :expires_at)", "sssiit", ""),
    [ STMT_ACCOUNT_DELETE ]         = DECL_STMT("DELETE FROM accounts WHERE name = ?", "s", ""),
    [ STMT_ACCOUNT_LOAD ]           = DECL_STMT("SELECT * FROM accounts WHERE name = ?", "s", ACCOUNT_OUTPUT_BINDS),
    [ STMT_ACCOUNT_COMPLETION ]     = DECL_STMT("SELECT name FROM accounts WHERE name LIKE ? || '%'", "s", "s"),
    [ STMT_ACCOUNT_LOAD_DEFAULT ]   = DECL_STMT("SELECT * FROM accounts ORDER BY is_default DESC LIMIT 1", "", ACCOUNT_OUTPUT_BINDS),
    [ STMT_ACCOUNT_UPDATE_DEFAULT ] = DECL_STMT("UPDATE accounts SET is_default = (name = ?)", "s", ""),
    [ STMT_ACCOUNT_UPDATE_KEY ]     = DECL_STMT("UPDATE accounts SET consumer_key = :consumer_key, expires_at = :expires_at WHERE name = :name", "sts", ""),
    [ STMT_APPLICATION_LIST ]       = DECL_STMT("SELECT * FROM applications", "", APPLICATION_OUTPUT_BINDS),
    [ STMT_APPLICATION_INSERT ]     = DECL_STMT("INSERT INTO applications(\"key\", secret, endpoint_id) VALUES(:key, :secret, :endpoint_id)", "ssi", ""),
    [ STMT_APPLICATION_DELETE ]     = DECL_STMT("DELETE FROM applications WHERE endpoint_id = ?", "i", ""),
    [ STMT_APPLICATION_LOAD ]       = DECL_STMT("SELECT * FROM applications WHERE endpoint_id = ?", "i", APPLICATION_OUTPUT_BINDS),
    [ STMT_FETCH_SELECT ]           = DECL_STMT("SELECT updated_at FROM fetches WHERE account_id = ? AND module_name = ?", "is", "i"),
    [ STMT_FETCH_UPSERT ]           = DECL_STMT("INSERT OR REPLACE INTO fetches(account_id, module_name, updated_at) VALUES(?, ?, strftime('%s','now'))", "is", ""),
};

model_t *account_model, *application_model;

#undef DECL_FIELD_STRUCT_NAME
#define DECL_FIELD_STRUCT_NAME account_t
static model_field_t account_fields[] = {
    DECL_FIELD_INT(N_("id"), id, MODEL_FLAG_PRIMARY | MODEL_FLAG_INTERNAL),
    DECL_FIELD_BOOL(N_("default"), is_default, 0),
    DECL_FIELD_STRING(N_("account"), name, MODEL_FLAG_UNIQUE),
    DECL_FIELD_STRING(N_("password"), password, MODEL_FLAG_NULLABLE),
    DECL_FIELD_DATETIME(N_("key expiration"), expires_at, MODEL_FLAG_NULLABLE),
    DECL_FIELD_STRING(N_("consumer key"), consumer_key, MODEL_FLAG_NULLABLE),
    DECL_FIELD_ENUM(N_("endpoint"), endpoint_id, 0, endpoint_names),
    MODEL_FIELD_SENTINEL
};

#undef DECL_FIELD_STRUCT_NAME
#define DECL_FIELD_STRUCT_NAME application_t
static model_field_t application_fields[] = {
    DECL_FIELD_STRING(N_("key"), key, 0),
    DECL_FIELD_STRING(N_("secret"), secret, 0),
    DECL_FIELD_ENUM(N_("endpoint"), endpoint_id, MODEL_FLAG_PRIMARY, endpoint_names),
    MODEL_FIELD_SENTINEL
};

enum {
    EXPIRES_IN,
    EXPIRES_AT
};

static const char * const expires_in_at[] = {
    [ EXPIRES_IN ] = "in",
    [ EXPIRES_AT ] = "at",
    NULL
};

static void account_flush(void)
{
    acd.current_account.id = 0;
    free(acd.current_account.name);
    acd.current_account.name = NULL;
    free(acd.current_account.password);
    acd.current_account.password = NULL;
    free((void *) acd.current_account.consumer_key);
    acd.current_account.consumer_key = NULL;
//     bzero(&acd.current_account, sizeof(acd.current_account));

    free(acd.current_application.key);
    acd.current_application.key = NULL;
    free(acd.current_application.secret);
    acd.current_application.secret = NULL;
//     bzero(&acd.current_application, sizeof(acd.current_application));
}

static bool account_set_current(const char *name, error_t **error)
{
    if (NO_ACTIVE_ACCOUNT || NULL == name || 0 != strcmp(name, acd.current_account.name)) {
        bool exists;
        Iterator it;
        int stmt;

        // change variables
        account_flush();
        if (NULL == name) {
            stmt = STMT_ACCOUNT_LOAD_DEFAULT;
            statement_bind(&statements[stmt], NULL);
        } else {
            stmt = STMT_ACCOUNT_LOAD;
            statement_bind(&statements[stmt], NULL, name);
        }
//         statement_bind_from_model(&statements[stmt], account_model, (modelized_t *) &acd.current_account);
        if (statement_fetch_to_model(&statements[stmt], (modelized_t *) &acd.current_account, TRUE, error)) {
//             statement_bind_from_model(&statements[STMT_APPLICATION_LOAD], application_model, (modelized_t *) &acd.current_application);
            statement_bind(&statements[STMT_APPLICATION_LOAD], NULL, acd.current_account.endpoint_id);
            if (!statement_fetch_to_model(&statements[STMT_APPLICATION_LOAD], (modelized_t *) &acd.current_application, TRUE, error)) {
                return FALSE;
            }
        } else {
            return FALSE;
        }
        // notify modules
        hashtable_to_iterator(&it, acd.modules_callbacks);
        for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
//             ht_hash_t h;
            HashTable *ht;
            void *key, *data;
            module_callbacks_t *mc;

            mc = iterator_current(&it, &key);
            assert(NULL != mc);
            if (NULL != mc->on_set_account) {
                data = NULL;
                exists = hashtable_direct_get(acd.modules_data, acd.current_account.id, &ht);
                if (!exists) {
                    ht = hashtable_ascii_cs_new(NULL, NULL, NULL);
                    hashtable_direct_put(acd.modules_data, 0, acd.current_account.id, ht, NULL);
//                     h = hashtable_hash(ht, key);
                } else {
//                     h = hashtable_hash(ht, key);
                    exists = hashtable_get(ht, key, &data);
                }
                mc->on_set_account(&data);
                if (!exists) {
                    hashtable_put(ht, 0, key, data, NULL);
//                     hashtable_quick_put(ht, 0, h, key, data, NULL);
                }
            }
        }
        iterator_close(&it);
    }

    return TRUE;
}

void account_invalidate_consumer_key(error_t **UNUSED(error))
{
    /**
     * TODO:
     * - do not invalidate fresh generated but not yet validated consumer key
     * - emit (early) an error if we try to use a such consumer key?
     */
    free((void *) acd.current_account.consumer_key);
    acd.current_account.consumer_key = NULL;
//     acd.current_account.expires_at = 0;
    /**
     * error is unused for now, it depends if we make the choice to UPDATE
     * the database now to be sure that the consumer key is not used anymore
     * or if we wait the generation of a new one
     *
     * for now, just invalidate in memory and wait the generation of a new
     * one before updating the account in database
     */
}

bool check_current_application_and_account(bool skip_CK_check, error_t **error)
{
    if (NO_ACTIVE_ACCOUNT) {
        error_set(error, WARN, _("no current account"));
        return FALSE;
    }
    if (NULL_OR_EMPTY(acd.current_application.key)) {
        error_set(error, WARN, _("no application registered for endpoint '%s'"), endpoint_names[acd.current_account.endpoint_id]);
        return FALSE;
    }
    if (skip_CK_check) {
        return TRUE;
    }
    // if no CK is defined or it is expired, get one
    if (NULL_OR_EMPTY(acd.current_account.consumer_key) || (0 != acd.current_account.expires_at && acd.current_account.expires_at < time(NULL))) {
        // if we successfully had a CK, save it
        if (NULL != (acd.current_account.consumer_key = request_consumer_key(&acd.current_account.expires_at, error))) {
            statement_bind_from_model(&statements[STMT_ACCOUNT_UPDATE_KEY], (modelized_t *) &acd.current_account);
            statement_fetch(&statements[STMT_ACCOUNT_UPDATE_KEY], error);
        }
    }

    return NULL != acd.current_account.consumer_key;
}

const char *account_current(void)
{
    if (NO_ACTIVE_ACCOUNT) {
        return "(no current account)";
    } else {
        return acd.current_account.name;
    }
}

void account_current_set_data(const char *name, void *data)
{
    HashTable *ht;

    if (!hashtable_direct_get(acd.modules_data, acd.current_account.id, &ht)) {
        assert(FALSE);
    }
    hashtable_put(ht, 0, name, data, NULL);
}

bool account_current_get_data(const char *name, void **data)
{
    HashTable *ht;

    if (!hashtable_direct_get(acd.modules_data, acd.current_account.id, &ht)) {
        assert(FALSE);
    }

    return hashtable_get(ht, name, data);
}

void account_register_module_callbacks(const char *name, DtorFunc dtor, void (*on_set_account)(void **))
{
    module_callbacks_t *mc;

    assert(NULL != name);

    if (NULL != dtor && NULL != on_set_account) { // nothing to do? Skip it!
        bool exists;
        ht_hash_t h;

        h = hashtable_hash(acd.modules_callbacks, name);
        if (!(exists = hashtable_quick_get(acd.modules_callbacks, h, name, &mc))) {
            mc = mem_new(*mc);
        }
        mc->dtor = dtor;
        mc->on_set_account = on_set_account;
        if (!exists) {
            hashtable_quick_put(acd.modules_callbacks, 0, h, name, mc, NULL);
        }
    }
}

// a hashtable (const char *module name => void *) is associated to each account
static void account_data_dtor(void *data)
{
    HashTable *ht;

    assert(NULL != data);

    ht = (HashTable *) data;
    if (NULL != ht) {
        if (NULL != acd.modules_callbacks) {
            Iterator it;

            hashtable_to_iterator(&it, ht);
            for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
                void *key, *value;
                module_callbacks_t *mc;

                value = iterator_current(&it, &key);
                if (hashtable_get(acd.modules_callbacks, key, &mc)) {
                    mc->dtor(value);
                }
            }
            iterator_close(&it);
        }
        hashtable_destroy(ht);
    }
}

bool account_get_last_fetch_for(const char *module_name, time_t *t, error_t **error)
{
    statement_bind(&statements[STMT_FETCH_SELECT], NULL, acd.current_account.id, module_name);
    return statement_fetch(&statements[STMT_FETCH_SELECT], error, &t);
}

bool account_set_last_fetch_for(const char *module_name, error_t **error)
{
    statement_bind(&statements[STMT_FETCH_UPSERT], NULL, acd.current_account.id, module_name);
    return statement_fetch(&statements[STMT_FETCH_UPSERT], error);
}

static bool account_early_ctor(error_t **error)
{
    account_flush();

    if (!create_or_migrate("accounts", "CREATE TABLE accounts(\n\
        id INTEGER NOT NULL PRIMARY KEY,\n\
        name TEXT NOT NULL UNIQUE,\n\
        password TEXT,\n\
        consumer_key TEXT,\n\
        endpoint_id INTEGER NOT NULL,\n\
        is_default INTEGER NOT NULL DEFAULT 0,\n\
        expires_at INTEGER\n\
    )", NULL, 0, error)) {
        return FALSE;
    }
    if (!create_or_migrate("applications", "CREATE TABLE applications(\n\
        key TEXT NOT NULL,\n\
        secret TEXT NOT NULL,\n\
        endpoint_id INTEGER NOT NULL UNIQUE\n\
    )", NULL, 0, error)) {
        return FALSE;
    }
    if (!create_or_migrate("fetches", "CREATE TABLE fetches(\n\
        account_id INT NOT NULL REFERENCES accounts(id) ON UPDATE CASCADE ON DELETE CASCADE,\n\
        module_name TEXT NOT NULL,\n\
        updated_at INT NOT NULL,\n\
        PRIMARY KEY (account_id, module_name)\n\
    )", NULL, 0, error)) {
        return FALSE;
    }

    if (!statement_batched_prepare(statements, STMT_COUNT, FALSE, error)) {
        return FALSE;
    }

    account_model = model_new("accounts", sizeof(account_t), account_fields, ARRAY_SIZE(account_fields) - 1, "name", &sqlite_backend, error);
    application_model = model_new("applications", sizeof(application_t), application_fields, ARRAY_SIZE(application_fields) - 1, "endpoint_id", &sqlite_backend, error);

    modelized_init(account_model, (modelized_t *) &acd.current_account);
    modelized_init(application_model, (modelized_t *) &acd.current_application);
    current_account = &acd.current_account;
    current_application = &acd.current_application;
    acd.modules_data = hashtable_new(value_hash, value_equal, NULL, NULL, account_data_dtor);
    acd.modules_callbacks = hashtable_ascii_cs_new(NULL, NULL /* no dtor for key as it is also part of the value */, free);

//     debug("endpoint_id = %zu", offsetof(account_t, endpoint_id));
//     debug("endpoint_id_changed = %zu/%zu", offsetof(account_t, endpoint_id_changed), offsetof(account_t, endpoint_id) + model_type_size_map[MODEL_TYPE_ENUM]);
//     debug("endpoint_id_not_null = %zu/%zu", offsetof(account_t, endpoint_id_not_null), offsetof(account_t, endpoint_id) + model_type_size_map[MODEL_TYPE_ENUM] + sizeof(bool));

    return TRUE;
}

static bool account_late_ctor(error_t **error)
{
    return account_set_current(NULL, error);
}

static void account_dtor(void)
{
    if (NULL != acd.modules_data) {
        hashtable_destroy(acd.modules_data);
    }
    if (NULL != acd.modules_callbacks) {
        hashtable_destroy(acd.modules_callbacks);
    }
    account_flush();
    model_destroy(account_model);
    model_destroy(application_model);
    statement_batched_finalize(statements, STMT_COUNT, FALSE);
}

static command_status_t account_list(COMMAND_ARGS)
{
    USED(arg);
    USED(error);
    USED(mainopts);

    return model_to_table(account_model, error);
}

static command_status_t account_add_or_update(COMMAND_ARGS, bool update)
{
    time_t expires_at;
    account_t account;
    account_argument_t *args;

    USED(mainopts);
    expires_at = (time_t) 0;
    args = (account_argument_t *) arg;
    assert(NULL != args->account);
    if (!update) {
        assert(NULL != args->password); // password argument is mandatory with actual account add command (may be not the case in the future if account add and update share the same logic)
    }

    if (NULL != args->expiration) {
        switch (args->expires_in_at) {
            case EXPIRES_IN:
                if (!parse_duration(args->expiration, &expires_at)) {
                    error_set(error, WARN, _("command aborted: unable to parse duration '%s'"), args->expiration);
                    return COMMAND_USAGE;
                }
                expires_at += time(NULL);
                break;
            case EXPIRES_AT:
                if (!date_parse_to_timestamp(args->expiration, "%c", &expires_at)) {
                    error_set(error, WARN, _("command aborted: unable to parse expiration date '%s'"), args->expiration);
                    return COMMAND_USAGE;
                }
                break;
            default:
                assert(FALSE);
        }
    }
    if (!update && !args->endpoint_present) {
        error_set(error, WARN, _("no endpoint specified"));
        return COMMAND_USAGE;
    }
    modelized_init(account_model, (modelized_t *) &account);
//     account.password = NULL;
//     account.endpoint_id = 0;
//     account.consumer_key = NULL;
//     account.name = args->account;
    MODELIZED_SET(&account, name, args->account);
    if (args->endpoint_present) {
//         account.endpoint_id = args->endpoint;
        MODELIZED_SET(&account, endpoint_id, args->endpoint);
    }
    if (!update || NULL != args->password) { // for update, only if a new password is set
//         account.password = args->password;
        MODELIZED_SET(&account, password, args->password);
    }
    if (NULL != args->consumer_key) { // only if a new CK is set
//         account.consumer_key = args->consumer_key;
//         account.expires_at = expires_at;
        MODELIZED_SET(&account, consumer_key, args->consumer_key);
        MODELIZED_SET(&account, expires_at, expires_at);
    }
    if (!update) {
        HashTable *ht;

#if 0
        statement_bind_from_model(&statements[STMT_ACCOUNT_INSERT], (modelized_t *) &account);
        statement_fetch(&statements[STMT_ACCOUNT_INSERT], error);
        account.id = sqlite_last_insert_id();
#else
        MODELIZED_SET(&account, is_default, FALSE);
        modelized_save((modelized_t *) &account, error);
#endif
        // if this is the first account, set it as current
        if (NO_ACTIVE_ACCOUNT) {
            acd.current_account = account;
            if (NULL != args->consumer_key) {
                acd.current_account.consumer_key = strdup(args->consumer_key);
            }
            if (NULL != args->password) {
                acd.current_account.password = strdup(args->password);
            }
            if (NULL != args->account) {
                acd.current_account.name = strdup(args->account);
            }
        }

        ht = hashtable_ascii_cs_new(NULL, NULL, NULL);
        hashtable_direct_put(acd.modules_data, 0, account.id, ht, NULL);
    } else {
#if 0
        bool nulls[5] = { FALSE };

        CHECK_NULLS_LENGTH(nulls, statements[STMT_ACCOUNT_UPDATE]);
        nulls[0] = NULL == args->password;
        nulls[1] = NULL == args->consumer_key;
        nulls[2] = NULL == args->consumer_key;
        nulls[3] = !args->endpoint_present;
        statement_bind_from_model(&statements[STMT_ACCOUNT_UPDATE], (modelized_t *) &account);
        statement_fetch(&statements[STMT_ACCOUNT_UPDATE], error);
#else
//         if (NULL == args->password) {
//             MODELIZED_NULL(account, );
//         }
        modelized_save((modelized_t *) &account, error);
#endif
    }

    return NULL == *error ? COMMAND_SUCCESS : COMMAND_FAILURE;
}

/**
 * account [nic-handle] add (password [password]) (key [consumer key] expires in|at [date]) (endpoint [endpoint])
 *
 * NOTE: in order to not record password, use an empty string (with "")
 **/
static command_status_t account_add(COMMAND_ARGS)
{
    return account_add_or_update(RELAY_COMMAND_ARGS, FALSE);
}

static command_status_t account_update(COMMAND_ARGS)
{
    return account_add_or_update(RELAY_COMMAND_ARGS, TRUE);
}

static command_status_t account_default_set(COMMAND_ARGS)
{
    bool success;
    account_argument_t *args;

    USED(error);
    USED(mainopts);
    args = (account_argument_t *) arg;
    assert(NULL != args->account);

    statement_bind(&statements[STMT_ACCOUNT_UPDATE_DEFAULT], NULL, args->account);
    statement_fetch(&statements[STMT_ACCOUNT_UPDATE_DEFAULT], error);
    success = 0 != sqlite_affected_rows();
    if (!success) {
        UNEXISTANT_ACCOUNT;
    }

    return success ? COMMAND_SUCCESS : COMMAND_FAILURE;
}

static command_status_t account_delete(COMMAND_ARGS)
{
    bool success;
    account_argument_t *args;

    USED(mainopts);
    args = (account_argument_t *) arg;
    assert(NULL != args->account);
    if (!NO_ACTIVE_ACCOUNT && 0 == strcmp(args->account, acd.current_account.name)) {
        account_flush();
    }
    statement_bind(&statements[STMT_ACCOUNT_DELETE], NULL, args->account);
    statement_fetch(&statements[STMT_ACCOUNT_DELETE], error);
    success = 0 != sqlite_affected_rows();
    if (!success) {
        UNEXISTANT_ACCOUNT;
    }
//     hashtable_direct_delete(acd.modules_data, <id of deleted accout>, DTOR_CALL);

    return success ? COMMAND_SUCCESS : COMMAND_FAILURE;
}

static command_status_t account_switch(COMMAND_ARGS)
{
    bool success;
    account_argument_t *args;

    USED(mainopts);
    args = (account_argument_t *) arg;
    assert(NULL != args->account);
    success = account_set_current(args->account, error);
    if (!success) {
        UNEXISTANT_ACCOUNT;
    }

    return success ? COMMAND_SUCCESS : COMMAND_FAILURE;
}

static command_status_t account_invalidate(COMMAND_ARGS)
{
    account_argument_t *args;

    USED(mainopts);
    args = (account_argument_t *) arg;
    assert(NULL != args->account);
    statement_bind(&statements[STMT_ACCOUNT_UPDATE_KEY], NULL, "", 0, args->account);
    statement_fetch(&statements[STMT_ACCOUNT_UPDATE_KEY], error);

    return COMMAND_SUCCESS;
}

static command_status_t application_list(COMMAND_ARGS)
{
    USED(arg);
    USED(error);
    USED(mainopts);

    return model_to_table(application_model, error);
}

static command_status_t application_add(COMMAND_ARGS)
{
    application_t *application;

    USED(error);
    USED(mainopts);
    application = (application_t *) arg;
    assert(NULL != application->key);
    assert(NULL != application->secret);
    // graph_command_dispatch isn't (yet) aware of model
    application->data.model = application_model;
    application->endpoint_id_not_null = application->key_not_null = application->secret_not_null = TRUE;
#if 0
    statement_bind_from_model(&statements[STMT_APPLICATION_INSERT], (modelized_t *) application);
    statement_fetch(&statements[STMT_APPLICATION_INSERT], error);
    // TODO: if !update (0 == sqlite_affected_rows), duplicate?
#else
    modelized_save((modelized_t *) application, error);
#endif

    return CMD_FLAG_SKIP_HISTORY | (NULL == *error ? COMMAND_SUCCESS : COMMAND_FAILURE);
}

static command_status_t application_delete(COMMAND_ARGS)
{
    bool success;
    application_t *application;

    USED(error);
    USED(mainopts);
    application = (application_t *) arg;
#if 0
    statement_bind(&statements[STMT_APPLICATION_DELETE], NULL, application->endpoint_id);
    statement_fetch(&statements[STMT_APPLICATION_DELETE], error);
#else
    application->data.model = application_model; // graph_command_dispatch doesn't set model
    modelized_delete((modelized_t *) application, error);
#endif
    success = 0 != sqlite_affected_rows();
    if (!success) {
        error_set(error, NOTICE, _("no application associated to endpoint %s"), endpoint_names[application->endpoint_id]);
    }

    return success ? COMMAND_SUCCESS : COMMAND_FAILURE;
}

static command_status_t export(COMMAND_ARGS)
{
    Iterator it;
    String *buffer;
    account_t account;
    application_t application;

    USED(arg);
    USED(error);
    USED(mainopts);
    buffer = string_new();
    // accounts
    statement_model_to_iterator(&it, &statements[STMT_ACCOUNT_LIST], account_model, FALSE);
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        iterator_current(&it, (void **) &account);
        string_append_formatted(buffer, "account %s add password \"%s\" endpoint %s", account.name, account.password, endpoint_names[account.endpoint_id]);
        if (!NULL_OR_EMPTY(account.consumer_key)) {
            char datebuf[512];

            if (0 != timestamp_to_localtime(account.expires_at, "%c", datebuf, ARRAY_SIZE(datebuf))) {
                string_append_formatted(buffer, " key \"%s\" expires at \"%s\"", account.consumer_key, datebuf);
            }
        }
        string_append_char(buffer, '\n');
        if (account.is_default) {
            string_append_formatted(buffer, "account %s default\n", account.name);
        }
    }
    iterator_close(&it);
    // applications
    statement_model_to_iterator(&it, &statements[STMT_APPLICATION_LIST], application_model, FALSE);
    for (iterator_first(&it); iterator_is_valid(&it); iterator_next(&it)) {
        iterator_current(&it, (void **) &application);
        string_append_formatted(buffer, "application %s add \"%s\" \"%s\"\n", endpoint_names[application.endpoint_id], application.key, application.secret);
    }
    iterator_close(&it);
    puts(buffer->ptr);
    string_destroy(buffer);

    return COMMAND_SUCCESS;
}

static void account_regcomm(graph_t *g)
{
    graph_create_full_path(g, argument_create_literal("export", export, _("export OVH accounts and applications in ovh-cli commands format")), NULL);
    // account ...
    {
        argument_t *arg_expires_in_at;
        argument_t *arg_account, *arg_password, *arg_consumer_key, *arg_expiration, *arg_endpoint;
        argument_t *lit_account, *lit_list, *lit_delete, *lit_add, *lit_update, *lit_invalidate, *lit_switch, *lit_default, *lit_expires, *lit_password, *lit_key, *lit_endpoint;

        lit_account = argument_create_literal("account", NULL, NULL);
        lit_list = argument_create_literal("list", account_list, _("list registered accounts"));
        lit_add = argument_create_literal("add", account_add, _("register a new OVH account"));
        lit_delete = argument_create_literal("delete", account_delete, _("remove an OVH account"));
        lit_default = argument_create_literal("default", account_default_set, _("set the default account"));
        lit_switch = argument_create_literal("switch", account_switch, _("switch to another OVH account"));
        lit_invalidate = argument_create_literal("invalidate", account_invalidate, _("drop consumer key associated to given OVH account"));
        lit_expires = argument_create_literal("expires", NULL, NULL);
        lit_update = argument_create_literal("update", account_update, _("modify a previously registered OVH account"));
        lit_key = argument_create_literal("key", NULL, NULL);
        lit_password = argument_create_literal("password", NULL, NULL);
        lit_endpoint = argument_create_relevant_literal(offsetof(account_argument_t, endpoint_present), "endpoint", NULL);

        arg_password = argument_create_string(offsetof(account_argument_t, password), "<password>", NULL, NULL);
        arg_expires_in_at = argument_create_choices(offsetof(account_argument_t, expires_in_at), "<in/at>",  expires_in_at);
        arg_expiration = argument_create_string(offsetof(account_argument_t, expiration), "<expiration>", NULL, NULL);
        arg_consumer_key = argument_create_string(offsetof(account_argument_t, consumer_key), "<consumer key>", NULL, NULL);
        arg_endpoint = argument_create_choices(offsetof(account_argument_t, endpoint), "<endpoint>", endpoint_names);
        arg_account = argument_create_string(offsetof(account_argument_t, account), "<account>", complete_from_statement, &statements[STMT_ACCOUNT_COMPLETION]);

        graph_create_full_path(g, lit_account, lit_list, NULL);
        graph_create_path(g, lit_account, lit_add, arg_account, NULL);
        graph_create_all_path(g, lit_add, NULL, 2, lit_password, arg_password, 5, lit_key, arg_consumer_key, lit_expires, arg_expires_in_at, arg_expiration, 2, lit_endpoint, arg_endpoint, 0);
        graph_create_full_path(g, lit_account, arg_account, lit_delete, NULL);
        graph_create_full_path(g, lit_account, arg_account, lit_default, NULL);
        graph_create_full_path(g, lit_account, arg_account, lit_switch, NULL);
        graph_create_full_path(g, lit_account, arg_account, lit_invalidate, NULL);

        graph_create_path(g, lit_account, lit_update, arg_account, NULL);
        graph_create_all_path(g, lit_update, NULL, 2, lit_password, arg_password, 5, lit_key, arg_consumer_key, lit_expires, arg_expires_in_at, arg_expiration, 2, lit_endpoint, arg_endpoint, 0);
    }
    // application ...
    {
        argument_t *arg_key, *arg_app_secret, *arg_endpoint;
        argument_t *lit_application, *lit_add, *lit_list, *lit_delete;

        lit_application = argument_create_literal("application", NULL, NULL);
        lit_add = argument_create_literal("add", application_add, _("register a new OVH application"));
        lit_list = argument_create_literal("list", application_list, _("list registered applications"));
        lit_delete = argument_create_literal("delete", application_delete, _("remove an application"));

        arg_endpoint = argument_create_choices(offsetof(application_t, endpoint_id), "<endpoint>", endpoint_names);
        arg_key = argument_create_string(offsetof(application_t, key), "<key>", NULL, NULL);
        arg_app_secret = argument_create_string(offsetof(application_t, secret), "<secret>", NULL, NULL);

        graph_create_full_path(g, lit_application, lit_list, NULL);
        graph_create_full_path(g, lit_application, arg_endpoint, lit_add, arg_key, arg_app_secret, NULL);
        graph_create_full_path(g, lit_application, arg_endpoint, lit_delete, NULL);
    }
}

DECLARE_MODULE(account) = {
    "account",
    account_regcomm,
    NULL,
    account_early_ctor,
    account_late_ctor,
    account_dtor
};
