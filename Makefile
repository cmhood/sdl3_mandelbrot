.POSIX:

mandelbrot: mandelbrot.o
	$(CC) -o $@ $< $(LDFLAGS) `pkg-config --libs sdl3 gl` -lm

.c.o:
	$(CC) -c -o $@ $< $(CFLAGS) `pkg-config --cflags sdl3 gl`

.PHONY: clean
clean:
	rm -f mandelbrot.o mandelbrot
