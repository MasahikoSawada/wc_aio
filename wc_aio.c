#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <liburing.h>

#define QD 16
#define BS (16 * 1024)

struct file
{
	int	fd;
	char *filename;
	int size;
	int read_offset;
	int count_left;

	bool in_word;
	int nlines;
	int nbytes;
	int nwords;
};

struct io_data
{
	int		idx;
	off_t	offset;
	size_t	len;
	size_t	first_len;
	struct iovec iov;
	char buf[];
};

static struct io_uring ring;
static int nreqs = 0;	/* number of request in cqe */
static char	*workbuf;

static int total_nlines = 0;
static int total_nbytes = 0;
static int total_nwords = 0;

static void
count(struct file *f, const char *buf, int len)
{
	int nl = 0;
	int nw = 0;

	for (int i = 0; i < len; i++)
	{
		if (buf[i] == '\n')
			nl++;

		if (f->in_word && !isalpha(buf[i]))
			f->in_word = false;
		else if (!f->in_word && isalpha(buf[i]))
		{
			f->in_word = true;
			nw++;
		}
	}

	f->nlines += nl;
	f->nwords += nw;
	f->nbytes += len;
	f->count_left -= len;

	total_nlines += nl;
	total_nbytes += len;
	total_nwords += nw;
}

static int
get_file_size(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return -1;
	if (!S_ISREG(st.st_mode))
		return -1;

	return st.st_size;
}

static int
queue_read_io(struct file *f, size_t size, int idx)
{
    struct io_uring_sqe *sqe;
    struct io_data *data;

	data = (struct io_data *)
		((char *) workbuf + ((sizeof(struct io_data) + BS) * nreqs));

	sqe = io_uring_get_sqe(&ring);
	if (!sqe)
		return 1;

	data->idx = idx;
	data->offset = f->read_offset;
	data->iov.iov_base = &data->buf;
	data->iov.iov_len = size;
	data->first_len = size;

	io_uring_prep_readv(sqe, f->fd, &data->iov, 1, f->read_offset);
	io_uring_sqe_set_data(sqe, data);

	f->read_offset += size;

	return 0;
}

static void
wc(struct file *files, int nfiles)
{
	struct io_uring_cqe *cqe = NULL;
	int		nprocessed;

	nprocessed = 0;
	while (nprocessed != nfiles)
	{
		bool first_time = true;

		for (int i = 0; i < nfiles; i++)
		{
			struct file *f = &files[i];

			while (nreqs < QD)
			{
				off_t this_size = f->size > 0
					? (f->size - f->read_offset)
					: BS;

				if (this_size > BS)
					this_size = BS;

				if (this_size <= 0)
					break;

				if (queue_read_io(f, this_size, i))
					break;

				nreqs++;

				if (f->size > 0 && f->size == f->read_offset)
					break;
			}

			if (nreqs >= QD)
				break;
		}

		if (io_uring_submit(&ring) < 0)
		{
			perror("io_uring_submit");
			break;
		}

		while (nreqs > 0)
		{
			struct io_data *data;
			int ret;

			if (first_time)
			{
				ret = io_uring_wait_cqe(&ring, &cqe);
				first_time = false;
			}
			else
			{
				ret = io_uring_peek_cqe(&ring, &cqe);
				if (ret == -EAGAIN)
				{
					cqe = NULL;
					ret = 0;
				}
			}

            if (ret < 0) {
                fprintf(stderr, "io_uring_peek_cqe: %s\n", strerror(-ret));
                return;
            }

			if (!cqe)
				break;

			data = io_uring_cqe_get_data(cqe);
			if (cqe->res < 0)
			{
				fprintf(stderr, "io_uring_cqe_get_data: %s\n", strerror(-cqe->res));
				return;
			}

			nreqs--;

			if (cqe->res != data->iov.iov_len)
			{
				struct io_uring_sqe *sqe;


				if (files[data->idx].size == -1)
				{
					assert(nfiles == 1 && files[0].size == -1);
					nprocessed++;
					break;
				}


				sqe = io_uring_get_sqe(&ring);
				assert(sqe);

				data->iov.iov_base += cqe->res;
				data->iov.iov_len -= cqe->res;
				data->offset += cqe->res;
				io_uring_prep_readv(sqe, files[data->idx].fd,
									&data->iov, 1, data->offset);
				io_uring_sqe_set_data(sqe, data);
				io_uring_cqe_seen(&ring, cqe);
				io_uring_submit(&ring);
				nreqs++;

				continue;
			}

			count(&files[data->idx], data->buf, data->first_len);

			/* Have we processed all buffers? */
			if (files[data->idx].count_left == 0)
				nprocessed++;

			io_uring_cqe_seen(&ring, cqe);
		}
	}

	/* Print result */
	for (int i = 0; i < nfiles; i++)
	{
		struct file *f = &files[i];
		if (f->filename)
			printf(" %4d %4d %4d %s\n", f->nlines, f->nwords, f->nbytes, f->filename);
		else
			printf(" %4d %4d %4d\n", f->nlines, f->nwords, f->nbytes);
	}

	if (nfiles > 1)
		printf(" %4d %4d %4d total\n", total_nlines, total_nwords, total_nbytes);
}

int
main(int argc, char **argv)
{
	struct file *files;
	int nfiles = 0;

	if (argc == 1)
	{
		/* input from stdin */
		files = malloc(sizeof(struct file));
		nfiles = 1;

		memset(&files[0], 0, sizeof(struct file));
		files[0].fd = STDIN_FILENO;
		files[0].size = -1; /* unknown file size */
	}
	else
	{
		/* one or more files are specified */

		files = malloc(sizeof(struct file) * (argc - 1));
		nfiles = argc - 1;
		for (int i = 0; i < nfiles; i++)
		{
			struct file *f = &files[i];
			memset(f, 0, sizeof(struct file));

			if ((f->fd = open(argv[1 + i], O_RDONLY)) < 0)
			{
				perror("open input file");
				return 1;
			}
			if ((f->size = get_file_size(f->fd)) < 0)
			{
				perror("could not get file size");
				return 1;
			}
			f->filename = strdup(argv[1 + i]);
			f->count_left = f->size;

			if (posix_fadvise(f->fd, 0, 0, POSIX_FADV_SEQUENTIAL) < 0)
			{
				perror("failed posix_fadvise");
				return 1;
			}
		}
	}

	if (io_uring_queue_init(QD, &ring, 0) < 0)
	{
		perror("queue_init error");
		return 1;
	}

	workbuf = malloc((sizeof(struct io_data) + BS) * QD);
	memset(workbuf, 0, (sizeof(struct io_data) + BS) * QD);
	wc(files, nfiles);

	for (int i = 0; i < nfiles; i++)
		close(files[i].fd);

	free(files);
	free(workbuf);
	io_uring_queue_exit(&ring);
	return 0;
}
