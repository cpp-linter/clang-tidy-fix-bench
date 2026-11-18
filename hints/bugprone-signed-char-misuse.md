Docs: https://clang.llvm.org/extra/clang-tidy/checks/bugprone/signed-char-misuse.html
A `char`/`signed char` value is converted to a wider integer, compared with an
unsigned char, or used as an index. Bytes 0x80-0xFF become negative numbers by
sign extension, so comparisons and lookups go wrong.
- Convert through `unsigned char` first: `static_cast<unsigned char>(c)`
  (in C: `(unsigned char)c`), or store the byte as `unsigned char`/`uint8_t`.
- When comparing with `EOF`, keep the `int` returned by `getc`/`fgetc` instead of
  storing it in a `char` first.
