/*
   +----------------------------------------------------------------------+
   | PHP Version 7                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2016 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Ruslan Osmanov <osmanov@php.net>                             |
   +----------------------------------------------------------------------+
*/

#include "src/common.h"
#include "src/util.h"
#include "src/priv.h"
#include "classes/http.h"


zend_class_entry *php_event_ce;
zend_class_entry *php_event_base_ce;
zend_class_entry *php_event_config_ce;
zend_class_entry *php_event_bevent_ce;
zend_class_entry *php_event_buffer_ce;
zend_class_entry *php_event_util_ce;
#ifdef HAVE_EVENT_EXTRA_LIB
zend_class_entry *php_event_dns_base_ce;
zend_class_entry *php_event_listener_ce;
zend_class_entry *php_event_http_conn_ce;
zend_class_entry *php_event_http_ce;
zend_class_entry *php_event_http_req_ce;
#endif
#ifdef HAVE_EVENT_OPENSSL_LIB
zend_class_entry *php_event_ssl_context_ce;
#endif

static HashTable classes;

static HashTable event_properties;
static HashTable event_bevent_properties;
static HashTable event_buffer_properties;
static HashTable event_listener_properties;
#ifdef HAVE_EVENT_OPENSSL_LIB
static HashTable event_ssl_context_properties;
int php_event_ssl_data_index;
#endif

static zend_object_handlers event_event_object_handlers;
static zend_object_handlers event_base_object_handlers;
static zend_object_handlers event_config_object_handlers;
static zend_object_handlers event_bevent_object_handlers;
static zend_object_handlers event_buffer_object_handlers;
static zend_object_handlers event_util_object_handlers;
#if HAVE_EVENT_EXTRA_LIB
static zend_object_handlers event_dns_base_object_handlers;
static zend_object_handlers event_listener_object_handlers;
static zend_object_handlers event_http_conn_object_handlers;
static zend_object_handlers event_http_object_handlers;
static zend_object_handlers event_http_req_object_handlers;
#endif
#ifdef HAVE_EVENT_OPENSSL_LIB
static zend_object_handlers event_ssl_context_object_handlers;
#endif

static const zend_module_dep event_deps[] = {
#ifdef PHP_EVENT_SOCKETS_SUPPORT
	ZEND_MOD_REQUIRED("sockets")
#endif
	ZEND_MOD_END
};

/* {{{ event_module_entry */
zend_module_entry event_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	event_deps,
	"event",
	NULL, /*event_functions*/
	PHP_MINIT(event),
	PHP_MSHUTDOWN(event),
	PHP_RINIT(event),
	NULL, /*PHP_RSHUTDOWN(event),*/
	PHP_MINFO(event),
	PHP_EVENT_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_EVENT
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(event)
#endif


/* {{{ Private functions */

static void free_prop_handler(zval *el)/*{{{*/
{
	pefree(Z_PTR_P(el), 1);
}/*}}}*/

static void php_event_event_free_obj(zend_object *object)/*{{{*/
{
	php_event_t *e = Z_EVENT_X_FETCH_OBJ(event, object);

	PHP_EVENT_ASSERT(e);

	if (e->event) {
		/* No need in event_del(e->event) since event_free makes event non-pending internally */
		event_free(e->event);
		e->event = NULL;
	}

#if 0
	if (e->stream_res) { /* stdin fd == 0 */
		/* In php5 we had decremented resource reference counter */
		e->stream_res = NULL;
	}
#endif

	if (!Z_ISUNDEF(e->data)) {
		zval_ptr_dtor(&e->data);
	}

	php_event_free_callback(&e->cb);

	Z_EVENT_STD_OBJ_DTOR(e);
}/*}}}*/

static void php_event_base_free_obj(zend_object *object)/*{{{*/
{
	php_event_base_t *b = Z_EVENT_X_FETCH_OBJ(base, object);

	PHP_EVENT_ASSERT(b);

	if (!b->internal && b->base) {
		event_base_free(b->base);
		b->base = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(b);
}/*}}}*/

static void php_event_config_free_obj(zend_object *object)/*{{{*/
{
	php_event_config_t *cfg = Z_EVENT_X_FETCH_OBJ(config, object);

	PHP_EVENT_ASSERT(cfg);

	if (cfg->ptr) {
		event_config_free(cfg->ptr);
		cfg->ptr = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(cfg);
}/*}}}*/

static void php_event_bevent_free_obj(zend_object *object)/*{{{*/
{
	php_event_bevent_t *b = Z_EVENT_X_FETCH_OBJ(bevent, object);

	if (EXPECTED(b)) {
		if (!Z_ISUNDEF(b->data)) {
			zval_ptr_dtor(&b->data);
		}

		php_event_free_callback(&b->cb_read);
		php_event_free_callback(&b->cb_write);
		php_event_free_callback(&b->cb_event);

		if (Z_REFCOUNT(b->self) > 1) {
			zval_ptr_dtor(&b->self);
		}

		if (!Z_ISUNDEF(b->base)) {
			zval_ptr_dtor(&b->base);
		}

		if (b->bevent) {
			bufferevent_free(b->bevent);
			b->bevent = NULL;
		}

		if (!Z_ISUNDEF(b->input)) {
			zval_ptr_dtor(&b->input);
		}

		if (!Z_ISUNDEF(b->output)) {
			zval_ptr_dtor(&b->output);
		}
	}

	Z_EVENT_STD_OBJ_DTOR(b);
}/*}}}*/

static void php_event_buffer_free_obj(zend_object *object)/*{{{*/
{
	php_event_buffer_t *b = Z_EVENT_X_FETCH_OBJ(buffer, object);

	PHP_EVENT_ASSERT(b);

	/* If we got the buffer in, say, a read callback the buffer
	 * is destroyed when the callback is done as any normal variable.
	 * Zend MM calls destructor which eventually calls this function.
	 * We'll definitely crash, if we call evbuffer_free() on an internal
	 * bufferevent buffer. */

	if (!b->internal && b->buf) {
		evbuffer_free(b->buf);
		b->buf = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(b);
}/*}}}*/

#ifdef HAVE_EVENT_EXTRA_LIB

static void php_event_dns_base_free_obj(zend_object *object)/*{{{*/
{
	php_event_dns_base_t *dnsb = Z_EVENT_X_FETCH_OBJ(dns_base, object);

	PHP_EVENT_ASSERT(dnsb);

	if (dnsb->dns_base) {
		/* Setting fail_requests to 1 makes all in-flight requests get
		 * their callbacks invoked with a canceled error code before it
		 * frees the base*/
		evdns_base_free(dnsb->dns_base, 1);
		dnsb->dns_base = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(dnsb);
}/*}}}*/

static void php_event_listener_free_obj(zend_object *object)/*{{{*/
{
	php_event_listener_t *l = Z_EVENT_X_FETCH_OBJ(listener, object);

	PHP_EVENT_ASSERT(l);

	if (!Z_ISUNDEF(l->data)) {
		zval_ptr_dtor(&l->data);
	}

	if (Z_REFCOUNT(l->self) > 1) {
		zval_ptr_dtor(&l->self);
	}

	php_event_free_callback(&l->cb);
	php_event_free_callback(&l->cb_err);

	if (l->listener) {
		evconnlistener_free(l->listener);
		l->listener = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(l);
}/*}}}*/

static void php_event_http_conn_free_obj(zend_object *object)/*{{{*/
{
	php_event_http_conn_t *evcon = Z_EVENT_X_FETCH_OBJ(http_conn, object);

	PHP_EVENT_ASSERT(evcon);

	php_event_free_callback(&evcon->cb_close);

	if (Z_REFCOUNT(evcon->self) > 1) {
		zval_ptr_dtor(&evcon->self);
	}

	if (!Z_ISUNDEF(evcon->data_closecb)) {
		zval_ptr_dtor(&evcon->data_closecb);
	}

	if (!Z_ISUNDEF(evcon->base)) {
		zval_ptr_dtor(&evcon->base);
	}

	if (!Z_ISUNDEF(evcon->dns_base)) {
		zval_ptr_dtor(&evcon->dns_base);
	}

	if (evcon->conn) {
		evhttp_connection_free(evcon->conn);
		evcon->conn = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(evcon);
}/*}}}*/

static void php_event_http_free_obj(zend_object *object)/*{{{*/
{
	php_event_http_cb_t *cb, *cb_next;
	php_event_http_t *http = Z_EVENT_X_FETCH_OBJ(http, object);

	PHP_EVENT_ASSERT(http);

	php_event_free_callback(&http->cb);

	/* Free attached callbacks */
	cb = http->cb_head;
	while (cb) {
		cb_next = cb->next;
		_php_event_free_http_cb(cb);
		cb = cb_next;
	}

	if (!Z_ISUNDEF(http->data)) {
		zval_ptr_dtor(&http->data);
	}

	if (!Z_ISUNDEF(http->base)) {
		zval_ptr_dtor(&http->base);
	}

	if (http->ptr) {
		evhttp_free(http->ptr);
		http->ptr = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(http);
}/*}}}*/

static void php_event_http_req_free_obj(zend_object *object)/*{{{*/
{
	php_event_http_req_t *http_req = Z_EVENT_X_FETCH_OBJ(http_req, object);

	PHP_EVENT_ASSERT(http_req);

	php_event_free_callback(&http_req->cb);

	if (Z_REFCOUNT(http_req->self) > 1) {
		zval_ptr_dtor(&http_req->self);
	}
	if (!Z_ISUNDEF(http_req->data)) {
		zval_ptr_dtor(&http_req->data);
	}

#if 0
	/*
	Libevent cleans up http_req->ptr despite the ownership of the pointer
	(evhttp_request_own()). So we'll get SEGFAULT here if we call
	evhttp_request_free().
	*/

	if (!http_req->internal && http_req->ptr) {
		evhttp_request_free(http_req->ptr);
		http_req->ptr = NULL;
	}
#endif

	Z_EVENT_STD_OBJ_DTOR(http_req);
}/*}}}*/

#endif /* HAVE_EVENT_EXTRA_LIB */


#ifdef HAVE_EVENT_OPENSSL_LIB
static void php_event_ssl_context_free_obj(zend_object *object)/*{{{*/
{
	php_event_ssl_context_t *ectx = Z_EVENT_X_FETCH_OBJ(ssl_context, object);

	if (ectx->ctx) {
		SSL_CTX_free(ectx->ctx);
		ectx->ctx = NULL;
	}

	if (ectx->ht) {
		zend_hash_destroy(ectx->ht);
		FREE_HASHTABLE(ectx->ht);
		ectx->ht = NULL;
	}

	Z_EVENT_STD_OBJ_DTOR(ectx);
}/*}}}*/
#endif /* HAVE_EVENT_OPENSSL_LIB */


static zend_object * event_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_t);
	intern->zo.handlers = &event_event_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_base_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_base_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_base_t);
	intern->zo.handlers = &event_base_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_config_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_config_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_config_t);
	intern->zo.handlers = &event_config_object_handlers;

	return &intern->zo;

}/*}}}*/

static zend_object * event_bevent_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_bevent_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_bevent_ce);
	intern->zo.handlers = &event_bevent_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_buffer_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_buffer_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_buffer_ce);
	intern->zo.handlers = &event_buffer_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_util_object_create(zend_class_entry *ce)/*{{{*/
{
	/* EventUtil is a singleton. This function should never be called */
	PHP_EVENT_ASSERT(0);
	return NULL;
}/*}}}*/

#ifdef HAVE_EVENT_OPENSSL_LIB
static zend_object * event_ssl_context_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_ssl_context_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_ssl_context_ce);
	intern->zo.handlers = &event_ssl_context_object_handlers;

	return &intern->zo;
}/*}}}*/
#endif

#if HAVE_EVENT_EXTRA_LIB
static zend_object * event_dns_base_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_dns_base_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_dns_base_t);
	intern->zo.handlers = &event_dns_base_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_listener_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_listener_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_listener_t);
	intern->zo.handlers = &event_listener_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_http_conn_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_http_conn_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_http_conn_t);
	intern->zo.handlers = &event_http_conn_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_http_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_http_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_http_t);
	intern->zo.handlers = &event_http_object_handlers;

	return &intern->zo;
}/*}}}*/

static zend_object * event_http_req_object_create(zend_class_entry *ce)/*{{{*/
{
	php_event_http_req_t *intern;

	PHP_EVENT_OBJ_ALLOC(intern, ce, php_event_http_req_t);
	intern->zo.handlers = &event_http_req_object_handlers;

	return &intern->zo;
}/*}}}*/
#endif /* HAVE_EVENT_EXTRA_LIB */


/* {{{ fatal_error_cb
 * Is called when Libevent detects a non-recoverable internal error. */
static void fatal_error_cb(int err)
{
	php_error_docref(NULL, E_ERROR, "Libevent detected non-recoverable internal error, code: %d", err);
}
/* }}} */


#if LIBEVENT_VERSION_NUMBER < 0x02001900
# define PHP_EVENT_LOG_CONST(name) _ ## name
#else
# define PHP_EVENT_LOG_CONST(name) name
#endif

/* {{{ log_cb
 * Overrides libevent's default error logging(it logs to stderr) */
static void log_cb(int severity, const char *msg)
{
	int error_type;

	switch (severity) {
		case PHP_EVENT_LOG_CONST(EVENT_LOG_DEBUG):
			error_type = E_STRICT;
		case PHP_EVENT_LOG_CONST(EVENT_LOG_MSG):
			error_type = E_NOTICE;
		case PHP_EVENT_LOG_CONST(EVENT_LOG_WARN):
			error_type = E_WARNING;
		case PHP_EVENT_LOG_CONST(EVENT_LOG_ERR):
			error_type = E_ERROR;
		default:
			error_type = E_NOTICE;
	}

	php_error_docref(NULL, error_type, "%s", msg);
}
/* }}} */


static zval * read_property_default(void *obj, zval *retval)/*{{{*/
{
	ZVAL_NULL(retval);
	php_error_docref(NULL, E_ERROR, "Cannot read property");
	return retval;
}/*}}}*/

static int write_property_default(void *obj, zval *newval)/*{{{*/
{
	php_error_docref(NULL, E_ERROR, "Cannot write property");
	return FAILURE;
}/*}}}*/

static zval * read_property(zval *object, zval *member, int type, void **cache_slot, zval *rv, void *obj, HashTable *prop_handler)/*{{{*/
{
	zval                      tmp_member;
	zval                     *retval;
	php_event_prop_handler_t *hnd        = NULL;

	if (Z_TYPE_P(member) != IS_STRING) {
		ZVAL_COPY(&tmp_member, member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	if (prop_handler != NULL) {
		hnd = zend_hash_find_ptr(prop_handler, Z_STR_P(member));
	}

	if (hnd) {
		retval = hnd->read_func(obj, rv);
		if (retval == NULL) {
			retval = &EG(uninitialized_zval);
		}
	} else {
		zend_object_handlers *std_hnd = zend_get_std_object_handlers();
		retval = std_hnd->read_property(object, member, type, cache_slot, rv);
	}

	if (member == &tmp_member) {
		zval_dtor(&tmp_member);
	}

	return retval;
}/*}}}*/

static void write_property(zval *object, zval *member, zval *value, void **cache_slot, void *obj, HashTable *prop_handler)/*{{{*/
{
	zval                      tmp_member;
	php_event_prop_handler_t *hnd        = NULL;

	if (Z_TYPE_P(member) != IS_STRING) {
		ZVAL_COPY(&tmp_member, member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	if (prop_handler != NULL) {
	    hnd = zend_hash_find_ptr(prop_handler, Z_STR_P(member));
	}

	if (hnd) {
		hnd->write_func(obj, value);
	} else {
		zend_object_handlers *std_hnd = zend_get_std_object_handlers();
	    std_hnd->write_property(object, member, value, cache_slot);
	}

	if (member == &tmp_member) {
		zval_dtor(member);
	}
}/*}}}*/

static int object_has_property(zval *object, zval *member, int has_set_exists, void **cache_slot, void *obj, HashTable *prop_handler)/*{{{*/
{
	php_event_prop_handler_t *p;
	int                       ret = 0;

	if ((p = zend_hash_find_ptr(prop_handler, Z_STR_P(member))) != NULL) {
		switch (has_set_exists) {
			case 2:
				ret = 1;
				break;
			case 1: {
						zval rv;
						zval *value = read_property(object, member, BP_VAR_IS, cache_slot, &rv, obj, prop_handler);
						if (value != &EG(uninitialized_zval)) {
							convert_to_boolean(value);
							ret = Z_TYPE_P(value) == IS_TRUE ? 1 : 0;
						}
						break;
					}
			case 0:{
					   zval rv;
					   zval *value = read_property(object, member, BP_VAR_IS, cache_slot, &rv, obj, prop_handler);
					   if (value != &EG(uninitialized_zval)) {
						   ret = Z_TYPE_P(value) != IS_NULL ? 1 : 0;
						   zval_ptr_dtor(value);
					   }
					   break;
				   }
			default:
				   php_error_docref(NULL, E_WARNING, "Invalid value for has_set_exists");
		}
	} else {
		zend_object_handlers *std_hnd = zend_get_std_object_handlers();
		ret = std_hnd->has_property(object, member, has_set_exists, cache_slot);
	}

	return ret;
}/*}}}*/

static HashTable * object_get_debug_info(zval *object, int *is_temp, void *obj, HashTable *prop_handler)/*{{{*/
{
	HashTable                *props = prop_handler;
	HashTable                *retval;
	php_event_prop_handler_t *entry;

	ALLOC_HASHTABLE(retval);
	ZEND_INIT_SYMTABLE_EX(retval, zend_hash_num_elements(props) + 1, 0);

	ZEND_HASH_FOREACH_PTR(props, entry) {
		zval rv, member;
		zval *value;
		ZVAL_STR(&member, entry->name);
		value = read_property(object, &member, BP_VAR_IS, 0, &rv, obj, prop_handler);
		if (value != &EG(uninitialized_zval)) {
			zend_hash_add(retval, Z_STR(member), value);
		}
	} ZEND_HASH_FOREACH_END();

	*is_temp = 1;

	return retval;
}/*}}}*/

static zval * get_property_ptr_ptr(zval *object, zval *member, int type, void **cache_slot, void *obj, HashTable *prop_handler)/*{{{*/
{
	zval                     *retval     = NULL;
	php_event_prop_handler_t *hnd        = NULL;
	zval                      tmp_member;

	if (Z_TYPE_P(member) != IS_STRING) {
		ZVAL_COPY(&tmp_member, member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
		cache_slot = NULL;
	}

	if (prop_handler != NULL) {
		hnd = zend_hash_find_ptr(prop_handler, Z_STR_P(member));
	}

	if (hnd && hnd->get_ptr_ptr_func != NULL) {
		retval = hnd->get_ptr_ptr_func(obj);
	} else {
		zend_object_handlers *std_hnd = zend_get_std_object_handlers();
		retval = std_hnd->get_property_ptr_ptr(object, member, type, cache_slot);
	}

	if (Z_ISUNDEF_P(retval)) {
		ZVAL_NULL(retval);
	}

	if (member == &tmp_member) {
		zval_dtor(member);
	}

	return retval;
}/*}}}*/

static HashTable * get_properties(zval *object, void *obj, HashTable *prop_handler)/*{{{*/
{
	HashTable                *props = zend_std_get_properties(object);
	php_event_prop_handler_t *hnd;
	zend_string              *key;

	if (prop_handler == NULL) {
		return NULL;
	}

	ZEND_HASH_FOREACH_STR_KEY_PTR(prop_handler, key, hnd) {
		zval zret;
		if (hnd->read_func && hnd->read_func(obj, &zret)) {
			zend_hash_update(props, key, &zret);
		}
	} ZEND_HASH_FOREACH_END();

	return props;
}/*}}}*/

static HashTable * get_gc(zval *object, zval **gc_data, int *gc_count)/*{{{*/
{
	*gc_data = NULL;
	*gc_count = 0;
	return zend_std_get_properties(object);
}/*}}}*/

#define PHP_EVENT_X_PROP_HND_DECL(x)                                                                                        \
static zval * php_event_ ## x ## _read_property(zval *object, zval *member, int type, void **cache_slot, zval *rv) {        \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	return (obj ? read_property(object, member, type, cache_slot, rv, (void *)obj, obj->prop_handler) : NULL);              \
}                                                                                                                           \
static void php_event_ ## x ## _write_property(zval *object, zval *member, zval *value, void **cache_slot)                  \
{                                                                                                                           \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	if (EXPECTED(obj)) {                                                                                                    \
		write_property(object, member, value, cache_slot, (void *)obj, obj->prop_handler);                                  \
	}                                                                                                                       \
}                                                                                                                           \
static int php_event_ ## x ## _has_property(zval *object, zval *member, int has_set_exists, void **cache_slot)              \
{                                                                                                                           \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	return (obj ? object_has_property(object, member, has_set_exists, cache_slot, (void *)obj, obj->prop_handler) : 0);     \
}                                                                                                                           \
static HashTable * php_event_ ## x ## _get_debug_info(zval *object, int *is_temp)                                           \
{                                                                                                                           \
	HashTable *retval;                                                                                                      \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	if (EXPECTED(obj)) {                                                                                                    \
		retval = object_get_debug_info(object, is_temp, (void *)obj, obj->prop_handler);                                    \
	} else {                                                                                                                \
		ALLOC_HASHTABLE(retval);                                                                                            \
	}                                                                                                                       \
	return retval;                                                                                                          \
}                                                                                                                           \
static zval * php_event_ ## x ## _get_property_ptr_ptr(zval *object, zval *member, int type, void **cache_slot)             \
{                                                                                                                           \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	return (EXPECTED(obj) ? get_property_ptr_ptr(object, member, type, cache_slot, (void *)obj, obj->prop_handler) : NULL); \
}                                                                                                                           \
static HashTable * php_event_ ## x ## _get_properties(zval *object)                                                         \
{                                                                                                                           \
	HashTable *retval;                                                                                                      \
	Z_EVENT_X_OBJ_T(x) *obj = Z_EVENT_X_OBJ_P(x, object);                                                                   \
	if (EXPECTED(obj)) {                                                                                                    \
		retval = get_properties(object, (void *)obj, obj->prop_handler);                                                    \
	} else {                                                                                                                \
		ALLOC_HASHTABLE(retval);                                                                                            \
	}                                                                                                                       \
	return retval;                                                                                                          \
}

PHP_EVENT_X_PROP_HND_DECL(base)
PHP_EVENT_X_PROP_HND_DECL(event)
PHP_EVENT_X_PROP_HND_DECL(config)
PHP_EVENT_X_PROP_HND_DECL(buffer)
PHP_EVENT_X_PROP_HND_DECL(bevent)

#ifdef HAVE_EVENT_EXTRA_LIB
PHP_EVENT_X_PROP_HND_DECL(dns_base)
PHP_EVENT_X_PROP_HND_DECL(listener)
PHP_EVENT_X_PROP_HND_DECL(http)
PHP_EVENT_X_PROP_HND_DECL(http_conn)
PHP_EVENT_X_PROP_HND_DECL(http_req)
#endif /* HAVE_EVENT_EXTRA_LIB */

#ifdef HAVE_EVENT_OPENSSL_LIB
PHP_EVENT_X_PROP_HND_DECL(ssl_context)
#endif /* HAVE_EVENT_OPENSSL_LIB */

#define PHP_EVENT_ADD_CLASS_PROPERTIES(a, b)                                 \
{                                                                            \
	int i = 0;                                                               \
	while (b[i].name != NULL) {                                              \
		php_event_add_property((a), (b)[i].name, (b)[i].name_length,         \
				(php_event_prop_read_t)(b)[i].read_func,                     \
				(php_event_prop_write_t)(b)[i].write_func,                   \
				(php_event_prop_get_ptr_ptr_t)(b)[i].get_ptr_ptr_func); \
		i++;                                                                 \
	}                                                                        \
}

static void php_event_add_property(HashTable *h, const char *name, size_t name_len, php_event_prop_read_t read_func, php_event_prop_write_t write_func, php_event_prop_get_ptr_ptr_t get_ptr_ptr_func) {/*{{{*/
	php_event_prop_handler_t p;

	p.name             = zend_string_init(name, name_len, 1);
	p.read_func        = read_func ? read_func : read_property_default;
	p.write_func       = write_func ? write_func: write_property_default;
	p.get_ptr_ptr_func = get_ptr_ptr_func;
	zend_hash_add_mem(h, p.name, &p, sizeof(php_event_prop_handler_t));
	zend_string_release(p.name);
}/*}}}*/

static zend_always_inline void register_classes()/*{{{*/
{
	zend_class_entry *ce;

	PHP_EVENT_REGISTER_CLASS("Event", event_object_create, php_event_ce, php_event_ce_functions);
	ce = php_event_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;
	zend_hash_init(&event_properties, 2, NULL, free_prop_handler, 1);
	PHP_EVENT_ADD_CLASS_PROPERTIES(&event_properties, event_property_entries);
	PHP_EVENT_DECL_PROP_NULL(ce, "pending", ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "data",    ZEND_ACC_PUBLIC);
	zend_hash_add_ptr(&classes, ce->name, &event_properties);

	PHP_EVENT_REGISTER_CLASS("EventBase", event_base_object_create, php_event_base_ce,
			php_event_base_ce_functions);
	ce = php_event_base_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

	PHP_EVENT_REGISTER_CLASS("EventConfig", event_config_object_create, php_event_config_ce,
			php_event_config_ce_functions);
	ce = php_event_config_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

	PHP_EVENT_REGISTER_CLASS("EventBufferEvent", event_bevent_object_create, php_event_bevent_ce,
			php_event_bevent_ce_functions);
	ce = php_event_bevent_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;
	zend_hash_init(&event_bevent_properties, 4, NULL, free_prop_handler, 1);
	PHP_EVENT_ADD_CLASS_PROPERTIES(&event_bevent_properties, event_bevent_property_entries);
	PHP_EVENT_DECL_PROP_NULL(ce, "priority", ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "fd",       ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "input",    ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "output",   ZEND_ACC_PUBLIC);
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
	PHP_EVENT_DECL_PROP_NULL(ce, "allow_ssl_dirty_shutdown", ZEND_ACC_PUBLIC);
#endif
	zend_hash_add_ptr(&classes, ce->name, &event_bevent_properties);

	PHP_EVENT_REGISTER_CLASS("EventBuffer", event_buffer_object_create, php_event_buffer_ce,
			php_event_buffer_ce_functions);
	ce = php_event_buffer_ce;
	zend_hash_init(&event_buffer_properties, 2, NULL, free_prop_handler, 1);
	PHP_EVENT_ADD_CLASS_PROPERTIES(&event_buffer_properties, event_buffer_property_entries);
	PHP_EVENT_DECL_PROP_NULL(ce, "length",           ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "contiguous_space", ZEND_ACC_PUBLIC);
	zend_hash_add_ptr(&classes, ce->name, &event_buffer_properties);

#if HAVE_EVENT_EXTRA_LIB
	PHP_EVENT_REGISTER_CLASS("EventDnsBase", event_dns_base_object_create, php_event_dns_base_ce,
			php_event_dns_base_ce_functions);
	ce = php_event_dns_base_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

	PHP_EVENT_REGISTER_CLASS("EventListener", event_listener_object_create, php_event_listener_ce,
			php_event_listener_ce_functions);
	ce = php_event_listener_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;
	zend_hash_init(&event_listener_properties, 1, NULL, free_prop_handler, 1);
	PHP_EVENT_ADD_CLASS_PROPERTIES(&event_listener_properties, event_listener_property_entries);
	PHP_EVENT_DECL_PROP_NULL(ce, "fd", ZEND_ACC_PUBLIC);
	zend_hash_add_ptr(&classes, ce->name, &event_listener_properties);

	PHP_EVENT_REGISTER_CLASS("EventHttpConnection", event_http_conn_object_create,
			php_event_http_conn_ce,
			php_event_http_conn_ce_functions);
	ce = php_event_http_conn_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

	PHP_EVENT_REGISTER_CLASS("EventHttp", event_http_object_create, php_event_http_ce,
			php_event_http_ce_functions);
	ce = php_event_http_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

	PHP_EVENT_REGISTER_CLASS("EventHttpRequest", event_http_req_object_create, php_event_http_req_ce,
			php_event_http_req_ce_functions);
	ce = php_event_http_req_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;
#endif /* HAVE_EVENT_EXTRA_LIB */

	PHP_EVENT_REGISTER_CLASS("EventUtil", event_util_object_create, php_event_util_ce,
			php_event_util_ce_functions);
	ce = php_event_util_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;

#ifdef HAVE_EVENT_OPENSSL_LIB
	PHP_EVENT_REGISTER_CLASS("EventSslContext", event_ssl_context_object_create,
			php_event_ssl_context_ce,
			php_event_ssl_context_ce_functions);
	ce = php_event_ssl_context_ce;
	ce->ce_flags |= ZEND_ACC_FINAL;
	zend_hash_init(&event_ssl_context_properties, 2, NULL, free_prop_handler, 1);
	PHP_EVENT_ADD_CLASS_PROPERTIES(&event_ssl_context_properties, event_ssl_context_property_entries);
	PHP_EVENT_DECL_PROP_NULL(ce, "local_cert", ZEND_ACC_PUBLIC);
	PHP_EVENT_DECL_PROP_NULL(ce, "local_pk",   ZEND_ACC_PUBLIC);
	zend_hash_add_ptr(&classes, ce->name, &event_ssl_context_properties);
#endif /* HAVE_EVENT_OPENSSL_LIB */
}/*}}}*/

/* Private functions }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(event)
{
	zend_object_handlers *std_hnd = zend_get_std_object_handlers();

	memcpy(&event_event_object_handlers, std_hnd, sizeof(zend_object_handlers));
	event_event_object_handlers.clone_obj = NULL;
	event_event_object_handlers.get_gc    = get_gc;
	PHP_EVENT_SET_X_OBJ_HANDLERS(event);

#define PHP_EVENT_INIT_X_OBJ_HANDLERS(x) do { \
	memcpy(&PHP_EVENT_X_OBJ_HANDLERS(x), &event_event_object_handlers, sizeof(zend_object_handlers)); \
	PHP_EVENT_SET_X_OBJ_HANDLERS(x); \
} while (0)

	PHP_EVENT_INIT_X_OBJ_HANDLERS(base);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(config);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(bevent);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(buffer);
#if HAVE_EVENT_EXTRA_LIB
	PHP_EVENT_INIT_X_OBJ_HANDLERS(dns_base);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(listener);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(http_conn);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(http);
	PHP_EVENT_INIT_X_OBJ_HANDLERS(http_req);
#endif
#if 0
	PHP_EVENT_INIT_X_OBJ_HANDLERS(util);
#else
	memcpy(&PHP_EVENT_X_OBJ_HANDLERS(util), std_hnd, sizeof(zend_object_handlers));
	PHP_EVENT_X_OBJ_HANDLERS(util).clone_obj = NULL;
	PHP_EVENT_X_OBJ_HANDLERS(util).get_gc    = get_gc;
#endif
#ifdef HAVE_EVENT_OPENSSL_LIB
	PHP_EVENT_INIT_X_OBJ_HANDLERS(ssl_context);
#endif

	zend_hash_init(&classes, 4, NULL, NULL, 1);
	register_classes();

	/* Loop flags */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, LOOP_ONCE,     EVLOOP_ONCE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, LOOP_NONBLOCK, EVLOOP_NONBLOCK);

	/* Run-time flags of event base usually passed to event_config_set_flag */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, NOLOCK,               EVENT_BASE_FLAG_NOLOCK);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, STARTUP_IOCP,         EVENT_BASE_FLAG_STARTUP_IOCP);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, NO_CACHE_TIME,        EVENT_BASE_FLAG_NO_CACHE_TIME);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, EPOLL_USE_CHANGELIST, EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST);
#ifdef EVENT_BASE_FLAG_IGNORE_ENV
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, IGNORE_ENV,           IGNORE_ENV);
#endif
#ifdef EVENT_BASE_FLAG_PRECISE_TIMER
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_base_ce, PRECISE_TIMER,        PRECISE_TIMER);
#endif

	/* Event flags */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, ET,      EV_ET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, PERSIST, EV_PERSIST);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, READ,    EV_READ);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, WRITE,   EV_WRITE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, SIGNAL,  EV_SIGNAL);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ce, TIMEOUT, EV_TIMEOUT);

	/* Features of event_base usually passed to event_config_require_features */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_config_ce, FEATURE_ET,  EV_FEATURE_ET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_config_ce, FEATURE_O1,  EV_FEATURE_O1);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_config_ce, FEATURE_FDS, EV_FEATURE_FDS);

	/* Buffer event flags */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, READING,   BEV_EVENT_READING);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, WRITING,   BEV_EVENT_WRITING);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, EOF,       BEV_EVENT_EOF);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, ERROR,     BEV_EVENT_ERROR);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, TIMEOUT,   BEV_EVENT_TIMEOUT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, CONNECTED, BEV_EVENT_CONNECTED);

	/* Option flags for bufferevents */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, OPT_CLOSE_ON_FREE,    BEV_OPT_CLOSE_ON_FREE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, OPT_THREADSAFE,       BEV_OPT_THREADSAFE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, OPT_DEFER_CALLBACKS,  BEV_OPT_DEFER_CALLBACKS);
#if LIBEVENT_VERSION_NUMBER >= 0x02000500
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, OPT_UNLOCK_CALLBACKS, BEV_OPT_UNLOCK_CALLBACKS);
#endif
#ifdef HAVE_EVENT_OPENSSL_LIB
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, SSL_OPEN,       BUFFEREVENT_SSL_OPEN);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, SSL_CONNECTING, BUFFEREVENT_SSL_CONNECTING);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_bevent_ce, SSL_ACCEPTING,  BUFFEREVENT_SSL_ACCEPTING);
#endif

	/* Address families */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, AF_INET,   AF_INET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, AF_INET6,  AF_INET6);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, AF_UNIX,   AF_UNIX);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, AF_UNSPEC, AF_UNSPEC);

	/* Socket options */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_DEBUG,     SO_DEBUG);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_REUSEADDR, SO_REUSEADDR);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_KEEPALIVE, SO_KEEPALIVE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_DONTROUTE, SO_DONTROUTE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_LINGER,    SO_LINGER);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_BROADCAST, SO_BROADCAST);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_OOBINLINE, SO_OOBINLINE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_SNDBUF,    SO_SNDBUF);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_RCVBUF,    SO_RCVBUF);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_SNDLOWAT,  SO_SNDLOWAT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_RCVLOWAT,  SO_RCVLOWAT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_SNDTIMEO,  SO_SNDTIMEO);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_RCVTIMEO,  SO_RCVTIMEO);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_TYPE,      SO_TYPE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SO_ERROR,     SO_ERROR);
#ifdef TCP_NODELAY
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, TCP_NODELAY, TCP_NODELAY);
#endif

	/* Socket protocol levels */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SOL_SOCKET, SOL_SOCKET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SOL_TCP,    IPPROTO_TCP);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, SOL_UDP,    IPPROTO_UDP);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, IPPROTO_IP, IPPROTO_IP);
#if HAVE_IPV6
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, IPPROTO_IPV6, IPPROTO_IPV6);
#endif


#ifdef HAVE_EVENT_EXTRA_LIB
	/* DNS options */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_dns_base_ce, OPTION_SEARCH,      DNS_OPTION_SEARCH);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_dns_base_ce, OPTION_NAMESERVERS, DNS_OPTION_NAMESERVERS);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_dns_base_ce, OPTION_MISC,        DNS_OPTION_MISC);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_dns_base_ce, OPTION_HOSTSFILE,   DNS_OPTION_HOSTSFILE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_dns_base_ce, OPTIONS_ALL,        DNS_OPTIONS_ALL);

	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_LEAVE_SOCKETS_BLOCKING, LEV_OPT_LEAVE_SOCKETS_BLOCKING);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_CLOSE_ON_FREE,          LEV_OPT_CLOSE_ON_FREE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_CLOSE_ON_EXEC,          LEV_OPT_CLOSE_ON_EXEC);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_REUSEABLE,              LEV_OPT_REUSEABLE);
# if LIBEVENT_VERSION_NUMBER >= 0x02010100
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_DISABLED,               LEV_OPT_DISABLED);
# endif
# if LIBEVENT_VERSION_NUMBER >= 0x02000800
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_THREADSAFE,             LEV_OPT_THREADSAFE);
#endif
# if LIBEVENT_VERSION_NUMBER >= 0x02010100
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_listener_ce, OPT_DEFERRED_ACCEPT,        LEV_OPT_DEFERRED_ACCEPT);
# endif

	/* EventHttpRequest command types */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_GET,     EVHTTP_REQ_GET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_POST,    EVHTTP_REQ_POST);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_HEAD,    EVHTTP_REQ_HEAD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_PUT,     EVHTTP_REQ_PUT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_DELETE,  EVHTTP_REQ_DELETE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_OPTIONS, EVHTTP_REQ_OPTIONS);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_TRACE,   EVHTTP_REQ_TRACE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_CONNECT, EVHTTP_REQ_CONNECT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, CMD_PATCH,   EVHTTP_REQ_PATCH);

	/* EventHttpRequest header types */
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, INPUT_HEADER,  PHP_EVENT_REQ_HEADER_INPUT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_http_req_ce, OUTPUT_HEADER, PHP_EVENT_REQ_HEADER_OUTPUT);

#endif /* HAVE_EVENT_EXTRA_LIB */

	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_util_ce, LIBEVENT_VERSION_NUMBER, LIBEVENT_VERSION_NUMBER);

	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, EOL_ANY,         EVBUFFER_EOL_ANY);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, EOL_CRLF,        EVBUFFER_EOL_CRLF);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, EOL_CRLF_STRICT, EVBUFFER_EOL_CRLF_STRICT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, EOL_LF,          EVBUFFER_EOL_LF);
#if LIBEVENT_VERSION_NUMBER >= 0x02010100
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, EOL_NUL,         EVBUFFER_EOL_NUL);
#endif
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, PTR_SET,         EVBUFFER_PTR_SET);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_buffer_ce, PTR_ADD,         EVBUFFER_PTR_ADD);

#ifdef HAVE_EVENT_OPENSSL_LIB
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv2_CLIENT_METHOD,  PHP_EVENT_SSLv2_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv3_CLIENT_METHOD,  PHP_EVENT_SSLv3_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv23_CLIENT_METHOD, PHP_EVENT_SSLv23_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLS_CLIENT_METHOD,    PHP_EVENT_TLS_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv2_SERVER_METHOD,  PHP_EVENT_SSLv2_SERVER_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv3_SERVER_METHOD,  PHP_EVENT_SSLv3_SERVER_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, SSLv23_SERVER_METHOD, PHP_EVENT_SSLv23_SERVER_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLS_SERVER_METHOD,    PHP_EVENT_TLS_SERVER_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLSv11_CLIENT_METHOD, PHP_EVENT_TLSv11_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLSv11_SERVER_METHOD, PHP_EVENT_TLSv11_SERVER_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLSv12_CLIENT_METHOD, PHP_EVENT_TLSv12_CLIENT_METHOD);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, TLSv12_SERVER_METHOD, PHP_EVENT_TLSv12_SERVER_METHOD);

	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_LOCAL_CERT,               PHP_EVENT_OPT_LOCAL_CERT);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_LOCAL_PK,                 PHP_EVENT_OPT_LOCAL_PK);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_PASSPHRASE,               PHP_EVENT_OPT_PASSPHRASE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_CA_FILE,                  PHP_EVENT_OPT_CA_FILE);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_CA_PATH,                  PHP_EVENT_OPT_CA_PATH);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_ALLOW_SELF_SIGNED,        PHP_EVENT_OPT_ALLOW_SELF_SIGNED);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_VERIFY_PEER,              PHP_EVENT_OPT_VERIFY_PEER);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_VERIFY_DEPTH,             PHP_EVENT_OPT_VERIFY_DEPTH);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_CIPHERS,                  PHP_EVENT_OPT_CIPHERS);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_NO_SSLv2,                 PHP_EVENT_OPT_NO_SSLv2);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_NO_SSLv3,                 PHP_EVENT_OPT_NO_SSLv3);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_NO_TLSv1,                 PHP_EVENT_OPT_NO_TLSv1);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_NO_TLSv1_1,               PHP_EVENT_OPT_NO_TLSv1_1);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_NO_TLSv1_2,               PHP_EVENT_OPT_NO_TLSv1_2);
	PHP_EVENT_REG_CLASS_CONST_LONG(php_event_ssl_context_ce, OPT_CIPHER_SERVER_PREFERENCE, PHP_EVENT_OPT_CIPHER_SERVER_PREFERENCE);

	/* Initialize openssl library */
	SSL_library_init();
	OpenSSL_add_all_ciphers();
	OpenSSL_add_all_digests();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	/* Create new index which will be used to retreive custom data of the OpenSSL callbacks */
	php_event_ssl_data_index = SSL_get_ex_new_index(0, "PHP EventSslContext index", NULL, NULL, NULL);
#endif /* HAVE_EVENT_OPENSSL_LIB */

#ifdef PHP_EVENT_DEBUG
	event_enable_debug_mode();
#endif

#ifdef HAVE_EVENT_PTHREADS_LIB
# ifdef WIN32
	evthread_use_windows_threads();
# else
	if (evthread_use_pthreads()) {
		php_error_docref(NULL, E_ERROR,
				"evthread_use_pthreads failed, submit a bug");
	}
# endif
#endif /* HAVE_EVENT_PTHREADS_LIB */

	/* Handle libevent's error logging more gracefully than it's default
	 * logging to stderr, or calling abort()/exit() */
	event_set_fatal_callback(fatal_error_cb);
	event_set_log_callback(log_cb);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(event)
{
#ifdef HAVE_EVENT_OPENSSL_LIB
	/* Removes memory allocated when loading digest and cipher names
	 * in the OpenSSL_add_all_ family of functions */
	EVP_cleanup();
#endif

#if LIBEVENT_VERSION_NUMBER >= 0x02010000
	/* libevent_global_shutdown is available since libevent 2.1.0-alpha.
	 *
	 * Make sure that libevent has released all internal library-global data
	 * structures. Don't call any of libevent functions below! */
	libevent_global_shutdown();
#endif

	zend_hash_destroy(&event_properties);
	zend_hash_destroy(&event_bevent_properties);
	zend_hash_destroy(&event_buffer_properties);
	zend_hash_destroy(&event_listener_properties);
#ifdef HAVE_EVENT_OPENSSL_LIB
	zend_hash_destroy(&event_ssl_context_properties);
#endif

	zend_hash_destroy(&classes);

	return SUCCESS;
}
/* }}} */

/*{{{ PHP_RINIT_FUNCTION */
PHP_RINIT_FUNCTION(event)
{
#if defined(COMPILE_DL_EVENT) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}
/*}}}*/

/*{{{ PHP_RSHUTDOWN_FUNCTION */
#if 0
PHP_RSHUTDOWN_FUNCTION(event)
{
	return SUCCESS;
}
#endif
/*}}}*/

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(event)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "Event support", "enabled");
#ifdef PHP_EVENT_SOCKETS_SUPPORT
	php_info_print_table_header(2, "Sockets support", "enabled");
#else
	php_info_print_table_header(2, "Sockets support", "disabled");
#endif
#ifdef PHP_EVENT_DEBUG
	php_info_print_table_row(2, "Debug support", "enabled");
#else
	php_info_print_table_row(2, "Debug support", "disabled");
#endif
#ifdef HAVE_EVENT_EXTRA_LIB
	php_info_print_table_row(2, "Extra functionality support including HTTP, DNS, and RPC", "enabled");
#else
	php_info_print_table_row(2, "Extra functionality support including HTTP, DNS, and RPC", "disabled");
#endif
#ifdef HAVE_EVENT_OPENSSL_LIB
	php_info_print_table_row(2, "OpenSSL support", "enabled");
#else
	php_info_print_table_row(2, "OpenSSL support", "disabled");
#endif
#ifdef HAVE_EVENT_PTHREADS_LIB
	php_info_print_table_row(2, "Thread safety support", "enabled");
#else
	php_info_print_table_row(2, "Thread safety support", "disabled");
#endif

	php_info_print_table_row(2, "Extension version", PHP_EVENT_VERSION);
	php_info_print_table_row(2, "libevent2 headers version", LIBEVENT_VERSION);
	php_info_print_table_end();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 sts=4 fdm=marker
 * vim<600: noet sw=4 ts=4 sts=4
 */
