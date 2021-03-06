#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>

/* exit(), etc */
#include <stdlib.h>

/* getopt */
#include <unistd.h>

#include <syslog.h>

/* open() */
#include <sys/stat.h>
#include <fcntl.h>

/* writev (netconsole + kmsg) */
#include <sys/uio.h>

/* socket (netconsole) */
#include <sys/types.h>
#include <sys/socket.h>

/* waitpid() */
#include <sys/wait.h>

struct fbuf {
	size_t bytes_in_buf;
	uint8_t buf[4096];
};

static void *fbuf_space_ptr(struct fbuf *f)
{
	return f->buf + f->bytes_in_buf;
}

static size_t fbuf_space(struct fbuf *f)
{
	return sizeof(f->buf) - f->bytes_in_buf;
}

static void fbuf_feed(struct fbuf *f, size_t n)
{
	assert(fbuf_space(f) > n);
	f->bytes_in_buf += n;
}

static void *fbuf_data_ptr(struct fbuf *f)
{
	return f->buf;
}

static size_t fbuf_data(struct fbuf *f)
{
	return f->bytes_in_buf;
}

static void fbuf_eat(struct fbuf *f, size_t n)
{
	assert(n <= fbuf_data(f));
	memmove(f->buf, f->buf + n, f->bytes_in_buf - n);
	f->bytes_in_buf -= n;
}

static void fbuf_init(struct fbuf *f)
{
	/* NOTE: for perf, we do not zero the buffer */
	f->bytes_in_buf = 0;
}

#define STR_(n) #n
#define STR(n) STR_(n)

static
const char *opts = ":aehknsp:P";
#define PRGMNAME_DEFAULT "logto"

static
void usage_(const char *prgmname, int e)
{
	FILE *f;
	if (e != EXIT_SUCCESS)
		f = stderr;
	else
		f = stdout;

	fprintf(f,
"Usage: %s [options] -- <program> [<args>...]\n"
"\n"
"Options: [%s]\n"
" -k          send output to /dev/kmsg\n"
" -n          send output to netconsole (udp)\n"
" -s          send output to syslog (local)\n"
" -a          automatically select either stdout or kmsg based on\n"
"             avaliability of STDIO_FILENO\n"
" -e	      print exit status (only occurs if something else requires a fork)\n"
" -p <name>   include name in the redirected output\n"
" -P          as if `-p` was used with the last element of <program>\n"
" -h          show this help text\n",
	prgmname, opts);
	exit(e);
}
#define usage(e) usage_(prgmname, e)

struct emit_ctx {
	const char *name;
	char *prefix_buf;
	size_t prefix_buf_len;
	int output_fd;
	bool use_syslog;
};

static
void emit_line(struct emit_ctx *e, char *read_line, size_t read_line_len)
{
	/* emit data! */
	struct iovec o[2] = {
		{ e->prefix_buf, e->prefix_buf_len },
		{ read_line, read_line_len }
	};
	struct iovec *v = o + 1;
	size_t v_ct = 1;

	bool have_level = read_line_len >= 3 && read_line[0] == '<' && read_line[2] == '>';

	if (e->name) {
		v--;
		v_ct++;
		/* extract line log level, if present */
		if (have_level) {
			e->prefix_buf[1] = read_line[1];
			o[1].iov_base += 3;
			o[1].iov_len -= 3;
		} else {
			/* no level, use default? */
			e->prefix_buf[1] = LOG_INFO + '0';
			have_level = true;
		}
	}

	/* write data to syslog or netconsole */
	if (e->output_fd != -1) {
		/* TODO: consider using splice for fd type interconnects where possible */
		ssize_t r = writev(e->output_fd, v, v_ct);
		if (r < 0) {
			fprintf(stderr, "emit failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}

		/* FIXME: support better advanced netconsole formats */
	} else {
		/* non-fd things */
		if (e->use_syslog) {
			/* TODO: compose data into a single buffer */
			/* TODO: use syslog(3p) to emit */
			int prio = LOG_INFO;
			if (have_level) {
				prio  = *((char *)(v[0].iov_base)) - '0';
				v[0].iov_len -= 3;
				v[0].iov_base += 3;
			}

			char unified_buf[v[0].iov_len + (v_ct > 1) * v[1].iov_len + 1];
			char *pos = unified_buf;
			size_t i;
			for (i = 0; i < v_ct; i++) {
				memcpy(pos, v[i].iov_base, v[i].iov_len);
				pos += v[i].iov_len;
			}

			*pos = '\0';

			/* XXX: consider using "%s%s" to eliminate copying */
			syslog(prio, "%s", unified_buf);
		} else {
			/* XXX: complain */
			fprintf(stderr, "whoops, the programmer screwed up\n");
			exit(EXIT_FAILURE);
		}
	}
}

static bool fd_is_open(int fd)
{
	off_t r = lseek(fd, 0, SEEK_CUR);
	return r != -1 || errno != EBADF;
}

int main(int argc, char **argv)
{
	const char *prgmname = argc?argv[0]:PRGMNAME_DEFAULT;
	int opt, err = 0;
	char *name = NULL;

	bool use_kmsg = false, use_netconsole = false, use_syslog = false,
	     use_stdout = false;
	bool auto_name = false;
	bool show_exit_status = false;

	while ((opt = getopt(argc, argv, opts)) != -1) {
		switch (opt) {
		case 'h':
			usage(EXIT_SUCCESS);
			break;
		case 'a':
			if (fd_is_open(STDOUT_FILENO)) {
				use_stdout = true;
			} else {
				use_kmsg = true;
			}
			break;
		case 'e':
			show_exit_status = true;
			break;
		case 'k':
			use_kmsg = true;
			break;
		case 'n':
			use_netconsole = true;
			break;
		case 's':
			use_syslog = true;
			break;
		case 'p':
			free(name);
			name = strdup(optarg);
			break;
		case 'P':
			auto_name = true;
			break;
		case '?':
			err++;
			break;
		default:
			fprintf(stderr, "Error: programmer screwed up argument -%c\n", opt);
			err++;
			break;
		}
	}

	if (!use_netconsole && !use_kmsg && !use_syslog && !use_stdout) {
		fprintf(stderr, "Error: no destination selected, but one is required\n");
		err++;
	}

	if ((use_netconsole + use_kmsg + use_syslog + use_stdout) > 1) {
		fprintf(stderr, "Error: Sorry, right now we only support one destination at a time\n");
		err++;
	}

	if (name && auto_name) {
		fprintf(stderr, "Error: Use either -p or -P, not both\n");
		err++;
	}


	/* TODO: check that we don't have any extra file descriptors open */

	/*
	 * we're going to exec another program with special handling for
	 * STDIN_FILENO, STDOUT_FILENO, and STDERR_FILENO
	 */
	argc -= optind;
	argv += optind;

	if (!argv[0]) {
		fprintf(stderr, "Error: a program to execute is required\n");
		err++;
	}

	if (err)
		usage(EXIT_FAILURE);

	if (auto_name) {
		/* direct assignment (instead of strdup) is fine because we're
		 * never going to free(name) */
		name = strrchr(argv[0], '/');
		if (!name)
			name = argv[0];
	}

	size_t name_len = 0;
	if (name)
		name_len = strlen(name);

	/* Generate a buffer for emitting the line prefixes */
	char prefix_buf[3 /* "<N>" */ + name_len + 2 /* ": " */];
	prefix_buf[0] = '<';
	prefix_buf[1] = 'N';
	prefix_buf[2] = '>';
	if (name)
		memcpy(prefix_buf + 3, name, name_len);
	prefix_buf[3 + name_len + 0] = ':';
	prefix_buf[3 + name_len + 1] = ' ';

	/* NOTE: error checking throughout this is a very fiddly problem:
	 * because of the type of program this is, unless we're able to log
	 * messages over the medium we're trying to open, the output may never
	 * be seen. We'll try to log failures anyhow, but there might be room
	 * for improvement. */

	/* 0 = read, 1 = write */
	int new_stdout[] = {-1, -1};
	if (use_kmsg && !name) {
		new_stdout[1] = open("/dev/kmsg", O_RDWR);
		if (new_stdout[1] == -1) {
			fprintf(stderr, "could not open /dev/kmsg: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	} else if (!(use_stdout && !name)) {
		int r = pipe(new_stdout);
		if (r == -1) {
			fprintf(stderr, "could not setup pipe(): %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}


	/* if required, fork */
	bool should_exec = true;
	pid_t child = -1;
	if (new_stdout[0] != -1 && !(use_stdout && !name)) {
		child = fork();
		if (child == -1) {
			fprintf(stderr, "fork failed: %s\n", strerror(errno));
		}
		should_exec = child == 0;
	}

	if (should_exec) {
		/* dup new fds into place, close old ones */
		if (!(use_stdout && !name)) {
			dup2(new_stdout[1], STDOUT_FILENO);
			dup2(new_stdout[1], STDERR_FILENO);

			close(new_stdout[0]);
			close(new_stdout[1]);
		}

		int r = execvp(argv[0], argv);
		if (r == -1) {
			fprintf(stderr, "exec failed: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	close(new_stdout[1]);

	/* if we've gotten here, we need to open our output mechanism so we can write into it later */
	int output_fd = -1;
	if (use_kmsg) {
		output_fd = open("/dev/kmsg", O_WRONLY);
		if (output_fd == -1) {
			fprintf(stderr, "could not open /dev/kmsg: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	} else if (use_netconsole) {
		/* XXX: do this properly? */
		output_fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (output_fd == -1) {
			fprintf(stderr, "could not setup UDP socket for netconsole: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}
	} else if (use_stdout) {
		output_fd = STDOUT_FILENO;
	} else {
		/* XXX: whoops? */
	}

	struct fbuf buf;
	fbuf_init(&buf);

	/* NOTE: we also need to wait() for the child to die and exit (or
	 * preform another appropriate action) when it does.
	 */

	/* XXX: consider allowing control of prefixing the output (or otherwise formatting it) */
	/* XXX: consider allowing the printing of informational messages when
	 * the child is started and when it stops */

	struct emit_ctx e = {
		.name = name,
		.output_fd = output_fd,
		.prefix_buf = prefix_buf,
		.prefix_buf_len = sizeof(prefix_buf),
		.use_syslog = use_syslog,
	};

	/* TODO: if required, process data over pipes */
	for (;;) {
		uint8_t *space = fbuf_space_ptr(&buf);
		ssize_t r = read(new_stdout[0], space, fbuf_space(&buf));
		if (r == 0) {
			/* This indicates the other end of the pipe() closed.
			 * Either the program has exited, or for some reason it
			 * closed the fd. For now, assume this means a program
			 * exit. */

			/* flush data from buffer */
			if (fbuf_data(&buf)) {
				emit_line(&e, fbuf_data_ptr(&buf), fbuf_data(&buf));
			}

			/* reap child & return it's return */
			/* TODO: handle the case where the child hasn't died,
			 * but it has closed it's output. Also allow some
			 * timeout for it to die after closing it's output */
			int status = 0;
			int rw = waitpid(-1, &status, 0);
			if (rw < 0) {
				fprintf(stderr, "waitpid returned %d: %s\n", rw, strerror(errno));
				exit(EXIT_FAILURE);
			}

			if (show_exit_status) {
				/* FIXME: provide a printf for emit_ctx */
				/* TODO: use more of the exit status */
				char out[1024];
				snprintf(out, sizeof(out) - 1, "[logto: program %s exited with code %d]\n", name?name:argv[0], WEXITSTATUS(status));
				out[sizeof(out) - 1] = '\0';
				emit_line(&e, out, strlen(out));
			}

			exit(WEXITSTATUS(status));
		} else if (r < 0) {
			/* XXX: flush data from buffer */
			/* XXX: emit info to output stream */
			fprintf(stderr, "read returned %zd: %s\n", r, strerror(errno));
			exit(EXIT_FAILURE);
		}

		fbuf_feed(&buf, r);

		/* scan for newline in new data */
		size_t i;
		for (i = 0; i < (size_t)r; i++) {
			if (space[i] == '\n') {
				/* TODO: emit data up to this point */
				size_t l = fbuf_data(&buf);
				emit_line(&e, fbuf_data_ptr(&buf), l);
				fbuf_eat(&buf, l);
			}
		}

		if (fbuf_space(&buf)) {
			/* while we have space, don't force a flush */
			continue;
		}

		char *read_line = fbuf_data_ptr(&buf);
		size_t read_line_len = fbuf_data(&buf);
		emit_line(&e, read_line, read_line_len);
		fbuf_eat(&buf, read_line_len);
	}

	return 0;
}
