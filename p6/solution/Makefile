BINS = wfs mkfs

CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`

LOGIN = santhanakrishnan
SUBMITPATH = ~cs537-1/handin/$(LOGIN)

.PHONY: all clean test submit

all: $(BINS)

wfs: wfs.c
	$(CC) $(CFLAGS) wfs.c $(FUSE_CFLAGS) -o wfs
	@echo "[INFO] Built wfs successfully."

# Build mkfs binary
mkfs: mkfs.c
	$(CC) $(CFLAGS) mkfs.c -o mkfs
	@echo "[INFO] Built mkfs successfully."

# Clean up binaries
clean:
	rm -f $(BINS)
	@echo "[INFO] Cleaned up binaries."

submit:
	@echo "[INFO] Submitting p6 directory to $(SUBMITPATH)..."
	cd ../.. && cp -r p6 $(SUBMITPATH)
	@echo "Project submitted by $(LOGIN) to $(SUBMITPATH)"

test: clean all
	@echo "[INFO] Running tests..."
	cd ../tests && ./run-tests.sh  -v
	@echo "[INFO] Tests completed."
