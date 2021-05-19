#include "httpng.c"

typedef sni_map_t servername_callback_arg_t;

#define STR_PORT_LENGTH 8

static int ip_version(const char *src) {
	char buf[16];
	if (inet_pton(AF_INET, src, buf)) {
		return AF_INET;
	} else if (inet_pton(AF_INET6, src, buf)) {
		return AF_INET6;
	}
	return -1;
}

/** Returns file descriptor or -1 on error */
static int open_listener(const char *addr_str, uint16_t port, const char **lerr)
{
	struct addrinfo hints, *res;
	char port_str[STR_PORT_LENGTH];
	snprintf(port_str, STR_PORT_LENGTH, "%d", port);
	int fd;

	memset(&hints, 0, sizeof(hints));

	int ai_family = ip_version(addr_str);
	if (ai_family == -1) {
		*lerr = "Can't detect IP address";
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
	if ((fd = socket(res->ai_family, flags, 0)) == -1) {
		*lerr = "create socket(2) failed";
		goto socket_create_fail;
	}
#ifndef SOCK_CLOEXEC
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		*lerr = "setting cloexec failed";
		goto close_fd;
	}
#endif /* SOCK_CLOEXEC */

	int reuseaddr_flag = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_flag,
		sizeof(reuseaddr_flag)) != 0) {
		*lerr = "setsockopt SO_REUSEADDR failed";
		goto close_fd;
	}

	int ipv6_flag = 1;
	if (ai_family == AF_INET6 &&
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_flag,
		sizeof(ipv6_flag)) != 0) {
		*lerr = "setsockopt IPV6_V6ONLY failed";
		goto close_fd;
	}

	if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
		perror("bind error");
		*lerr = "bind error";
		goto close_fd;
	}

	if (listen(fd, SOMAXCONN) != 0) {
		*lerr = "listen error";
		goto close_fd;
	}

#ifdef TCP_DEFER_ACCEPT
	{
		/* We are only interested in connections
		 * when actual data is received. */
		int flag = 1;
		if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag,
			sizeof(flag)) != 0) {
			*lerr = "setting TCP_DEFER_ACCEPT failed";
			goto close_fd;
		}
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
#else
		tfo_queues = conf.tfo_queues;
#endif /* __APPLE__ */
		if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN,
			(const void *)&tfo_queues, sizeof(tfo_queues)) != 0) {
			*lerr = "setting TCP TFO feature failed";
			goto close_fd;
		}
#else
		assert(!".tfo_queues not zero on platform w/o TCP_FASTOPEN");
#endif /* TCP_FASTOPEN */
	}

	return fd;

close_fd:
	close(fd);
socket_create_fail:
getaddrinfo_fail:
ip_detection_fail:
	assert(*lerr != NULL);
	return -1;
}

static sni_map_t *sni_map_create(int certs_num, const char **lerr) {
	sni_map_t *sni_map = calloc(1, sizeof(*sni_map));
	if (sni_map == NULL) {
		*lerr = "sni map memory allocation failed";
		goto sni_map_alloc_fail;
	}

	sni_map->sni_fields = 0;
	sni_map->sni_fields_size = 0;
	sni_map->ssl_ctxs_size = 0;
	sni_map->ssl_ctxs_capacity = 0;
	sni_map->ssl_ctxs = calloc(certs_num, sizeof(*sni_map->ssl_ctxs));
	if (sni_map->ssl_ctxs == NULL) {
		*lerr = "memory allocation failed for ssl_ctxs in sni map";
		goto ssl_ctxs_alloc_fail;
	}
	sni_map->ssl_ctxs_capacity = certs_num;
	sni_map->sni_fields = calloc(certs_num, sizeof(*sni_map->ssl_ctxs));
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

static int sni_map_insert(sni_map_t *sni_map, const char *certificate_file, const char *certificate_key_file, const char **lerr) {
	if (sni_map == NULL) {
		*lerr = "pointer to sni map is NULL";
		goto error;
	}

	if (sni_map->ssl_ctxs_capacity <= sni_map->ssl_ctxs_size) {
		*lerr = "sni_ctxs_capacity <= ssl_cts_size";
		goto error;
	}

	if (sni_map->sni_fields_capacity <= sni_map->sni_fields_size) {
		*lerr = "sni_fields_capacity <= sni_fields_size";
		goto error;
	}

	X509 *X509_cert = get_X509_from_certificate_path(certificate_file, lerr);
	if (X509_cert == NULL) {
		*lerr = "can't parse certificate file";
		goto error;
	}

	const char * common_name = get_subject_common_name(X509_cert);
	if (common_name == NULL) {
		*lerr = "can't get common name";
		goto x509_common_name_fail;
	}

	SSL_CTX *ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file, conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL)
		goto make_ssl_ctx_fail;
	sni_map->ssl_ctxs[sni_map->ssl_ctxs_size++] = ssl_ctx;
	sni_map->sni_fields[sni_map->sni_fields_size].hostname = common_name;
	sni_map->sni_fields[sni_map->sni_fields_size++].ssl_ctx = ssl_ctx;

	return 0;

make_ssl_ctx_fail:
	free((char *)common_name);
x509_common_name_fail:
	X509_free(X509_cert);
error:
	assert(*lerr != NULL);
	return -1;
}

static void sni_map_deinit(sni_map_t *sni_map) {
	if (sni_map == NULL)
		return;
	for (size_t i = 0; i < sni_map->ssl_ctxs_size; ++i)
		SSL_CTX_free(sni_map->ssl_ctxs[i]);
	for (size_t i = 0; i < sni_map->sni_fields_size; ++i)
		free((char *)sni_map->sni_fields[i].hostname);
	free(sni_map);
}

static void conf_sni_map_cleanup() {
	if (conf.sni_maps == NULL)
		return;
	for (size_t i = 0; i < conf.num_listeners; ++i)
		sni_map_deinit(conf.sni_maps[i]);
	free(conf.sni_maps);
}

#define GET_REQUIRED_LISTENER_FIELD(name, lua_ttype, convert_func_postfix) \
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

static SSL_CTX *get_ssl_ctx_not_uses_sni(lua_State *L, unsigned listener_idx, const char **lerr) {
	SSL_CTX *ssl_ctx = NULL;

#ifndef NDEBUG
	unsigned certs_num = lua_objlen(L, -1);
	assert(certs_num == 1);
#endif /* NDEBUG */
	conf.sni_maps[listener_idx] = NULL;

	const char *certificate_file = NULL;
	const char *certificate_key_file = NULL;

	lua_rawgeti(L, -1, 1);
	assert(lua_istable(L, -1));
	GET_REQUIRED_LISTENER_FIELD(certificate_file, LUA_TSTRING, string);
	GET_REQUIRED_LISTENER_FIELD(certificate_key_file, LUA_TSTRING, string);
	lua_pop(L, 1);

	ssl_ctx = make_ssl_ctx(certificate_file, certificate_key_file, conf.openssl_security_level, conf.min_tls_proto_version, lerr);
	if (ssl_ctx == NULL) {
		goto ssl_ctx_create_fail;
	}

	return ssl_ctx;

ssl_ctx_create_fail:
required_field_fail:
	assert(*lerr != NULL);
	return NULL;
}

static int servername_callback(SSL *s, int *al, void *arg) {
	assert(arg != NULL);
	sni_map_t *sni_map = (servername_callback_arg_t *) arg;

	const char *servername = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
	if (servername == NULL) {
		fprintf(stderr, "Server name is not recieved:%s\n", servername);
		/* FIXME: think maybe return SSL_TLSEXT_ERR_NOACK */
		goto get_servername_fail;
	}

	/* FIXME: make hash table for sni_fields, not an array */
	for (size_t i = 0; i < sni_map->sni_fields_size; ++i) {
		if (strcasecmp(servername, sni_map->sni_fields[i].hostname) == 0) {
			SSL_CTX *ssl_ctx = sni_map->sni_fields[i].ssl_ctx;
			if (SSL_set_SSL_CTX(s, ssl_ctx) != ssl_ctx) {
				fprintf(stderr, "Error while switching SSL context after scanning TLS SNI\n");
				goto set_ssl_ctx_fail;
			}
		}
	}
	return SSL_TLSEXT_ERR_OK;

set_ssl_ctx_fail:
get_servername_fail:
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static SSL_CTX *get_ssl_ctx_uses_sni(lua_State *L, unsigned listener_idx, const char **lerr) {
	SSL_CTX *ssl_ctx = NULL;
	unsigned certs_num = lua_objlen(L, -1);
	sni_map_t *sni_map = NULL;
	if ((sni_map = sni_map_create(certs_num, lerr)) == NULL) {
		goto sni_map_init_fail;
	}

	lua_pushnil(L); /* Start of table  */
	while (lua_next(L, -2)) {
		const char *certificate_file = NULL;
		const char *certificate_key_file = NULL;

		GET_REQUIRED_LISTENER_FIELD(certificate_file, LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(certificate_key_file, LUA_TSTRING, string);

		if (sni_map_insert(sni_map, certificate_file, certificate_key_file, lerr) != 0) {
			goto sni_map_insert_fail;
		}
		lua_pop(L, 1);
	}

	ssl_ctx = make_ssl_ctx(NULL, NULL, conf.openssl_security_level, conf.min_tls_proto_version ,lerr);
	if (ssl_ctx == NULL) {
		goto ssl_ctx_create_fail;
	}
	SSL_CTX_set_tlsext_servername_callback(ssl_ctx, servername_callback);
	SSL_CTX_set_tlsext_servername_arg(ssl_ctx, (servername_callback_arg_t *) sni_map);
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

static SSL_CTX *get_tls_field_from_lua(lua_State *L, unsigned listener_idx, bool uses_sni, const char **lerr) {
	assert(lua_istable(L, -1));
#ifndef NDEBUG
	unsigned certs_num = lua_objlen(L, -1);
	assert((!uses_sni && certs_num == 1) || uses_sni);
#endif /* NDEBUG */
	SSL_CTX *ssl_ctx = NULL;
	if (uses_sni)
		ssl_ctx = get_ssl_ctx_uses_sni(L, listener_idx, lerr);
	else
		ssl_ctx = get_ssl_ctx_not_uses_sni(L, listener_idx, lerr);
	return ssl_ctx;
}

int multilisten_get_listen_from_lua(lua_State *L, int LUA_STACK_IDX_TABLE, const char **lerr) {
	/* FIXME: move it to cfg? */
	SSL_library_init();
	SSL_load_error_strings();

	int need_to_pop = 0;
	lua_getfield(L, LUA_STACK_IDX_TABLE, "listen");
	++need_to_pop;
	if (lua_isnil(L, -1)) {
		*lerr = "listen is absent";
		goto listen_invalid_type;
	}

	if (lua_istable(L, -1)) {
		conf.num_listeners = lua_objlen(L, -1);
	} else {
		*lerr = "listen isn't table";
		goto listen_invalid_type;
	}

	if ((conf.listener_cfgs = calloc(conf.num_listeners, sizeof(*conf.listener_cfgs))) == NULL) {
		*lerr = "allocation memory for listener_cfgs failed";
		goto listener_cfg_malloc_fail;
	}

	if ((conf.sni_maps = calloc(conf.num_listeners, sizeof(*conf.sni_maps))) == NULL) {
		*lerr = "allocation memory for sni maps failed";
		goto sni_map_alloc_fail;
	}

	size_t listener_idx = 0;
	lua_pushnil(L); /* Start of table  */
	while (lua_next(L, -2)) {
		++need_to_pop;
		if (!lua_istable(L, -1)) {
			*lerr = "clear <listen-conf> isn't a table";
			goto clear_listen_conf_fail;
		}

		const char  *addr;
		uint16_t    port;

		GET_REQUIRED_LISTENER_FIELD(addr, LUA_TSTRING, string);
		GET_REQUIRED_LISTENER_FIELD(port, LUA_TNUMBER, integer);

		SSL_CTX *ssl_ctx = NULL;
		lua_getfield(L, -1, "tls");
		++need_to_pop;
		if (lua_isnil(L, -1)) {
			conf.sni_maps[listener_idx] = NULL;
		} else if (lua_istable(L, -1)) {
			lua_pop(L, 1);
			--need_to_pop;

			bool uses_sni;
			GET_REQUIRED_LISTENER_FIELD(uses_sni, LUA_TBOOLEAN, boolean);
			lua_getfield(L, -1, "tls");
			++need_to_pop;

			ssl_ctx = get_tls_field_from_lua(L, listener_idx, uses_sni, lerr);
			if (ssl_ctx == NULL) {
				goto clear_listen_conf_fail;
			}
		} else {
			*lerr = "tls isn't table or nil";
			goto clear_listen_conf_fail;
		}
		lua_pop(L, 1); /* pop tls */
		--need_to_pop;

		int fd = open_listener(addr, port, lerr);
		if (fd < 0)
			goto open_listener_fail;
		register_listener_cfgs_socket(fd, ssl_ctx, listener_idx);

		lua_pop(L, 1); /* pop "clear" listen cfg */
		--need_to_pop;
		// printf("listener_cfg[%zu] configured\nnum_listeners = %u\n", listener_idx, conf.num_listeners);
		++listener_idx;
	}

	lua_pop(L, 1); /* pop listen table */
	assert(--need_to_pop == 0);
	return 0;

open_listener_fail:
required_field_fail:
clear_listen_conf_fail:
	conf_sni_map_cleanup();
	close_listener_cfgs_sockets();
	conf.sni_maps = NULL;
sni_map_alloc_fail:
	free(conf.listener_cfgs);
	conf.listener_cfgs = NULL;
listener_cfg_malloc_fail:
listen_invalid_type:
	lua_pop(L, need_to_pop); /* pop values pushed while executing current function */
	EVP_cleanup();
	ERR_free_strings();
	assert(*lerr != NULL);
	return 1;
}

#undef GET_REQUIRED_LISTENER_FIELD