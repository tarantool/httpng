#include "httpng.c"

typedef sni_map_t servername_callback_arg_t;

#define STR_PORT_LENGTH 8

static int
ip_version(const char *src)
{
	char buf[sizeof(struct in6_addr)];
	if (inet_pton(AF_INET, src, buf))
		return AF_INET;
	if (inet_pton(AF_INET6, src, buf))
		return AF_INET6;
	return -1;
}

/** Returns file descriptor or -1 on error */
static int
open_listener(const char *addr_str, uint16_t port, const char **lerr)
{
	struct addrinfo hints, *res;
	char port_str[STR_PORT_LENGTH];
	snprintf(port_str, sizeof(port_str), "%d", port);

	memset(&hints, 0, sizeof(hints));

	int ai_family = ip_version(addr_str);
	if (ai_family < 0) {
		*lerr = "Can't parse IP address";
		goto ip_detection_fail;
	}

	hints.ai_family = ai_family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;

	int ret = getaddrinfo(addr_str, port_str, &hints, &res);
	if (ret || res->ai_family != ai_family) {
		*lerr = "getaddrinfo can't find appropriate ip and port";
		goto getaddrinfo_fail;
	}

	int flags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
	flags |= SOCK_CLOEXEC;
#endif /* SOCK_CLOEXEC */
	int fd;
	if ((fd = socket(res->ai_family, flags, 0)) == -1) {
		*lerr = "create socket failed";
		goto socket_create_fail;
	}
#ifndef SOCK_CLOEXEC
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		*lerr = "setting FD_CLOEXEC failed";
		goto fdcloexec_set_fail;
	}
#endif /* SOCK_CLOEXEC */

	int reuseaddr_flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag,
	    sizeof(reuseaddr_flag)) != 0) {
		*lerr = "setsockopt SO_REUSEADDR failed";
		goto so_reuseaddr_set_fail;
	}

	int ipv6_flag = 1;
	if (ai_family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_flag,
		sizeof(ipv6_flag)) != 0) {
		*lerr = "setsockopt IPV6_V6ONLY failed";
		goto ipv6_only_set_fail;
	}

	if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
		*lerr = "bind error";
		goto bind_fail;
	}

	if (listen(fd, SOMAXCONN) != 0) {
		*lerr = "listen error";
		goto listen_fail;
	}

#ifdef TCP_DEFER_ACCEPT
	{
		/* We are only interested in connections
		 * when actual data is received. */
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag,
		    sizeof(flag)) != 0)
			/* FIXME: report in log */
			fprintf(stderr, "setting TCP_DEFER_ACCEPT failed\n");
	}
#endif /* TCP_DEFER_ACCEPT */

	if (conf.tfo_queues > 0) {
		/* TCP extension to do not wait for SYN/ACK for "known"
		 * clients. */
#ifdef TCP_FASTOPEN
		int tfo_queues;
#ifdef __APPLE__
		/* In OS X, the option value for TCP_FASTOPEN must be 1
		 * if is's enabled. */
		tfo_queues = 1;
#else /* __APPLE__ */
		tfo_queues = conf.tfo_queues;
#endif /* __APPLE__ */
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN,
		    (const void *)&tfo_queues, sizeof(tfo_queues)) != 0)
			/* FIXME: report in log */
			fprintf(stderr, "setting TCP TFO feature failed\n");
#else /* TCP_FASTOPEN */
		assert(!".tfo_queues not zero on platform w/o TCP_FASTOPEN");
#endif /* TCP_FASTOPEN */
	}

	return fd;

listen_fail:
bind_fail:
ipv6_only_set_fail:
so_reuseaddr_set_fail:
#ifndef SOCK_CLOEXEC
fdcloexec_set_fail:
#endif /* SOCK_CLOEXEC */
	close(fd);
socket_create_fail:
getaddrinfo_fail:
ip_detection_fail:
	assert(*lerr != NULL);
	return -1;
}

static sni_map_t *
sni_map_create(int certs_num, const char **lerr)
{
	sni_map_t *sni_map = calloc(1, sizeof(*sni_map));
	if (sni_map == NULL) {
		*lerr = "sni map memory allocation failed";
		goto sni_map_alloc_fail;
	}

	sni_map->ssl_ctxs = calloc(certs_num, sizeof(*sni_map->ssl_ctxs));
	if (sni_map->ssl_ctxs == NULL) {
		*lerr = "memory allocation failed for ssl_ctxs in sni map";
		goto ssl_ctxs_alloc_fail;
	}
	sni_map->ssl_ctxs_capacity = certs_num;
	sni_map->sni_fields = calloc(certs_num, sizeof(*sni_map->sni_fields));
	if (sni_map->sni_fields == NULL) {
		*lerr = "memory allocation failed for sni_fields in sni map";
		goto sni_fields_alloc_fail;
	}
	sni_map->sni_fields_capacity = certs_num;
	return sni_map;

sni_fields_alloc_fail:
	free(sni_map->sni_fields);
ssl_ctxs_alloc_fail:
	free(sni_map->ssl_ctxs);
sni_map_alloc_fail:
	assert(*lerr != NULL);
	return NULL;
}

static int
sni_map_insert(sni_map_t *sni_map, const char *certificate_file,
	       const char *certificate_key_file, const char **lerr)
{
	if (sni_map == NULL) {
		*lerr = "pointer to sni map is NULL";
		goto error;
	}

	if (sni_map->ssl_ctxs_size >= sni_map->ssl_ctxs_capacity) {
		*lerr = "ssl_ctxs_size >= ssl_ctxs_capacity";
		goto error;
	}

	if (sni_map->sni_fields_size >= sni_map->sni_fields_capacity) {
		*lerr = "sni_fields_size >= sni_fields_capacity";
		goto error;
	}

	X509 *X509_cert = get_X509_from_certificate_path(certificate_file, lerr);
	if (X509_cert == NULL) {
		*lerr = "can't parse certificate file";
		goto error;
	}

	const char *common_name = get_subject_common_name(X509_cert);
	if (common_name == NULL) {
		*lerr = "can't get common name";
		goto x509_common_name_fail;
	}

	SSL_CTX *ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file,
		conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto make_ssl_ctx_fail;
	sni_map->ssl_ctxs[sni_map->ssl_ctxs_size++] = ssl_ctx;
	sni_map->sni_fields[sni_map->sni_fields_size].hostname.base =
		(char *)common_name;
	sni_map->sni_fields[sni_map->sni_fields_size].hostname.len =
		strlen(common_name);
	sni_map->sni_fields[sni_map->sni_fields_size++].ssl_ctx = ssl_ctx;

	X509_free(X509_cert);
	return 0;

make_ssl_ctx_fail:
	free((char *)common_name);
x509_common_name_fail:
	X509_free(X509_cert);
error:
	assert(*lerr != NULL);
	return -1;
}

static void
sni_map_deinit(sni_map_t *sni_map)
{
	if (sni_map == NULL)
		return;
	for (size_t i = 0; i < sni_map->ssl_ctxs_size; ++i)
		SSL_CTX_free(sni_map->ssl_ctxs[i]);
	for (size_t i = 0; i < sni_map->sni_fields_size; ++i)
		free((char *)sni_map->sni_fields[i].hostname.base);
	free(sni_map);
}

static void
conf_sni_map_cleanup(void)
{
	if (conf.sni_maps == NULL)
		return;
	for (size_t i = 0; i < conf.num_listeners; ++i)
		sni_map_deinit(conf.sni_maps[i]);
	free(conf.sni_maps);
}

#define GET_REQUIRED_LISTENER_FIELD(name, lua_ttype, convert_func_postfix) \
	do { \
		lua_getfield(L, -1, #name); \
		if (lua_isnil(L, -1)) { \
			*lerr = #name " is absent"; \
			lua_pop(L, 1); \
			goto required_field_fail; \
		} \
		if (lua_type(L, -1) != lua_ttype) { \
			*lerr = #name " isn't " #convert_func_postfix; \
			lua_pop(L, 1); \
			goto required_field_fail; \
		} \
		name = lua_to##convert_func_postfix(L, -1); \
		lua_pop(L, 1); \
	} while (0);

static SSL_CTX *
get_ssl_ctx_not_uses_sni(lua_State *L, unsigned listener_idx,
			 const char **lerr)
{
	SSL_CTX *ssl_ctx = NULL;

	unsigned certs_num = lua_objlen(L, -1);
	if (certs_num != 1) {
		*lerr = "certs num must be 1 when not using sni";
		goto certs_num_fail;
	}
	conf.sni_maps[listener_idx] = NULL;

	const char *certificate_file = NULL;
	const char *certificate_key_file = NULL;

	lua_rawgeti(L, -1, 1);
	if (!lua_istable(L, -1)) {
		*lerr = "element of `tls` table isn't a table";
		goto tls_pair_not_a_table;
	}
	GET_REQUIRED_LISTENER_FIELD(certificate_file, LUA_TSTRING, string);
	GET_REQUIRED_LISTENER_FIELD(certificate_key_file, LUA_TSTRING, string);
	lua_pop(L, 1);

	ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file,
		conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto ssl_ctx_create_fail;

	return ssl_ctx;

ssl_ctx_create_fail:
tls_pair_not_a_table:
required_field_fail:
certs_num_fail:
	assert(*lerr != NULL);
	return NULL;
}

static int
servername_callback(SSL *s, int *al, void *arg)
{
	assert(arg != NULL);
	sni_map_t *sni_map = (servername_callback_arg_t *)arg;

	const char *servername = SSL_get_servername(s,
		TLSEXT_NAMETYPE_host_name);
	if (servername == NULL) {
/* FIXME: report to log */
#ifndef NDEBUG
		fprintf(stderr, "Server name is not received:%s\n",
			servername);
#endif /* NDEBUG */
		/* FIXME: think maybe return SSL_TLSEXT_ERR_NOACK */
		goto get_servername_fail;
	}
	size_t servername_len = strlen(servername);
	
	/* FIXME: make hash table for sni_fields, not an array */
	size_t i;
	for (i = 0; i < sni_map->sni_fields_size; ++i) {
		if (servername_len == sni_map->sni_fields[i].hostname.len &&
		    memcmp(servername, sni_map->sni_fields[i].hostname.base,
		    servername_len) == 0) {
			SSL_CTX *ssl_ctx = sni_map->sni_fields[i].ssl_ctx;
			if (SSL_set_SSL_CTX(s, ssl_ctx) == NULL) {
				fprintf(stderr, "Error while switching SSL "
					"context after scanning TLS SNI\n");
				goto set_ssl_ctx_fail;
			}
		}
	}
	return SSL_TLSEXT_ERR_OK;

set_ssl_ctx_fail:
get_servername_fail:
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static SSL_CTX *
get_ssl_ctx_uses_sni(lua_State *L, unsigned listener_idx, const char **lerr)
{
	SSL_CTX *ssl_ctx = NULL;
	unsigned certs_num = lua_objlen(L, -1);
	sni_map_t *sni_map = NULL;
	if ((sni_map = sni_map_create(certs_num, lerr)) == NULL)
		goto sni_map_init_fail;

	lua_pushnil(L); /* Start of table  */
	while (lua_next(L, -2)) {
		const char *certificate_file = NULL;
		const char *certificate_key_file = NULL;

		GET_REQUIRED_LISTENER_FIELD(certificate_file,
			LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(certificate_key_file,
			LUA_TSTRING, string);

		if (sni_map_insert(sni_map, certificate_file,
		    certificate_key_file, lerr) != 0) {
			lua_pop(L, 1);
			goto sni_map_insert_fail;
		}
		lua_pop(L, 1);
	}

	ssl_ctx = make_ssl_ctx(NULL, NULL, conf.openssl_security_level,
		conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto ssl_ctx_create_fail;
	SSL_CTX_set_tlsext_servername_callback(ssl_ctx, servername_callback);
	SSL_CTX_set_tlsext_servername_arg(ssl_ctx,
		(servername_callback_arg_t *)sni_map);
	conf.sni_maps[listener_idx] = sni_map;
	return ssl_ctx;

ssl_ctx_create_fail:
required_field_fail:
sni_map_insert_fail:
	sni_map_deinit(sni_map);
sni_map_init_fail:
	assert(*lerr != NULL);
	return NULL;
}

static SSL_CTX *
get_tls_field_from_lua(lua_State *L, unsigned listener_idx,
		       bool uses_sni, const char **lerr)
{
	if (!lua_istable(L, -1)) {
		*lerr = "`tls` isn't a table";
		goto tls_not_a_table;
	}
	unsigned certs_num = lua_objlen(L, -1);
	if (!uses_sni && certs_num != 1) {
		*lerr = "certificates number and `uses_sni` contradict "
			"each other";
		goto wrong_cert_num_and_uses_sni;
	}
	return uses_sni ? get_ssl_ctx_uses_sni(L, listener_idx, lerr)
			: get_ssl_ctx_not_uses_sni(L, listener_idx, lerr);

wrong_cert_num_and_uses_sni:
tls_not_a_table:
	assert(*lerr != NULL);
	return NULL;
}

static int
multilisten_get_listen_from_lua(lua_State *L, int lua_stack_idx_table,
				const char **lerr)
{
	/* FIXME: move it to cfg? */
	SSL_library_init();
	SSL_load_error_strings();

	lua_getfield(L, lua_stack_idx_table, "listen");
	int need_to_pop = 1;
	if (lua_isnil(L, -1)) {
		*lerr = "listen is absent";
		goto listen_invalid_type;
	}

	if (!lua_istable(L, -1)) {
		*lerr = "listen isn't table";
		goto listen_invalid_type;
	}
	conf.num_listeners = lua_objlen(L, -1);

	if ((conf.listener_cfgs = calloc(conf.num_listeners,
	    sizeof(*conf.listener_cfgs))) == NULL) {
		*lerr = "allocation memory for listener_cfgs failed";
		goto listener_cfg_malloc_fail;
	}

	if ((conf.sni_maps = calloc(conf.num_listeners,
	    sizeof(*conf.sni_maps)))== NULL) {
		*lerr = "allocation memory for sni maps failed";
		goto sni_map_alloc_fail;
	}

	size_t listener_idx = 0;
	lua_pushnil(L); /* Start of table  */
	while (lua_next(L, -2)) {
		++need_to_pop;
		if (!lua_istable(L, -1)) {
			*lerr = "listeners are bad parsed";
			goto failed_parsing_clean_listen_conf;
		}

		const char *addr;
		uint16_t port;

		GET_REQUIRED_LISTENER_FIELD(addr, LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(port, LUA_TNUMBER, integer);

		SSL_CTX *ssl_ctx = NULL;
		lua_getfield(L, -1, "tls");
		++need_to_pop;
		if (lua_isnil(L, -1))
			conf.sni_maps[listener_idx] = NULL;
		else if (lua_istable(L, -1)) {
			lua_pop(L, 1);
			--need_to_pop;

			bool uses_sni;
			GET_REQUIRED_LISTENER_FIELD(uses_sni,
				LUA_TBOOLEAN, boolean);
			lua_getfield(L, -1, "tls");
			++need_to_pop;

			ssl_ctx = get_tls_field_from_lua(L, listener_idx,
				uses_sni, lerr);
			if ((ssl_ctx = get_tls_field_from_lua(L, listener_idx,
			    uses_sni, lerr)) == NULL)
				goto failed_parsing_clean_listen_conf;
		} else {
			*lerr = "`tls` isn't table or nil";
			goto failed_parsing_clean_listen_conf;
		}
		/* pop "tls" and "clean" listen cfg */
		lua_pop(L, 2);
		need_to_pop -= 2;

		int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto open_listener_fail;
		register_listener_cfgs_socket(fd, ssl_ctx, listener_idx);
		++listener_idx;
	}

	/* pop listen table */
	lua_pop(L, 1);
	assert(--need_to_pop == 0);
	return 0;

open_listener_fail:
required_field_fail:
failed_parsing_clean_listen_conf:
	conf_sni_map_cleanup();
	close_listener_cfgs_sockets();
	conf.sni_maps = NULL;
sni_map_alloc_fail:
	free(conf.listener_cfgs);
	conf.listener_cfgs = NULL;
listener_cfg_malloc_fail:
listen_invalid_type:
	/* pop values pushed while executing current function */
	lua_pop(L, need_to_pop);
	EVP_cleanup();
	ERR_free_strings();
	assert(*lerr != NULL);
	return 1;
}

#undef GET_REQUIRED_LISTENER_FIELD
