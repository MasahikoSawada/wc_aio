# wc_aio
wc command using [io_uring](https://github.com/axboe/liburing)

# Usage

```bash
$ gcc -Wall -O2 -o wc_aio wc_aio.c -luring
$ ./wc_aio wc_aio.c README.md
  291  723 5082 wc_aio.c
    8   21  110 README.md
  299  744 5192 total
```