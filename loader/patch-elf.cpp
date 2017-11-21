#define LOAD_ELF_VERBOSITY 0
#include "loader.cpp"

static void
fwrite_all(FILE *fp, const void *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        written += fwrite((char *)buf + written, 1, len - written, fp);
        if (ferror(fp))
            panic("file write: %s\n", strerror(errno));
    }
}

int
main(int argc, char **argv) {
    if (argc != 3)
        panic("usage: patch-elf [input] [output]");
    char *input_filename = argv[1];
    char *output_filename = argv[2];
    FILE *fp = fopen(input_filename, "rb");
    if (!fp)
        panic("fopen(\"%s\", \"rb\"): %s\n", input_filename, strerror(errno));
    size_t buf_len = 0;
    char *buf = NULL;
    while (!feof(fp)) {
        buf = (char *)realloc(buf, buf_len + 32768);
        buf_len += fread(buf + buf_len, 1, 32768, fp);
        if (ferror(fp))
            panic("file read: %s\n", strerror(errno));
    }
    fclose(fp);

    // Update text for local relocs
    struct load_elf_info in;
    in.in_buf = buf;
    in.in_bufsize = buf_len;
    in.flags = LOAD_ELF_DO_LOCAL_RELOCS;
    load_elf(&in);

    fp = fopen(output_filename, "wb");
    if (!fp)
        panic("fopen(\"%s\", \"wb\"): %s\n", output_filename, strerror(errno));
    fwrite_all(fp, buf, buf_len);
    fclose(fp);
}
