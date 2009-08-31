#define _GNU_SOURCE
#include "daemon.h"

/*
 * File-scoped definition of the database settings we've tried
 * (or succeeded) connecting with
 */
static struct {
	char *host;
	char *name;
	char *user;
	char *pass;
	char *table;
	char *type;
	char *encoding;
	unsigned int port;
	dbi_conn conn;
	dbi_result result;
} db;

#undef ESC_BUFSIZE
#define ESC_BUFSIZE (8192 * 2)
#undef ESC_BUFS
#define ESC_BUFS 8 /* must be a power of 2 */
#define MAX_ESC_STRING ((ESC_BUFSIZE * 2) + 1)

/*
 * Quotes a string and escapes all meta-characters inside the string.
 * **dst must be free()'d by the caller.
 */
size_t sql_quote(const char *src, char **dst)
{
	if (!src) {
		*dst = NULL;
		return 0;
	}
	return dbi_conn_quote_string_copy(db.conn, src, dst);
}


/*
 * Escapes a string to make it suitable for use in sql-queries.
 * It's up to the caller to put quotation marks around the string
 * in the query. Use "sql_quote()" instead, since that has fewer
 * operations and causes the sql queries to be shorter.
 */
size_t sql_escape(const char *src, char **dst)
{
	size_t len;
	len = dbi_conn_quote_string_copy(db.conn, src, dst);
	*dst[len] = 0;
	(*dst)++;
	return len - 2;
}

/*
 * these two functions are only here to allow callers
 * access to error reporting without having to expose
 * the db-link to theh callers. It's also nifty as we
 * want to remain database layer agnostic
 */
int sql_error(const char **msg)
{
	if (!db.conn) {
		*msg = strdup("no database connection");
		return DBI_ERROR_NOCONN;
	}

	return dbi_conn_error(db.conn, msg);
}

void sql_free_result(void)
{
	if (db.result) {
		dbi_result_free(db.result);
		db.result = NULL;
	}
}

dbi_result sql_get_result(void)
{
	return db.result;
}


int sql_query(const char *fmt, ...)
{
	char *query;
	int len, db_error;
	va_list ap;

	if (!use_database) {
		lerr("Not using a database, but daemon still issued a query");
		return -1;
	}

	va_start(ap, fmt);
	len = vasprintf(&query, fmt, ap);
	va_end(ap);

	if (len == -1 || !query) {
		linfo("sql_query: Failed to build query from format-string '%s'", fmt);
		return -1;
	}
	db.result = dbi_conn_query_null(db.conn, (unsigned char *)query, len);
	if (!db.result) {
		const char *error_msg;
		db_error = sql_error(&error_msg);
		linfo("dbi_conn_query_null(): Failed to run [%s]: %s",
			  query, error_msg);

		/*
		 * if we failed because the connection has gone away, we try
		 * reconnecting once and rerunning the query before giving up.
		 */
		if (db_error == DBI_ERROR_NOCONN && !sql_reinit()) {
			db.result = dbi_conn_query_null(db.conn, (unsigned char *)query, len);
			/* database backlog code goes here */
		}
	}

	free(query);

	return !!db.result;
}


int sql_init(void)
{
	int result;
	dbi_driver driver;

	if (!use_database)
		return 0;

	result = dbi_initialize(NULL);
	if (result < 1) {
		lerr("Failed to initialize any libdbi drivers");
		return -1;
	}

	if (!db.type)
		db.type = "mysql";

	driver = dbi_driver_open(db.type);
	if (!driver) {
		lerr("Failed to open libdbi driver '%s'", db.type);
		return -1;
	}

	db.conn = dbi_conn_open(driver);
	if (!db.conn) {
		lerr("Failed to create a database connection instance");
		return -1;
	}

	result = dbi_conn_set_option(db.conn, "host", db.host ? db.host : "localhost");
	result |= dbi_conn_set_option(db.conn, "username", db.user ? db.user : "merlin");
	result |= dbi_conn_set_option(db.conn, "password", db.pass ? db.pass : "merlin");
	result |= dbi_conn_set_option(db.conn, "dbname", db.name ? db.name : "merlin");
	if (db.port)
		result |= dbi_conn_set_option_numeric(db.conn, "port", db.port);
	result |= dbi_conn_set_option(db.conn, "encoding", db.encoding ? db.encoding : "UTF-8");
	if (result) {
		lerr("Failed to set one or more database connection options");
	}

	if (dbi_conn_connect(db.conn) < 0) {
		const char *error_msg;
		sql_error(&error_msg);
		lerr("Failed to connect to '%s' at '%s':'%d' using %s:%s as credentials: %s",
		     db.name, db.host, db.port, db.user, db.pass, error_msg);

		db.conn = NULL;
		return -1;
	}

	return 0;
}


int sql_close(void)
{
	if (!use_database)
		return 0;

	if (db.conn)
		dbi_conn_close(db.conn);

	dbi_shutdown();
	db.conn = NULL;
	return 0;
}


int sql_reinit(void)
{
	sql_close();
	return sql_init();
}

/*
 * Some nifty helper functions which still allow us to keep the
 * db struct in file scope
 */

const char *sql_db_name(void)
{
	return db.name ? db.name : "merlin";
}

const char *sql_db_user(void)
{
	return db.user ? db.user : "merlin";
}

const char *sql_db_pass(void)
{
	return db.pass ? db.pass : "merlin";
}

const char *sql_db_host(void)
{
	return db.host ? db.host : "localhost";
}

const char *sql_table_name(void)
{
	return db.table ? db.table : "report_data";
}


/*
 * Config parameters from the "database" section end up here
 */
int sql_config(const char *key, const char *value)
{
	char *value_cpy;

	if (value)
		value_cpy = strdup(value);
	else
		value_cpy = NULL;

	if (!prefixcmp(key, "name") || !prefixcmp(key, "database"))
		db.name = value_cpy;
	else if (!prefixcmp(key, "user"))
		db.user = value_cpy;
	else if (!prefixcmp(key, "pass"))
		db.pass = value_cpy;
	else if (!prefixcmp(key, "host"))
		db.host = value_cpy;
	else if (!prefixcmp(key, "type"))
		db.type = value_cpy;
	else if (!prefixcmp(key, "port") && value) {
		char *endp;
		free(value_cpy);
		db.port = (unsigned int)strtoul(value, &endp, 0);

		if (endp == value || *endp != 0)
			return -1;
	}
	else
		return -1; /* config error */

	return 0;
}
