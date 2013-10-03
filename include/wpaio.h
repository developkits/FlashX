#ifndef _WPAIO_H_
#define _WPAIO_H_

# ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>
#include <stdlib.h>


#include <libaio.h>

#define A_READ 0
#define A_WRITE 1

struct free_list_s;

class aio_ctx
{
	int max_aio;
	int busy_aio;
	io_context_t ctx;
	struct free_list_s* free_list;

	aio_ctx() {
		max_aio = 0;
		busy_aio = 0;
		ctx = 0;
		free_list = NULL;
	}

	struct iocb* get_iocb();
	void put_iocb(struct iocb* io);

public:
	static aio_ctx* create_aio_ctx(int max_aio);
	static void destroy_aio_ctx(aio_ctx *);

	virtual struct iocb* make_io_request(int fd, size_t iosize, long long offset,
			void* buffer, int io_type, struct io_callback_s *cb);
	virtual struct iocb *make_iovec_request(int fd, const struct iovec iov[],
			int count, long long offset, int io_type, struct io_callback_s *cb);
	virtual void submit_io_request(struct iocb* ioq[], int num);
	virtual int io_wait(struct timespec* to, int num);
	virtual int max_io_slot();
};

typedef void (*callback_t) (io_context_t, struct iocb*[],
		void *[], long *, long *, int);

struct io_callback_s
{
	callback_t func;
};

#endif
