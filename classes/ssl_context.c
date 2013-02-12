/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2013 The PHP Group                                |
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

#ifndef HAVE_EVENT_OPENSSL_LIB
# error "HAVE_EVENT_OPENSSL_LIB undefined"
#endif

/* {{{ Private */

/* {{{ passwd_callback */
static int passwd_callback(char *buf, int num, int verify, void *data)
{
    HashTable  *ht  = (HashTable *) data;
    zval      **val = NULL;

	if (zend_hash_find(ht, "passphrase", sizeof("passphrase"),
				(void **) &val) == SUCCESS) {
        if (Z_STRLEN_PP(val) < num - 1) {
            memcpy(buf, Z_STRVAL_PP(val), Z_STRLEN_PP(val) + 1);
            return Z_STRLEN_PP(val);
        }
    }

    return 0;
}
/* }}} */

/* {{{ set_ca */
static zend_always_inline void set_ca(SSL_CTX *ctx, const char *cafile, const char *capath TSRMLS_DC) {
    if (!SSL_CTX_load_verify_locations(ctx, cafile, capath)) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING,
        		"Unable to set verify locations `%s' `%s'",
        		cafile, capath);
    }
}
/* }}} */

/* {{{ set_ciphers */
static zend_always_inline void set_ciphers(SSL_CTX *ctx, const char *cipher_list TSRMLS_DC)
{
	if (SSL_CTX_set_cipher_list(ctx, cipher_list) != 1) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING,
            	"Failed setting cipher list: `%s'", cipher_list);
	}
}
/* }}} */

/* {{{ set_local_cert */
static int set_local_cert(SSL_CTX *ctx, const char *certfile, const char *private_key TSRMLS_DC)
{
	char resolved_path_buff[MAXPATHLEN];

    if (VCWD_REALPATH(certfile, resolved_path_buff)) {
        if (SSL_CTX_use_certificate_chain_file(ctx, resolved_path_buff) != 1) {
            php_error_docref(NULL TSRMLS_CC, E_WARNING,
            		"SSL_CTX_use_certificate_chain_file failed, file: `%s'", certfile);
            return -1;
        }

        if (private_key) {
            char resolved_path_buff_pk[MAXPATHLEN];

            if (VCWD_REALPATH(private_key, resolved_path_buff_pk)) {
                if (SSL_CTX_use_PrivateKey_file(ctx, resolved_path_buff_pk, SSL_FILETYPE_PEM) != 1) {
                    php_error_docref(NULL TSRMLS_CC, E_WARNING,
                    		"Unable to set private key file `%s'",
                    		resolved_path_buff_pk);
                    return -1;
                }
            }
        } else {
            if (SSL_CTX_use_PrivateKey_file(ctx, resolved_path_buff, SSL_FILETYPE_PEM) != 1) {
                php_error_docref(NULL TSRMLS_CC, E_WARNING,
                		"Unable to set private key file `%s'",
                		resolved_path_buff);
                return -1;
            }
        }
    }

    return 0;
}
/* }}} */

/* {{{ set_ssl_ctx_options */
static inline void set_ssl_ctx_options(SSL_CTX *ctx, HashTable *ht TSRMLS_DC)
{
	HashPosition  pos         = 0;
	zend_bool     got_ciphers = 0;
	char         *cafile      = NULL;
	char         *capath      = NULL;

	for (zend_hash_internal_pointer_reset_ex(ht, &pos);
			zend_hash_has_more_elements_ex(ht, &pos) == SUCCESS;
			zend_hash_move_forward_ex(ht, &pos)) {
		char   *key;
		uint    keylen;
		ulong   idx;
		int     type;
		zval  **ppzval;

		type = zend_hash_get_current_key_ex(ht, &key, &keylen,
				&idx, 0, &pos);
		if (type != HASH_KEY_IS_LONG) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Invalid option `%s'", key);
			continue;
		}

		if (zend_hash_get_current_data_ex(ht, (void **) &ppzval, &pos) == FAILURE) {
			continue;
		}

		switch (idx) {
			case PHP_EVENT_OPT_LOCAL_CERT:
				convert_to_string_ex(ppzval);
				zval **ppz_private_key;

				if (zend_hash_index_find(ht, PHP_EVENT_OPT_LOCAL_PK,
						(void **) &ppz_private_key) == SUCCESS) {
					set_local_cert(ctx, Z_STRVAL_PP(ppzval), Z_STRVAL_PP(ppz_private_key));
				} else {
					set_local_cert(ctx, Z_STRVAL_PP(ppzval), NULL);
				}
				break;
			case PHP_EVENT_OPT_LOCAL_PK:
				/* Skip. SSL_CTX_use_PrivateKey_file is applied in "local_cert". */
				break;
			case PHP_EVENT_OPT_PASSPHRASE:
				convert_to_string_ex(ppzval);
        		SSL_CTX_set_default_passwd_cb_userdata(ctx, ht);
        		SSL_CTX_set_default_passwd_cb(ctx, passwd_callback);
				break;
			case PHP_EVENT_OPT_CA_FILE:
				convert_to_string_ex(ppzval);
				cafile = Z_STRVAL_PP(ppzval);
				break;
			case PHP_EVENT_OPT_CA_PATH:
				convert_to_string_ex(ppzval);
				capath = Z_STRVAL_PP(ppzval);
				break;
			case PHP_EVENT_OPT_ALLOW_SELF_SIGNED:
				/* Skip */
				break;
			case PHP_EVENT_OPT_VERIFY_PEER:
				/* Skip */
				break;
			case PHP_EVENT_OPT_VERIFY_DEPTH:
				convert_to_long_ex(ppzval);
				SSL_CTX_set_verify_depth(ctx, Z_LVAL_PP(ppzval));
				break;
			case PHP_EVENT_OPT_CIPHERS:
				got_ciphers = 1;
				convert_to_string_ex(ppzval);
				set_ciphers(ctx, Z_STRVAL_PP(ppzval));
				break;
			default:
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
						"Unknown option %ld", idx);
		}
	}

	if (got_ciphers == 0) {
		set_ciphers(ctx, "DEFAULT");
	}

	if (cafile || capath) {
		set_ca(ctx, cafile, capath);
	}
}
/* }}} */

/* {{{ get_ssl_method */
static zend_always_inline SSL_METHOD *get_ssl_method(long in_method)
{
	SSL_METHOD *method;

	switch (in_method) {
    	case PHP_EVENT_SSLv2_CLIENT_METHOD:
    		method = (SSL_METHOD *) SSLv2_method();
			break;
    	case PHP_EVENT_SSLv3_CLIENT_METHOD:
    		method = (SSL_METHOD *) SSLv3_method();
			break;
    	case PHP_EVENT_SSLv23_CLIENT_METHOD:
    		method = (SSL_METHOD *) SSLv23_method();
			break;
    	case PHP_EVENT_TLS_CLIENT_METHOD:
    		method = (SSL_METHOD *) TLSv1_method();
			break;
    	case PHP_EVENT_SSLv2_SERVER_METHOD:
    		method = (SSL_METHOD *) SSLv2_server_method();
			break;
    	case PHP_EVENT_SSLv3_SERVER_METHOD:
    		method = (SSL_METHOD *) SSLv3_server_method();
			break;
    	case PHP_EVENT_SSLv23_SERVER_METHOD:
    		method = (SSL_METHOD *) SSLv23_server_method();
			break;
    	case PHP_EVENT_TLS_SERVER_METHOD:
    		method = (SSL_METHOD *) TLSv1_server_method();
    		break;
    	default:
    		return NULL;
	}

	return method;
}
/* }}} */

/* Private }}} */


/* {{{ proto EventSslContext EventSslContext::__construct(int method, array options);
 *
 * Creates SSL context holding pointer to SSL_CTX.
 * method parameter is one of EventSslContext::*_METHOD constants.
 * options parameter is an associative array of SSL context options */
PHP_METHOD(EventSslContext, __construct)
{
	php_event_ssl_context_t *ectx;
	HashTable               *ht_options;
	long                     in_method;
	SSL_METHOD              *method;
	SSL_CTX                 *ctx;
	long                     options    = SSL_OP_ALL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "lh",
				&in_method, &ht_options) == FAILURE) {
		return;
	}

	method = get_ssl_method(in_method);
	if (method == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"Invalid method passed: %ld", in_method);
		return;
	}

	ctx = SSL_CTX_new(method);
	if (ctx == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"Creation of a new SSL_CTX object failed");
		return;
	}

	PHP_EVENT_FETCH_SSL_CONTEXT(ectx, getThis());
	ectx->ctx = ctx;

	ALLOC_HASHTABLE(ectx->ht);
	zend_hash_init_ex(ectx->ht, zend_hash_num_elements(ht_options), NULL, NULL, 0, 1);
	zend_hash_copy(ectx->ht, ht_options, (copy_ctor_func_t) zval_add_ref,
			(void *) NULL, sizeof(zval *));

	SSL_CTX_set_options(ectx->ctx, options);
	set_ssl_ctx_options(ectx->ctx, ectx->ht TSRMLS_CC);
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
