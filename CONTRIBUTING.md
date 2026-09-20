# Contributing

Bug reports and focused pull requests are welcome. When reporting a firmware
failure, describe the board, flash size, ESP-IDF version, and the smallest
repeatable test case.

For code changes:

1. Create a branch from `main`.
2. Clone the content repository beside this one, so the catalogue tests and the
   firmware build can find the article text:
   `git clone https://github.com/bilawalriaz/pocketwiki-content.git`
3. Install the Python tools: `python3 -m pip install -r tools/requirements.txt`.
4. Keep generated firmware, archives, corpora, `sdkconfig`, and managed
   components out of the commit.
5. Run `python3 -m pytest -q`.
6. Build with ESP-IDF when the change touches `firmware/`, CMake, Kconfig, or
   partition data.
7. Preview frontend changes with `tools/reference_server.py` at desktop and
   phone widths.
8. Explain the observable change and the checks you ran in the pull request.

Archive parsing and HTML sanitization are trust boundaries. Changes there need
tests for malformed input, size limits, and failure behaviour. Do not weaken
validation to accept one troublesome corpus file; fix the producer or reject
the file with a useful error.

Corrections to article text belong in the
[pocketwiki-content](https://github.com/bilawalriaz/pocketwiki-content)
repository, not here.

By contributing, you agree that your contribution is licensed under the MIT
License used by this repository.
