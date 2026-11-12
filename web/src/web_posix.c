/**
 * Raven Web - web_posix.c
 * Linux and the other Unix systems: the browser process and its two pipes,
 * the reading thread, folders.
 *
 * The "pipes" are socket pairs: a write to a browser that died then fails
 * instead of killing the application with SIGPIPE, and a read that waits in
 * another thread can be woken up (shutdown).
 */

#if !defined(_WIN32)

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "web_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

struct rw_process_t {
	pid_t pid;
	/** rw_process_exit_code collected it already: how it ended. */
	int reaped, status;
	/** Our end of the pipe the child reads, and of the pipe it writes. */
	int to_child;
	int from_child;
};

rw_process_t* rw_process_start(const char* const* arguments) {
	rw_process_t* process;
	int down[2], up[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, down) != 0) return NULL;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, up) != 0) {
		close(down[0]);
		close(down[1]);
		return NULL;
	}
	pid = fork();
	if (pid < 0) {
		close(down[0]);
		close(down[1]);
		close(up[0]);
		close(up[1]);
		return NULL;
	}
	if (pid == 0) {
		/* the child: its ends become descriptors 3 and 4, everything else is closed */
		int input = fcntl(down[1], F_DUPFD, 10), output = fcntl(up[1], F_DUPFD, 10);
		int null, fd, limit = (int)sysconf(_SC_OPEN_MAX);
		if (input < 0 || output < 0) _exit(127);
		if (dup2(input, 3) < 0 || dup2(output, 4) < 0) _exit(127);
		if (limit < 0 || limit > 65536) limit = 65536;
		for (fd = 5; fd < limit; fd++) close(fd);
		/* a browser talks a lot on its error output: only shown when asked */
		if (!getenv("RAVEN_WEB_DEBUG")) {
			null = open("/dev/null", O_RDWR);
			if (null >= 0) {
				dup2(null, 0);
				dup2(null, 1);
				dup2(null, 2);
				if (null > 4) close(null);
			}
		}
		/* in its own group: a Ctrl+C in the terminal of the application is for the application */
		setpgid(0, 0);
		execv(arguments[0], (char* const*)arguments);
		_exit(127);
	}
	close(down[1]);
	close(up[1]);
	process = (rw_process_t*)rw_alloc(sizeof(rw_process_t));
	if (!process) {
		close(down[0]);
		close(up[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return NULL;
	}
	process->pid = pid;
	process->reaped = process->status = 0;
	process->to_child = down[0];
	process->from_child = up[0];
	/* (not passed on to the programs the application may start later) */
	fcntl(process->to_child, F_SETFD, FD_CLOEXEC);
	fcntl(process->from_child, F_SETFD, FD_CLOEXEC);
	return process;
}

int rw_process_write(rw_process_t* process, const void* data, size_t size) {
	const char* cursor = (const char*)data;
	while (size > 0) {
		ssize_t sent = send(process->to_child, cursor, size, MSG_NOSIGNAL);
		if (sent < 0) {
			if (errno == EINTR) continue;
			return 0;
		}
		cursor += sent;
		size -= (size_t)sent;
	}
	return 1;
}

int rw_process_read(rw_process_t* process, void* buffer, size_t size) {
	for (;;) {
		ssize_t got = recv(process->from_child, buffer, size, 0);
		if (got < 0 && errno == EINTR) continue;
		return got < 0 ? 0 : (int)got;
	}
}

void rw_process_hang_up(rw_process_t* process) {
	shutdown(process->to_child, SHUT_RDWR);
	shutdown(process->from_child, SHUT_RDWR);
}

int rw_process_exit_code(rw_process_t* process, int wait_ms, long* code) {
	int i;
	for (i = 0; !process->reaped; i++) {
		struct timespec pause = { 0, 10 * 1000 * 1000 };
		if (waitpid(process->pid, &process->status, WNOHANG) == process->pid) process->reaped = 1;
		else if (i * 10 >= wait_ms) return 0;
		else nanosleep(&pause, NULL);
	}
	if (WIFEXITED(process->status)) *code = WEXITSTATUS(process->status);
	else if (WIFSIGNALED(process->status)) *code = -(long)WTERMSIG(process->status);
	else *code = 0;
	return 1;
}

void rw_process_stop(rw_process_t* process) {
	int i, status = 0, gone = process->reaped;
	rw_process_hang_up(process);
	/* a browser leaves by itself when its pipe closes: it is given two seconds */
	for (i = 0; i < 200 && !gone; i++) {
		struct timespec pause = { 0, 10 * 1000 * 1000 };
		if (waitpid(process->pid, &status, WNOHANG) != 0) gone = 1;
		else nanosleep(&pause, NULL);
	}
	if (!gone) {
		/* its group: the helpers it started go with it */
		kill(-process->pid, SIGKILL);
		kill(process->pid, SIGKILL);
		waitpid(process->pid, &status, 0);
	}
	close(process->to_child);
	close(process->from_child);
	rw_free(process);
}

/* ---------------------------------------------------------------------------
 *  Thread and lock
 * ------------------------------------------------------------------------- */

struct rw_thread_t {
	pthread_t handle;
	void (*function)(void*);
	void* argument;
};

static void* rw_thread_main(void* argument) {
	rw_thread_t* thread = (rw_thread_t*)argument;
	thread->function(thread->argument);
	return NULL;
}

rw_thread_t* rw_thread_start(void (*function)(void*), void* argument) {
	rw_thread_t* thread = (rw_thread_t*)rw_alloc(sizeof(rw_thread_t));
	if (!thread) return NULL;
	thread->function = function;
	thread->argument = argument;
	if (pthread_create(&thread->handle, NULL, rw_thread_main, thread) != 0) {
		rw_free(thread);
		return NULL;
	}
	return thread;
}

void rw_thread_join(rw_thread_t* thread) {
	if (!thread) return;
	pthread_join(thread->handle, NULL);
	rw_free(thread);
}

struct rw_mutex_t {
	pthread_mutex_t handle;
};

rw_mutex_t* rw_mutex_create(void) {
	rw_mutex_t* mutex = (rw_mutex_t*)rw_alloc(sizeof(rw_mutex_t));
	if (mutex) pthread_mutex_init(&mutex->handle, NULL);
	return mutex;
}

void rw_mutex_destroy(rw_mutex_t* mutex) {
	if (!mutex) return;
	pthread_mutex_destroy(&mutex->handle);
	rw_free(mutex);
}

void rw_mutex_lock(rw_mutex_t* mutex) {
	pthread_mutex_lock(&mutex->handle);
}

void rw_mutex_unlock(rw_mutex_t* mutex) {
	pthread_mutex_unlock(&mutex->handle);
}

/* ---------------------------------------------------------------------------
 *  Files
 * ------------------------------------------------------------------------- */

int rw_is_program(const char* path) {
	struct stat info;
	return path && path[0] && stat(path, &info) == 0 && S_ISREG(info.st_mode) && access(path, X_OK) == 0;
}

char* rw_find_browser(void) {
	static const char* const names[] = {
		"chromium", "chromium-browser", "google-chrome", "google-chrome-stable", "microsoft-edge",
		"microsoft-edge-stable", "brave-browser", "vivaldi", "chrome"
	};
	static const char* const places[] = {
		"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
		"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
		"/Applications/Chromium.app/Contents/MacOS/Chromium",
		"/snap/bin/chromium"
	};
	const char* search = getenv("PATH");
	char candidate[PATH_MAX];
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		const char* cursor = search;
		while (cursor && *cursor) {
			const char* end = strchr(cursor, ':');
			size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
			if (length > 0 && length + strlen(names[i]) + 2 < sizeof(candidate)) {
				snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, cursor, names[i]);
				if (rw_is_program(candidate)) return rw_strdup(candidate);
			}
			cursor = end ? end + 1 : NULL;
		}
	}
	for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
		if (rw_is_program(places[i])) return rw_strdup(places[i]);
	}
	return NULL;
}

char* rw_temp_folder(void) {
	const char* base = getenv("TMPDIR");
	char path[PATH_MAX];
	if (!base || !base[0]) base = "/tmp";
	snprintf(path, sizeof(path), "%s/raven-web-XXXXXX", base);
	return mkdtemp(path) ? rw_strdup(path) : NULL;
}

void rw_remove_folder(const char* path) {
	DIR* folder;
	struct dirent* entry;
	if (!path || !path[0]) return;
	folder = opendir(path);
	if (folder) {
		while ((entry = readdir(folder)) != NULL) {
			char child[PATH_MAX];
			struct stat info;
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
			if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= sizeof(child)) continue;
			/* (links are removed, never followed) */
			if (lstat(child, &info) == 0 && S_ISDIR(info.st_mode)) rw_remove_folder(child);
			else unlink(child);
		}
		closedir(folder);
	}
	rmdir(path);
}

char* rw_file_url(const char* path) {
	char absolute[PATH_MAX];
	rw_text_t url;
	const unsigned char* cursor;
	if (!path) return NULL;
	if (!realpath(path, absolute)) {
		/* a file that is not there: the browser will say so */
		if (path[0] == '/') {
			snprintf(absolute, sizeof(absolute), "%s", path);
		} else {
			char folder[PATH_MAX];
			if (!getcwd(folder, sizeof(folder))) folder[0] = 0;
			if ((size_t)snprintf(absolute, sizeof(absolute), "%s/%s", folder, path) >= sizeof(absolute)) return NULL;
		}
	}
	memset(&url, 0, sizeof(url));
	rw_text_puts(&url, "file://");
	for (cursor = (const unsigned char*)absolute; *cursor; cursor++) {
		unsigned char c = *cursor;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("/-_.~", c)) rw_text_add(&url, (const char*)&c, 1);
		else rw_text_printf(&url, "%%%02X", (unsigned)c);
	}
	return url.data;
}

int rw_needs_no_sandbox(void) {
	return geteuid() == 0;
}


/*
 * Runs a program and returns what it writes, bounded in time and in size:
 * the program is killed when it takes longer than `timeout_ms`. NULL when it
 * could not run, did not succeed, or wrote nothing.
 */
static uint8_t* rw_run_capture(const char* const* arguments, size_t limit, int timeout_ms, size_t* size) {
	int fds[2];
	pid_t pid;
	uint8_t* data = NULL;
	size_t used = 0, capacity = 0;
	struct timespec start, now;
	int status = 0, ok = 1;

	*size = 0;
	if (pipe(fds) != 0) return NULL;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 2);
		}
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		execvp(arguments[0], (char* const*)arguments);
		_exit(127);
	}
	close(fds[1]);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct pollfd waiting;
		long elapsed;
		int left;
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (long)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
		left = timeout_ms - (int)elapsed;
		if (left <= 0) {
			ok = 0;
			break;
		}
		waiting.fd = fds[0];
		waiting.events = POLLIN;
		waiting.revents = 0;
		if (poll(&waiting, 1, left) < 0) {
			if (errno == EINTR) continue;
			ok = 0;
			break;
		}
		if (waiting.revents & (POLLIN | POLLHUP)) {
			uint8_t chunk[16384];
			ssize_t count = read(fds[0], chunk, sizeof(chunk));
			if (count < 0) {
				if (errno == EINTR || errno == EAGAIN) continue;
				ok = 0;
				break;
			}
			if (count == 0) break;
			if (used + (size_t)count > limit) {
				ok = 0;
				break;
			}
			if (used + (size_t)count + 1 > capacity) {
				capacity = (used + (size_t)count + 1) * 2;
				data = (uint8_t*)rw_realloc(data, capacity);
				if (!data) {
					ok = 0;
					break;
				}
			}
			memcpy(data + used, chunk, (size_t)count);
			used += (size_t)count;
		}
	}
	close(fds[0]);
	if (!ok) kill(pid, SIGKILL);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || used == 0) {
		rw_free(data);
		return NULL;
	}
	data[used] = 0;
	*size = used;
	return data;
}

uint8_t* rw_clipboard_image(size_t* size, char* mime, size_t mime_size) {
	static const char* const kinds[] = { "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp" };
	size_t listed = 0, i;
	const char* list_command[] = { "wl-paste", "--list-types", NULL };
	uint8_t* types;
	uint8_t* data = NULL;
	*size = 0;
	if (!getenv("WAYLAND_DISPLAY")) return NULL;
	types = rw_run_capture(list_command, 4096, 700, &listed);
	if (!types) return NULL;
	for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		const char* at = strstr((const char*)types, kinds[i]);
		size_t length = strlen(kinds[i]);
		/* (a whole line of the list: "image/png", not "image/png-x") */
		if (at && (at == (const char*)types || at[-1] == '\n') && (at[length] == '\n' || at[length] == 0)) {
			const char* command[] = { "wl-paste", "--no-newline", "--type", kinds[i], NULL };
			data = rw_run_capture(command, 64u * 1024u * 1024u, 5000, size);
			if (data) {
				snprintf(mime, mime_size, "%s", kinds[i]);
				break;
			}
		}
	}
	rw_free(types);
	return data;
}

#else
typedef int rw_posix_unused_t;
#endif /* !_WIN32 */
