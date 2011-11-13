/* vim: set ts=3 sts=3 sw=3 noet ai:
 *
 * brainfunc: a really bad brainfuck interpreter
 *
 * brainfunc compiles brainfuck code to bytecode and then interprets it.
 * It also extends the language to provide 'functions'.
 * Any functions must appear after the main body of the program
 * (nothing else makes sense syntactically, as you'll see...)
 *
 * A function starts with an identifier immediately followed by a colon
 * (no space or other characters are allowed between the identifier and
 * the colon). A function ends when you define another function, or when
 * the source code ends.
 *
 * An identifier not followed by a colon is a function call. Calls to a
 * function may appear before the function itself: function resolution
 * happens at the end of compilation.
 *
 * An identifier is matched by the expression ([a-zA-Z_][a-zA-Z0-9_]*)
 * The '#' character is used as a line comment marker, causing all
 * further characters on that line to be ignored.
 *
 * By default, program source code size is limited to 1MB, and programs
 * run with a 1MB tape. To allow movement left, programs start half way
 * along the tape.
 *
 * The size limits can be changed by altering the #defines at the top
 * of this file.
 *
 * The need for function resolution means that all source code must be
 * available before the program begins execution (sorry about that).
 *
 * This file is Copyright (C) 2011, John Bartholomew.
 *
 * ---- LICENSE ----
 *
 * It is released under the Do What The Fuck You Want To Public License
 *
 *             DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE 
 *                     Version 2, December 2004 
 * 
 *  Copyright (C) 2004 Sam Hocevar <sam@hocevar.net> 
 * 
 *  Everyone is permitted to copy and distribute verbatim or modified 
 *  copies of this license document, and changing it is allowed as long 
 *  as the name is changed. 
 * 
 *             DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE 
 *    TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION 
 * 
 *   0. You just DO WHAT THE FUCK YOU WANT TO.
 *
 * ---- WARRANTY ----
 *
 *   This program is free software. It comes without any warranty, to
 *   the extent permitted by applicable law. You can redistribute it
 *   and/or modify it under the terms of the Do What The Fuck You Want
 *   To Public License, Version 2, as listed above.
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <assert.h>

#define STATE_SIZE (1<<20) /* 1 MB */
#define RECURSION_LIMIT 256
#define MIN_CBUF_CAPACITY 128
#define MIN_IDTABLE_CAPACITY 8
#define MAX_SOURCE_SIZE (1<<20) /* 1 MB */

int readstream(FILE *fl, char **out_buf, size_t *out_length, size_t *out_capacity) {
	size_t len, buflen, rdlen, remain;
	char *buf;

	assert(fl);
	assert(out_buf);

	buflen = 1024;
	len = 0;
	buf = malloc(buflen);
	if (! buf) {
		fclose(fl);
		fprintf(stderr, "could not allocate memory to read source\n");
		return -1;
	}
	while (len < MAX_SOURCE_SIZE) {
		remain = buflen-len-1; /* -1 for the null terminator */
		rdlen = fread(&buf[len], 1, remain, fl);
		if (rdlen < remain) {
			len += rdlen;
			if (feof(fl)) {
				buf[len] = '\0';
				break;
			}
			if (ferror(fl)) {
				free(buf);
				fprintf(stderr, "error while reading source file\n");
				return -1;
			}
		} else {
			char *newbuf;
			buflen *= 2;
			newbuf = realloc(buf, buflen);
			if (! newbuf) {
				free(buf);
				fprintf(stderr, "could not allocate memory to read source\n");
				return -1;
			} else
				buf = newbuf;
		}
	}
	if (len > MAX_SOURCE_SIZE) {
		free(buf);
		fprintf(stderr, "source is too long\n");
		return -1;
	}
	*out_buf = buf;
	if (out_length) *out_length = len;
	if (out_capacity) *out_capacity = buflen;
	return 0;
}

int readfile(const char *path, char **out_buf, size_t *out_length, size_t *out_capacity) {
	FILE *fl;
	char *buf;
	long len;
	size_t rdlen;

	assert(path);
	assert(out_buf);

	fl = fopen(path, "rb");
	if (! fl) {
		fprintf(stderr, "could not open file '%s'\n", path);
		return -1;
	}
	if (fseek(fl, 0, SEEK_END) < 0) {
		fclose(fl);
		fprintf(stderr, "could not seek to end of file '%s'\n", path);
		return -1;
	}
	len = ftell(fl);
	if (len == (long)(-1)) {
		fclose(fl);
		fprintf(stderr, "could not find size of file '%s'\n", path);
		return -1;
	}
	if (len > MAX_SOURCE_SIZE) {
		fclose(fl);
		fprintf(stderr, "source is too long\n");
		return -1;
	}
	if (fseek(fl, 0, SEEK_SET) < 0) {
		fclose(fl);
		fprintf(stderr, "could not seek back to beginning of file '%s'\n", path);
		return -1;
	}
	buf = malloc(len+1);
	if (! buf) {
		fclose(fl);
		fprintf(stderr, "could not allocate memory to read file '%s'\n", path);
		return -1;
	}
	rdlen = fread(buf, 1, len, fl);
	if (rdlen < (size_t)len) {
		free(buf);
		fclose(fl);
		fprintf(stderr, "could not read file contents '%s'\n", path);
		return -1;
	}
	buf[len] = '\0';
	*out_buf = buf;
	if (out_length) *out_length = len;
	if (out_capacity) *out_capacity = len+1;
	return 0;
}

enum Command {
	CMD_RET = 0,

	CMD_IN,
	CMD_OUT,
	CMD_LEFT,
	CMD_RIGHT,
	CMD_INC,
	CMD_DEC,
	CMD_REPNZ,

	CMD_CALL,

	CMD_COUNT
};

const char *CMD_NAMES[] = {
	"ret", "in", "out", "left", "right", "inc", "dec", "repnz", "call", 0
};

struct cmd {
	int cmd;
	int value;
};

struct idtablerec {
	size_t hash;
	const char *start;
	const char *end;

	size_t value;
};

struct idtable {
	struct idtablerec *recs;
	size_t len;
	size_t capacity;
};

/* DJB hash function -- super simple */
size_t strhash(const char *c, const char *end) {
	size_t h = 5381ul;
	while (c != end) {
		size_t cc = *c++;
		h += h*33 ^ cc;
	}
	return h;
}

int idtable_getid(struct idtable *ids, const char *start, const char *end) {
	size_t l1, hash;
	struct idtablerec *rec;
	assert(ids && start);

	l1 = end - start;
	hash = strhash(start, end);

	if (ids->len) {
		size_t i;
		/* horrible linear search */
		for (i = 0; i < ids->len; ++i) {
			rec = &(ids->recs[i]);
			if (rec->hash == hash) {
				size_t l2 = rec->end - rec->start;
				if ((l1 == l2) && (memcmp(start, rec->start, l1) == 0))
					return i;
			}
		}
	}

	if (ids->capacity == ids->len) {
		struct idtablerec *newrecs;
		size_t newcap;
		if (ids->capacity)
			newcap = ids->capacity * 2;
		else
			newcap = MIN_IDTABLE_CAPACITY;
		newrecs = realloc(ids->recs, sizeof(ids->recs[0])*newcap);
		if (! newrecs)
			return -1;
		memset(newrecs + ids->capacity, 0, sizeof(ids->recs[0])*(newcap - ids->capacity));
		ids->recs = newrecs;
		ids->capacity = newcap;
	}

	rec = &ids->recs[ids->len++];
	rec->hash = hash;
	rec->start = start;
	rec->end = end;
	return ids->len - 1;
}

void idtable_clear(struct idtable *ids) {
	free(ids->recs);
	ids->recs = 0;
	ids->len = ids->capacity = 0;
}

struct cmdbuf {
	struct cmd *commands;
	size_t len;
	size_t capacity;
};

void print_cmd(FILE *fl, const struct cmd *cmd) {
	assert(fl && cmd && cmd->cmd >= 0 && cmd->cmd < CMD_COUNT);
	fprintf(fl, "%5s %d", CMD_NAMES[cmd->cmd], cmd->value);
}

int cmdbuf_reserve(struct cmdbuf *cbuf, size_t capacity) {
	assert(cbuf);
	if (cbuf->capacity < capacity) {
		struct cmd *newcmds;
		newcmds = realloc(cbuf->commands, sizeof(cbuf->commands[0])*capacity);
		if (! newcmds)
			return -1;
		memset(newcmds + cbuf->capacity, 0, sizeof(cbuf->commands[0])*(capacity - cbuf->capacity));
		cbuf->commands = newcmds;
		cbuf->capacity = capacity;
	}
	return 0;
}

int cmdbuf_appendcmd(struct cmdbuf *cbuf, int cmd, int value) {
	struct cmd *c;
	assert(cbuf);
	if (cbuf->len == cbuf->capacity) {
		size_t cap;
		if (cbuf->capacity)
			cap = cbuf->capacity * 2;
		else
			cap = MIN_CBUF_CAPACITY;
		if (cmdbuf_reserve(cbuf, cap) < 0)
			return -1;
	}
	c = &cbuf->commands[cbuf->len++];
	c->cmd = cmd;
	c->value = value;
	return 0;
}

int cmdbuf_appendcmds(struct cmdbuf *cbuf, const struct cmd *cmds, size_t len) {
	struct cmd *c;
	size_t i;
	assert(cbuf && (cmds || !len));
	if (cmdbuf_reserve(cbuf, cbuf->len + len) < 0)
		return -1;
	c = &cbuf->commands[cbuf->len];
	for (i = 0; i < len; ++i)
		*c++ = cmds[i];
	cbuf->len += len;
	return 0;
}

int cmdbuf_appendcmdbuf(struct cmdbuf *cbuf, const struct cmdbuf *cmds) {
	return cmdbuf_appendcmds(cbuf, cmds->commands, cmds->len);
}

void cmdbuf_clear(struct cmdbuf *cbuf) {
	free(cbuf->commands);
	cbuf->commands = 0;
	cbuf->len = cbuf->capacity = 0;
}

enum blocktype {
	BT_MAIN,
	BT_FUNC,
	BT_LOOP
};

const char *compileblock(const char * const buf, const char * const end, struct idtable *ids, struct cmdbuf *cbuf, int blocktype) {
	const char *c;
	char cc;
	int count;
	assert((end >= buf) && ((end - buf) < INT_MAX));

	c = buf;
	while (c != end) {
		if (isalpha(*c) || (*c == '_')) {
			int id = 0;
			const char *start = c;
			while (c != end && (isalnum(*c) || (*c == '_'))) ++c;
			id = idtable_getid(ids, start, c);
			if (c != end && *c == ':') {
				struct cmdbuf blockbuf = {0,0,0};

				if (blocktype == BT_LOOP) {
					fprintf(stderr, "function defined inside loop\n");
					return 0;
				} else if (blocktype == BT_MAIN) {
					++c;
					c = compileblock(c, end, ids, &blockbuf, BT_FUNC);
					if (!c) {
						cmdbuf_clear(&blockbuf);
						return 0;
					}

					/* putting RET on cbuf to end the previous function or the main block */
					if (cmdbuf_appendcmd(cbuf, CMD_RET, 0) < 0) {
						cmdbuf_clear(&blockbuf);
						return 0;
					}

					ids->recs[id].value = cbuf->len;
					if (cmdbuf_appendcmdbuf(cbuf, &blockbuf) < 0) {
						cmdbuf_clear(&blockbuf);
						return 0;
					}
				} else {
					assert(blocktype == BT_FUNC);
					return start;
				}
			} else {
				if (cmdbuf_appendcmd(cbuf, CMD_CALL, id) < 0) {
					return 0;
				}
			}
		} else {
			switch (*c) {
				case '#': /* line comment */
					while (c != end && *c != '\n' && *c != '\r') ++c;
					if (c != end && *c == '\n') ++c;
					break;
				case '.': case ',': case '+': case '-': case '<': case '>':
					cc = *c;
					for (count = 0; c != end && *c == cc; ++c) ++count;
					switch (cc) {
						case '.': cmdbuf_appendcmd(cbuf, CMD_OUT  , count); break;
						case ',': cmdbuf_appendcmd(cbuf, CMD_IN   , count); break;
						case '+': cmdbuf_appendcmd(cbuf, CMD_INC  , count); break;
						case '-': cmdbuf_appendcmd(cbuf, CMD_DEC  , count); break;
						case '<': cmdbuf_appendcmd(cbuf, CMD_LEFT , count); break;
						case '>': cmdbuf_appendcmd(cbuf, CMD_RIGHT, count); break;
					}
					break;
				case '[':
					{
						struct cmdbuf loopbuf = {0,0,0};
						c = compileblock(c + 1, end, ids, &loopbuf, BT_LOOP);
						if (!c) {
							cmdbuf_clear(&loopbuf);
							return 0;
						}
						if (cmdbuf_appendcmd(cbuf, CMD_REPNZ, loopbuf.len) < 0) {
							cmdbuf_clear(&loopbuf);
							return 0;
						}
						if (cmdbuf_appendcmdbuf(cbuf, &loopbuf) < 0) {
							cmdbuf_clear(&loopbuf);
							return 0;
						}
					}
					break;
				case ']':
					if (blocktype == BT_LOOP) {
						++c;
						return c;
					} else {
						fprintf(stderr, "close bracket outside loop\n");
						return 0;
					}
					break;
				default: /* other characters are ignored */
					++c;
					break;
			}
		}
	}
	assert(c == end);
	if (blocktype == BT_LOOP) {
		fprintf(stderr, "unclosed loop\n");
		return 0;
	}
	if (blocktype == BT_FUNC) {
		if (cmdbuf_appendcmd(cbuf, CMD_RET, 0) < 0)
			return 0;
	}
	return end;
}

int compile(const char *buf, size_t len, struct cmdbuf *cbuf) {
	size_t i;
	struct idtable ids = {0,0,0};
	/* compile */
	const char *end = compileblock(buf, buf + len, &ids, cbuf, BT_MAIN);
	if (! end) {
		idtable_clear(&ids);
		return -1;
	}
#if 0
	for (i = 0; i < ids.len; ++i) {
		const struct idtablerec *rec = ids.recs + i;
		printf("'%.*s' starts at cmd %d\n", (int)(rec->end - rec->start), rec->start, (int)rec->value);
	}
#endif
	/* fixup calls */
	for (i = 0; i < cbuf->len; ++i) {
		struct cmd *cmd = &cbuf->commands[i];
		if (cmd->cmd == CMD_CALL) {
			cmd->value = ids.recs[cmd->value].value;
		}
	}
	idtable_clear(&ids);
	return 0;
}

struct state {
	int *cells;
	size_t len;
	size_t pos;
};

#if DUMP_EXECUTION
#define exec_fprintf fprintf
#else
#define exec_fprintf swallow_fprintf
void swallow_fprintf(FILE *fl, const char *fmt, ...) {
	(void)fl;  /* unused */
	(void)fmt; /* unused */
}
#endif

int run(struct cmdbuf *cbuf, struct state *state, size_t from, size_t to, int recursion) {
	size_t i, pos = state->pos;
	int *cells = state->cells;
	if (recursion > RECURSION_LIMIT) {
		fprintf(stderr, "recursion limit reached\n");
		return -1;
	}
	for (i = from; (i < cbuf->len) && (i != to); ++i) {
		int cmd = cbuf->commands[i].cmd;
		int cmdarg = cbuf->commands[i].value;
		assert(cmdarg >= 0);
		switch (cmd) {
			case CMD_RET:
				exec_fprintf(stdout, "exec ret %d\n", cmdarg);
				if (to != (size_t)(-1)) {
					fprintf(stderr, "ret instruction inside loop\n");
					return -1;
				}
				return 0;
			case CMD_IN:
				exec_fprintf(stdout, "exec in %d\n", cmdarg);
				while (cmdarg) {
					cells[pos] = getchar();
					--cmdarg;
				}
				break;
			case CMD_OUT:
				exec_fprintf(stdout, "exec out %d\n", cmdarg);
				while (cmdarg) {
					putchar(cells[pos]);
					--cmdarg;
				}
				break;
			case CMD_LEFT:
				exec_fprintf(stdout, "exec left %d\n", cmdarg);
				if (pos < (size_t)cmdarg) {
					fprintf(stderr, "ran off the left end of the tape\n");
					return -1;
				}
				pos -= cmdarg;
				break;
			case CMD_RIGHT:
				exec_fprintf(stdout, "exec right %d\n", cmdarg);
				if (pos > state->len - (size_t)cmdarg) {
					fprintf(stderr, "ran off the right end of the tape\n");
					return -1;
				}
				pos += cmdarg;
				break;
			case CMD_INC:
				exec_fprintf(stdout, "exec inc %d\n", cmdarg);
				if (cells[pos] > INT_MAX - cmdarg) {
					fprintf(stderr, "positive overflow\n");
					return -1;
				}
				cells[pos] += cmdarg;
				break;
			case CMD_DEC:
				exec_fprintf(stdout, "exec dec %d\n", cmdarg);
				if (cells[pos] < INT_MIN + cmdarg) {
					fprintf(stderr, "negative overflow\n");
					return -1;
				}
				cells[pos] -= cmdarg;
				break;
			case CMD_REPNZ:
				exec_fprintf(stdout, "exec repnz %d\n", cmdarg);
				if (cells[pos]) {
					state->pos = pos;
					if (run(cbuf, state, i+1, i+1+cmdarg, recursion+1) < 0) {
						return -1;
					}
					pos = state->pos;
					/* repeat this */
					--i;
				} else {
					/* skip the loop body */
					i += cmdarg;
				}
				break;
			case CMD_CALL:
				exec_fprintf(stdout, "exec call %d\n", cmdarg);
				state->pos = pos;
				if (run(cbuf, state, (size_t)cmdarg, (size_t)(-1), recursion+1) < 0) {
					return -1;
				}
				pos = state->pos;
				break;
		}
	}
	state->pos = pos;
	return 0;
}

int dumpbin(struct cmdbuf *cbuf) {
	size_t i;
	for (i = 0; i < cbuf->len; ++i) {
		printf("[%4d]  ", (int)i);
		print_cmd(stdout, &cbuf->commands[i]);
		printf("\n");
	}
	return 0;
}

int runsource(const char *buf, size_t len) {
	int ret;
	struct cmdbuf cbuf = {0,0,0};
	struct state state = {0,0,0};
	if (compile(buf, len, &cbuf) < 0) {
		cmdbuf_clear(&cbuf);
		return -1;
	}

#if DUMP_BIN
	if (dumpbin(&cbuf) < 0) {
		cmdbuf_clear(&cbuf);
		return -1;
	}
#endif

	state.len = STATE_SIZE;
	state.cells = calloc(STATE_SIZE, sizeof(state.cells[0]));
	state.pos = state.len / 2;
	ret = run(&cbuf, &state, 0, (size_t)(-1), 0);
	free(state.cells);
	return ret;
}

int runstream(FILE *fl) {
	int ret;
	char *buf;
	size_t len;
	if (readstream(fl, &buf, &len, 0) < 0)
		return -1;
	ret = runsource(buf, len);
	free(buf);
	return ret;
}

int runfile(const char *path) {
	int ret;
	char *buf;
	size_t len;
	if (readfile(path, &buf, &len, 0) < 0)
		return -1;
	ret = runsource(buf, len);
	return ret;
}

int main(int argc, char **argv) {
	int ret = 0;
	if (argc > 1) {
		char *path = argv[1];
		if (!*path) {
			fprintf(stderr, "bad command line argument (empty)\n");
			return 1;
		}
		if (path[0] == '-') {
			if (path[1] != '\0') {
				fprintf(stderr, "unknown command line option '%s'\n", path);
				return 1;
			} else {
				ret = runstream(stdin);
			}
		} else
			ret = runfile(path);
	} else
		ret = runstream(stdin);
	return (ret == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
