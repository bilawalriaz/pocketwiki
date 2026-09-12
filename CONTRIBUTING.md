# Contributing

Bug reports and focused pull requests are welcome. Please describe the board,
flash size, ESP-IDF version, and the smallest repeatable test case when reporting
firmware failures.

For code changes:

1. Create a branch from `main`.
2. Keep generated firmware, archives, corpora, `sdkconfig`, and managed
   components out of the commit.
3. Run `python3 -m pytest -q` and, when you touch article generation,
   `python3 tools/finalize_articles.py --check`.
4. Build with ESP-IDF when the change touches `firmware/`, CMake, Kconfig, or
   partition data.
5. Preview frontend changes with `tools/reference_server.py` at desktop and
   phone widths.
6. Explain the observable change and the checks you ran in the pull request.

Archive parsing and HTML sanitization are trust boundaries. Changes there need
tests for malformed input, size limits, and failure behaviour. Do not weaken
validation to accept one troublesome corpus file; fix the producer or reject
the file with a useful error.

By contributing, you agree that your contribution is licensed under the Apache
License 2.0 used by this repository. See LICENSE for the code license and
NOTICE for third-party terms. Article text is not part of this repository: it is
CC BY-SA 4.0 and lives in the companion pocketwiki-content repository.
