#include <spawn.h>
#include <stdio.h>
#include <string.h>
extern char **environ;

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	char long_str[4096];
	size_t total_len = 0;
	int i;
	if (argc < 2)
		return 2;
	for (i = 1; i < argc; ++i) {
		const size_t len = strlen(argv[i]);
		if (total_len + len + 1>= sizeof(long_str))
			return 1;
		memcpy(&long_str[total_len], argv[i], len);
		total_len += len;
		long_str[total_len] = ' ';
		++total_len;
	}
	if (total_len + 1 >= sizeof(long_str))
		return 1;
	long_str[total_len] = 0;
	char *const *args = &argv[1];
	posix_spawn_file_actions_t file_actions;
	posix_spawn_file_actions_init(&file_actions);

	pid_t pid;
	const int res = posix_spawnp(&pid, argv[1], &file_actions,
		/* attrp */ NULL, args, environ);
	FILE *const pidfile = fopen("tmp_pid.txt", "w");
	if (pidfile != NULL)
		fprintf(pidfile, "%u", (unsigned)pid);
	return res;
}
