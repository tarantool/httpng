#include "openssl_utils.h"
#include <h2o.h>
#include <assert.h>
#include <stdio.h>

/* NOTE: only PEM format certificate is allowed */
X509 *get_X509_from_certificate_path(const char *cert_path, const char **lerr) {
	X509 *cert = NULL;
	FILE *fp = NULL;

	if (cert_path == NULL) {
		*lerr = "cert_path = NULL in get_X509_from_certificate_path";
		goto end;
		return NULL;
	}

	fp = fopen(cert_path, "r");
	if (!fp) {
		*lerr = "unable to open certificate file";
		goto end;
	}
	cert = PEM_read_X509(fp, NULL, NULL, NULL);
	if (!cert) {
		*lerr = "unable to parse certificate: only PEM format is allowed";
		goto fopen_close;
	}

fopen_close:
	fclose(fp);
end:
	if (cert == NULL)
		assert(*lerr != NULL);
	return cert;
}

const char *get_subject_common_name(X509 *cert) {
	if (cert == NULL) {
		return NULL;
	}
	X509_NAME *subj = X509_get_subject_name(cert);

	int length = X509_NAME_get_text_by_NID(subj, NID_commonName, NULL, 0);
	char *common_name = calloc(length + 1, sizeof(*common_name));
	int retval = X509_NAME_get_text_by_NID(subj, NID_commonName, common_name, length + 1);
	if (retval < 0) {
		free((char *)common_name);
		return NULL;
	}
	return common_name;
}

/* NOTE: only PEM format is allowed */
SSL_CTX *make_ssl_ctx(const char *certificate_file, const char *key_file,
						int level, long min_proto_version, const char **lerr) {
	SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx == NULL) {
		*lerr = "memory allocation for ssl context failed";
		goto make_ssl_ctx_error;
	}

	SSL_CTX_set_min_proto_version(ssl_ctx, min_proto_version);

	SSL_CTX_set_security_level(ssl_ctx, level);

	if (certificate_file != NULL && key_file != NULL) {
		if (SSL_CTX_use_certificate_chain_file(ssl_ctx, certificate_file) != 1) {
			*lerr = "can't bind certificate file to ssl_ctx";
			fprintf(stderr, "certificate file: %s\n", certificate_file);
			goto make_ssl_ctx_error;
		}
		if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) != 1) {
			*lerr = "can't bind private key to ssl_ctx";
			goto make_ssl_ctx_error;
		}
		if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
			*lerr = "check private key with certificate failed";
			goto make_ssl_ctx_error;
		}
	}

/* Setup protocol negotiation methods. */
#if H2O_USE_NPN
	h2o_ssl_register_npn_protocols(ssl_ctx, h2o_http2_npn_protocols);
#endif /* H2O_USE_NPN */
#if H2O_USE_ALPN
#ifdef DISABLE_HTTP2
	/* Disable HTTP/2 e. g. to test WebSockets. */
	static const h2o_iovec_t my_alpn_protocols[] = {
		{H2O_STRLIT("http/1.1")}, {NULL}
	};
	h2o_ssl_register_alpn_protocols(ssl_ctx, my_alpn_protocols);
#else /* DISABLE_HTTP2 */

	h2o_ssl_register_alpn_protocols(ssl_ctx, h2o_http2_alpn_protocols);
#endif /* DISABLE_HTTP2 */
#endif /* H2O_USE_ALPN */

	return ssl_ctx;

make_ssl_ctx_error:
	assert(*lerr != NULL);
	if (ssl_ctx != NULL)
		SSL_CTX_free(ssl_ctx);
	return NULL;
}
