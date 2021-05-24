#ifndef OPENSSL_UTILS_H
#define OPENSSL_UTILS_H

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

X509 *get_X509_from_certificate_path(const char *cert_path, const char **lerr);
const char *get_subject_common_name(X509 *cert);
SSL_CTX *make_ssl_ctx(const char *certificate_file, const char *key_file,
						int security_level, long min_proto_version, const char **lerr);

#endif /* OPENSSL_UTILS_H */